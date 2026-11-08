/*-------------------------------------------------------------------------
 * graph_stats_collect.c
 *
 * Explicit, bounded single-hop graph statistics collection.  This is not an
 * ANALYZE hook and is never called by the planner.  It scans visible heap
 * tuples with one borrowed active snapshot, then atomically publishes through
 * the independent graph-statistics catalog.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/detoast.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_graph_statistic.h"
#include "catalog/pg_propgraph_element.h"
#include "catalog/pg_type_d.h"
#include "common/pg_prng.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "statistics/graph_stats.h"
#include "storage/bufmgr.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/jsonb.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/wait_event.h"

#define COLLECT_OPTION_FIELDS 15
#define COLLECT_MAX_STRATA 16
#define COLLECT_MAX_HEAVY 16

typedef struct CollectOptions
{
	bool		full;
	uint64		seed;
	uint32		target;
	uint32		strata;
	uint32		heavy_limit;
	uint64		max_scan_pages;
	uint64		max_examined;
	uint64		max_bindings;
	uint64		max_entries_per_seed;
	Size		max_memory;
	Size		max_payload;
	uint32		max_profiles;
	uint32		deadline_ms;
	uint32		lock_timeout_ms;
	uint32		throttle_delay_ms;
} CollectOptions;

typedef struct VertexSample
{
	int64		key;
	uint64		degree;
	uint32		stratum;
	bool		censored;
} VertexSample;

typedef struct SampleStratum
{
	VertexSample *items;
	uint64		population;
	uint32		capacity;
	uint32		count;
	pg_prng_state random;
} SampleStratum;

typedef struct HeavyCandidate
{
	int64		key;
	uint64		discovery_count;
	uint64		discovery_error;
	uint64		exact_degree;
	bool		used;
} HeavyCandidate;

typedef struct PairKey
{
	int64		source;
	int64		destination;
} PairKey;

typedef struct PairEntry
{
	PairKey		key;
	uint64		count;
} PairEntry;

typedef struct WeightedDegree
{
	uint64		degree;
	double		weight;
} WeightedDegree;

typedef struct GraphMapping
{
	Oid			graph_relid;
	Oid			vertex_relid;
	Oid			edge_relid;
	AttrNumber	vertex_key;
	AttrNumber	source_key;
	AttrNumber	destination_key;
} GraphMapping;

PG_FUNCTION_INFO_V1(pg_graph_statistic_collect);

static JsonbValue *
collect_field(Jsonb *object, const char *name)
{
	JsonbValue key;
	JsonbValue *value;

	key.type = jbvString;
	key.val.string.val = (char *) name;
	key.val.string.len = strlen(name);
	value = findJsonbValueFromContainer(&object->root, JB_FOBJECT, &key);
	if (!value)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("missing graph collection option \"%s\"", name)));
	return value;
}

static uint64
collect_uint(Jsonb *object, const char *name, uint64 maximum)
{
	JsonbValue *value = collect_field(object, name);
	int64		number;
	Numeric		integral;

	if (value->type != jbvNumeric)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("graph collection option \"%s\" must be an integer", name)));
	number = DatumGetInt64(DirectFunctionCall1(numeric_int8,
											 NumericGetDatum(value->val.numeric)));
	integral = DatumGetNumeric(DirectFunctionCall1(int8_numeric, Int64GetDatum(number)));
	if (number < 0 || (uint64) number > maximum ||
		DatumGetInt32(DirectFunctionCall2(numeric_cmp,
									 NumericGetDatum(value->val.numeric),
									 NumericGetDatum(integral))) != 0)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("graph collection option \"%s\" is out of range", name)));
	return number;
}

static void
parse_options(Jsonb *object, CollectOptions *o)
{
	JsonbValue *mode;

	if (!JB_ROOT_IS_OBJECT(object) || JB_ROOT_COUNT(object) != COLLECT_OPTION_FIELDS)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("graph collection requires exactly its versioned options")));
	mode = collect_field(object, "mode");
	if (mode->type != jbvString)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("graph collection mode must be a string")));
	if (mode->val.string.len == 4 && memcmp(mode->val.string.val, "full", 4) == 0)
		o->full = true;
	else if (mode->val.string.len == 6 && memcmp(mode->val.string.val, "sample", 6) == 0)
		o->full = false;
	else
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("graph collection mode must be full or sample")));
	o->seed = collect_uint(object, "seed", PG_INT64_MAX);
	o->target = collect_uint(object, "vertex_sample_target", 65536);
	o->strata = collect_uint(object, "strata", COLLECT_MAX_STRATA);
	o->heavy_limit = collect_uint(object, "heavy_hitter_limit", COLLECT_MAX_HEAVY);
	o->max_scan_pages = collect_uint(object, "max_scan_pages", PG_INT64_MAX);
	o->max_examined = collect_uint(object, "max_examined_entries", PG_INT64_MAX);
	o->max_bindings = collect_uint(object, "max_bindings", PG_INT64_MAX);
	o->max_entries_per_seed = collect_uint(object, "max_entries_per_seed", PG_INT64_MAX);
	o->max_memory = collect_uint(object, "max_memory_bytes", MaxAllocSize);
	o->max_payload = collect_uint(object, "max_payload_bytes", GGC_STATS_MAX_PAYLOAD_BYTES);
	o->max_profiles = collect_uint(object, "max_profiles", GGC_STATS_MAX_PROFILES);
	o->deadline_ms = collect_uint(object, "deadline_ms", PG_UINT32_MAX);
	o->lock_timeout_ms = collect_uint(object, "lock_timeout_ms", PG_UINT32_MAX);
	o->throttle_delay_ms = collect_uint(object, "throttle_delay_ms", 1000);
	if (o->target == 0 || o->strata == 0 || o->target < o->strata ||
		o->heavy_limit == 0 || o->max_scan_pages == 0 || o->max_examined == 0 ||
		o->max_bindings < o->target || o->max_entries_per_seed == 0 ||
		o->max_memory < 64 * 1024 || o->max_payload == 0 ||
		o->max_profiles == 0 || o->deadline_ms == 0 || o->lock_timeout_ms == 0)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("graph collection budgets must be finite and positive")));
}

static int16
single_int2_array(HeapTuple tuple, TupleDesc desc, AttrNumber attnum,
				  const char *description)
{
	bool		isnull;
	Datum		value = heap_getattr(tuple, attnum, desc, &isnull);
	ArrayType  *array;
	Datum	   *values;
	bool	   *nulls;
	int			count;

	if (isnull)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("%s mapping is missing", description)));
	array = DatumGetArrayTypeP(value);
	deconstruct_array(array, INT2OID, sizeof(int16), true, TYPALIGN_SHORT,
					  &values, &nulls, &count);
	if (count != 1 || nulls[0])
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("%s mapping must contain one column", description)));
	return DatumGetInt16(values[0]);
}

static void
load_scope_and_mapping(Oid stats_oid, GgcStatsScope *scope, GraphMapping *mapping)
{
	Relation	head;
	HeapTuple	stats;
	Form_pg_graph_statistic form;
	bool		isnull;
	Datum		encoded;
	HeapTuple	edge;
	HeapTuple	vertex;
	Form_pg_propgraph_element edge_form;
	Form_pg_propgraph_element vertex_form;
	Relation	maprel;
	Oid			vertex_element;

	head = table_open(GraphStatisticRelationId, AccessShareLock);
	stats = SearchSysCache1(GRAPHSTATISTICOID, ObjectIdGetDatum(stats_oid));
	if (!HeapTupleIsValid(stats))
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
						errmsg("graph statistics object %u does not exist", stats_oid)));
	form = (Form_pg_graph_statistic) GETSTRUCT(stats);
	if (form->gstowner != GetUserId())
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						errmsg("graph statistics collector must own its scope")));
	encoded = heap_getattr(stats, Anum_pg_graph_statistic_gstscope,
					   RelationGetDescr(head), &isnull);
	if (isnull || toast_raw_datum_size(encoded) > 84)
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
						errmsg("graph statistics scope is missing or oversized")));
	graph_stats_scope_decode(DatumGetByteaPP(encoded), scope);
	ReleaseSysCache(stats);
	table_close(head, AccessShareLock);
	if (scope->scope_kind != GGC_STATS_LOGICAL || scope->catalog_provider_oid ||
		scope->effective_role != GetUserId())
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("single-hop collection requires an owned native logical scope")));

	edge = SearchSysCache1(PROPGRAPHELOID, ObjectIdGetDatum(scope->edge_element_oid));
	if (!HeapTupleIsValid(edge))
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						errmsg("graph edge mapping no longer exists")));
	edge_form = (Form_pg_propgraph_element) GETSTRUCT(edge);
	if (edge_form->pgepgid != scope->graph_oid || edge_form->pgekind != PGEKIND_EDGE)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("graph statistics edge mapping differs from its scope")));
	mapping->graph_relid = scope->graph_oid;
	mapping->edge_relid = edge_form->pgerelid;
	vertex_element = scope->direction == 1 ? edge_form->pgesrcvertexid :
		edge_form->pgedestvertexid;
	maprel = table_open(PropgraphElementRelationId, AccessShareLock);
	mapping->source_key = single_int2_array(edge, RelationGetDescr(maprel),
		Anum_pg_propgraph_element_pgesrckey, "edge source key");
	mapping->destination_key = single_int2_array(edge, RelationGetDescr(maprel),
		Anum_pg_propgraph_element_pgedestkey, "edge destination key");
	mapping->vertex_key = single_int2_array(edge, RelationGetDescr(maprel),
		scope->direction == 1 ? Anum_pg_propgraph_element_pgesrcref :
		Anum_pg_propgraph_element_pgedestref, "edge referenced vertex key");
	ReleaseSysCache(edge);
	vertex = SearchSysCache1(PROPGRAPHELOID, ObjectIdGetDatum(vertex_element));
	if (!HeapTupleIsValid(vertex))
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						errmsg("graph vertex mapping no longer exists")));
	vertex_form = (Form_pg_propgraph_element) GETSTRUCT(vertex);
	if (vertex_form->pgepgid != scope->graph_oid || vertex_form->pgekind != PGEKIND_VERTEX ||
		single_int2_array(vertex, RelationGetDescr(maprel),
						  Anum_pg_propgraph_element_pgekey,
						  "vertex key") != mapping->vertex_key)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("single-hop collection requires one matching vertex key")));
	mapping->vertex_relid = vertex_form->pgerelid;
	ReleaseSysCache(vertex);
	table_close(maprel, AccessShareLock);
}

static void
validate_relation(Relation relation, const AttrNumber *keys, int nkeys)
{
	TupleDesc	desc = RelationGetDescr(relation);

	if (relation->rd_rel->relkind != RELKIND_RELATION ||
		relation->rd_rel->relam != HEAP_TABLE_AM_OID ||
		relation->rd_rel->relpersistence != RELPERSISTENCE_PERMANENT ||
		relation->rd_rel->relrowsecurity || RELATION_IS_OTHER_TEMP(relation))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("single-hop collection supports ordinary logged heaps without RLS")));
	if (pg_class_aclcheck(RelationGetRelid(relation), GetUserId(), ACL_SELECT) != ACLCHECK_OK)
		aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLE, RelationGetRelationName(relation));
	for (int i = 0; i < nkeys; i++)
	{
		Form_pg_attribute attribute;

		if (keys[i] <= 0 || keys[i] > desc->natts)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							errmsg("graph key column is out of range")));
		attribute = TupleDescAttr(desc, keys[i] - 1);
		if (attribute->attisdropped || attribute->atttypid != INT8OID ||
			!attribute->attnotnull)
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("single-hop collection requires bigint NOT NULL keys")));
	}
}

static int
sample_key_compare(const void *a, const void *b)
{
	const VertexSample *left = a;
	const VertexSample *right = b;

	return left->key < right->key ? -1 : left->key > right->key;
}

static int
degree_compare(const void *a, const void *b)
{
	const WeightedDegree *left = a;
	const WeightedDegree *right = b;

	return left->degree < right->degree ? -1 : left->degree > right->degree;
}

static VertexSample *
sample_lookup(VertexSample *samples, uint32 count, int64 key)
{
	VertexSample probe = {.key = key};

	return bsearch(&probe, samples, count, sizeof(*samples), sample_key_compare);
}

static void
heavy_observe(HeavyCandidate *items, uint32 count, int64 key)
{
	int			empty = -1;
	int			minimum = 0;

	for (uint32 i = 0; i < count; i++)
	{
		if (items[i].used && items[i].key == key)
		{
			items[i].discovery_count++;
			return;
		}
		if (!items[i].used && empty < 0)
			empty = i;
		if (items[i].discovery_count < items[minimum].discovery_count)
			minimum = i;
	}
	if (empty >= 0)
	{
		items[empty].used = true;
		items[empty].key = key;
		items[empty].discovery_count = 1;
		return;
	}
	items[minimum].key = key;
	items[minimum].discovery_error = items[minimum].discovery_count;
	items[minimum].discovery_count++;
}

static bool
collect_checkpoint(const CollectOptions *o, TimestampTz started, uint64 examined)
{
	static uint32 wait_event;

	CHECK_FOR_INTERRUPTS();
	if (TimestampDifferenceExceeds(started, GetCurrentTimestamp(), o->deadline_ms))
		return false;
	if (o->throttle_delay_ms && examined && examined % 256 == 0)
	{
		if (!wait_event)
			wait_event = WaitEventExtensionNew("GraphStatsThrottle");
		pgstat_report_wait_start(wait_event);
		pg_usleep((long) o->throttle_delay_ms * 1000L);
		pgstat_report_wait_end();
		CHECK_FOR_INTERRUPTS();
		if (TimestampDifferenceExceeds(started, GetCurrentTimestamp(), o->deadline_ms))
			return false;
	}
	return true;
}

static bool
collect_lock_relation(Oid relid, TimestampTz started, uint32 timeout_ms)
{
	for (;;)
	{
		CHECK_FOR_INTERRUPTS();
		if (ConditionalLockRelationOid(relid, AccessShareLock))
			return true;
		if (TimestampDifferenceExceeds(started, GetCurrentTimestamp(), timeout_ms))
			return false;
		pg_usleep(1000L);
	}
}

static Jsonb *
status_json(const char *status, const char *reason, int64 generation,
			uint64 scan_pages, uint64 examined, Size peak)
{
	char	   *text = psprintf("{\"status\":\"%s\",\"published\":%s,"
		"\"reason\":\"%s\",\"generation\":" INT64_FORMAT ","
		"\"resources\":{\"scan_pages\":" UINT64_FORMAT
		",\"examined_entries\":" UINT64_FORMAT ",\"peak_bytes\":%zu}}",
		status, strcmp(status, "READY") == 0 ? "true" : "false", reason,
		generation, scan_pages, examined, peak);

	return DatumGetJsonbP(DirectFunctionCall1(jsonb_in, CStringGetDatum(text)));
}

static void
append_profile(StringInfo profiles, int *count, int metric_id,
			   const char *population, const char *metric, const char *extra_key,
			   const char *metadata, GgcMetricKind kind, GgcStatsState state,
			   double value, uint64 sample_count, bool censored)
{
	appendStringInfo(profiles,
		"%s{\"metric_kind\":%d,\"profile_key\":{\"population\":\"%s\","
		"\"metric\":\"%s\"%s},\"metadata\":%s,\"estimate\":{"
		"\"kind\":%d,\"state\":%d,\"sample_count\":" UINT64_FORMAT ","
		"\"p95_known\":false,\"uncertainty_known\":false,\"censored\":%s,"
		"\"expected\":%s%s,\"workload_p95\":null,"
		"\"uncertainty_low\":null,\"uncertainty_high\":null}}",
		*count ? "," : "", metric_id, population, metric, extra_key,
		metadata, kind, state, sample_count, censored ? "true" : "false",
		kind == GGC_METRIC_LOWER_BOUND || kind == GGC_METRIC_UNKNOWN ? "null" :
			psprintf("%.17g", value),
		kind == GGC_METRIC_LOWER_BOUND ? psprintf(",\"lower_bound\":%.17g", value) : "");
	(*count)++;
}

Datum
pg_graph_statistic_collect(PG_FUNCTION_ARGS)
{
	Oid			stats_oid = PG_GETARG_OID(0);
	CollectOptions options;
	GgcStatsScope scope;
	GraphMapping mapping = {0};
	MemoryContext collect_context;
	MemoryContext old_context;
	Relation	vertex = NULL;
	Relation	edge = NULL;
	TableScanDesc scan = NULL;
	TupleTableSlot *slot = NULL;
	SampleStratum *strata;
	VertexSample *samples;
	HeavyCandidate *heavy;
	HTAB	   *pairs = NULL;
	uint64		examined = 0;
	uint64		scan_pages;
	uint64		visible_edges = 0;
	uint64		self_loops = 0;
	uint64		parallel_extra = 0;
	uint64		population = 0;
	uint32		sample_count = 0;
	Size		peak = 0;
	TimestampTz started = GetCurrentTimestamp();
	TimestampTz lock_started;
	const char *partial_reason = NULL;
	Jsonb	   *return_value;
	AttrNumber vertex_keys[1];
	AttrNumber edge_keys[2];
	Oid			lock_oids[3];
	char	   *counter_baseline;

	parse_options(PG_GETARG_JSONB_P(1), &options);
	load_scope_and_mapping(stats_oid, &scope, &mapping);
	lock_oids[0] = mapping.graph_relid;
	lock_oids[1] = mapping.vertex_relid;
	lock_oids[2] = mapping.edge_relid;
	for (int i = 0; i < 3; i++)
		for (int j = i + 1; j < 3; j++)
			if (lock_oids[j] < lock_oids[i])
			{
				Oid swap = lock_oids[i];

				lock_oids[i] = lock_oids[j];
				lock_oids[j] = swap;
			}
	lock_started = GetCurrentTimestamp();
	for (int i = 0; i < 3; i++)
	{
		if (i && lock_oids[i] == lock_oids[i - 1])
			continue;
		if (!collect_lock_relation(lock_oids[i], lock_started,
								   options.lock_timeout_ms))
			PG_RETURN_JSONB_P(status_json("PARTIAL", "lock_unavailable", 0, 0, 0, 0));
	}
	graph_stats_check_collect_privileges(&scope);
	vertex = table_open(mapping.vertex_relid, NoLock);
	edge = table_open(mapping.edge_relid, NoLock);
	vertex_keys[0] = mapping.vertex_key;
	edge_keys[0] = mapping.source_key;
	edge_keys[1] = mapping.destination_key;
	validate_relation(vertex, vertex_keys, 1);
	validate_relation(edge, edge_keys, 2);
	counter_baseline = graph_stats_counter_baseline(&scope);
	scan_pages = (uint64) RelationGetNumberOfBlocks(vertex) +
		(uint64) RelationGetNumberOfBlocks(edge) * 2;
	if (scan_pages > options.max_scan_pages)
	{
		table_close(edge, NoLock);
		table_close(vertex, NoLock);
		PG_RETURN_JSONB_P(status_json("PARTIAL", "scan_page_budget", 0,
											scan_pages, 0, 0));
	}

	collect_context = AllocSetContextCreate(CurrentMemoryContext,
		"single-hop graph statistics", ALLOCSET_DEFAULT_SIZES);
	old_context = MemoryContextSwitchTo(collect_context);
	strata = palloc0(sizeof(*strata) * options.strata);
	for (uint32 i = 0; i < options.strata; i++)
	{
		strata[i].capacity = options.full ? options.target :
			options.target / options.strata +
			(i < options.target % options.strata ? 1 : 0);
		strata[i].items = palloc0(sizeof(VertexSample) * strata[i].capacity);
		pg_prng_seed(&strata[i].random, options.seed ^ ((uint64) i << 32) ^ i);
	}
	heavy = palloc0(sizeof(*heavy) * options.heavy_limit);
	if (options.full)
	{
		HASHCTL ctl = {0};

		ctl.keysize = sizeof(PairKey);
		ctl.entrysize = sizeof(PairEntry);
		ctl.hcxt = collect_context;
		pairs = hash_create("full graph parallel edges", 128, &ctl,
						HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	peak = MemoryContextMemAllocated(collect_context, true);
	if (peak > options.max_memory)
		partial_reason = "memory_budget";

	/* A complete vertex pass is required before its reservoir is publishable. */
	if (!partial_reason)
	{
		scan = table_beginscan(vertex, GetActiveSnapshot(), 0, NULL, SO_NONE);
		slot = table_slot_create(vertex, NULL);
		while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
		{
			bool isnull;

			if (examined >= options.max_examined)
			{
				partial_reason = "vertex_entry_budget";
				break;
			}
			int64 key = DatumGetInt64(slot_getattr(slot, mapping.vertex_key, &isnull));
			uint32 sid = (uint64) key % options.strata;
			SampleStratum *s = &strata[sid];

			if (isnull)
				elog(ERROR, "null graph vertex key despite mapping validation");
			s->population++;
			population++;
			examined++;
			if (s->count < s->capacity)
				s->items[s->count++] = (VertexSample) {.key = key, .stratum = sid};
			else
			{
				uint64 position = pg_prng_uint64_range(&s->random, 0, s->population - 1);

				if (position < s->capacity)
					s->items[position] = (VertexSample) {.key = key, .stratum = sid};
			}
			ExecClearTuple(slot);
			if (!collect_checkpoint(&options, started, examined))
			{
				partial_reason = "deadline";
				break;
			}
		}
		table_endscan(scan);
		scan = NULL;
		ExecDropSingleTupleTableSlot(slot);
		slot = NULL;
	}
	if (!partial_reason)
	{
		for (uint32 i = 0; i < options.strata; i++)
		{
			if (strata[i].population && strata[i].count == 0)
				partial_reason = "unsampled_stratum";
			if (options.full && strata[i].population > strata[i].capacity)
				partial_reason = "full_vertex_target";
			sample_count += strata[i].count;
		}
		if (sample_count > options.max_bindings)
			partial_reason = "binding_budget";
	}
	if (!partial_reason)
	{
		samples = palloc0(sizeof(*samples) * sample_count);
		for (uint32 i = 0, offset = 0; i < options.strata; i++)
		{
			memcpy(samples + offset, strata[i].items,
				   sizeof(*samples) * strata[i].count);
			offset += strata[i].count;
		}
		qsort(samples, sample_count, sizeof(*samples), sample_key_compare);
		peak = Max(peak, MemoryContextMemAllocated(collect_context, true));
		if (peak > options.max_memory)
			partial_reason = "memory_budget";
	}

	/* The first edge pass gives exact N_E and complete sampled degrees. */
	if (!partial_reason)
	{
		scan = table_beginscan(edge, GetActiveSnapshot(), 0, NULL, SO_NONE);
		slot = table_slot_create(edge, NULL);
		while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
		{
			bool srcnull;
			bool dstnull;

			if (examined >= options.max_examined)
			{
				partial_reason = "edge_entry_budget";
				break;
			}
			int64 src = DatumGetInt64(slot_getattr(slot, mapping.source_key, &srcnull));
			int64 dst = DatumGetInt64(slot_getattr(slot, mapping.destination_key, &dstnull));
			int64 endpoint = scope.direction == 1 ? src : dst;
			VertexSample *sample;

			if (srcnull || dstnull)
				elog(ERROR, "null graph edge endpoint despite mapping validation");
			visible_edges++;
			self_loops += src == dst;
			examined++;
			sample = sample_lookup(samples, sample_count, endpoint);
			if (sample)
			{
				if (sample->degree < options.max_entries_per_seed)
					sample->degree++;
				else
					sample->censored = true;
			}
			heavy_observe(heavy, options.heavy_limit, endpoint);
			if (pairs)
			{
				PairKey key = {.source = src, .destination = dst};
				bool found;
				PairEntry *entry = hash_search(pairs, &key, HASH_ENTER, &found);

				if (found)
					parallel_extra++;
				entry->count++;
			}
			ExecClearTuple(slot);
			peak = Max(peak, MemoryContextMemAllocated(collect_context, true));
			if (peak > options.max_memory ||
				!collect_checkpoint(&options, started, examined))
			{
				partial_reason = peak > options.max_memory ? "memory_budget" :
					"deadline";
				break;
			}
		}
		table_endscan(scan);
		scan = NULL;
		ExecDropSingleTupleTableSlot(slot);
		slot = NULL;
	}

	/* Recheck only discovered candidates in a second visible heap pass. */
	if (!partial_reason)
	{
		scan = table_beginscan(edge, GetActiveSnapshot(), 0, NULL, SO_NONE);
		slot = table_slot_create(edge, NULL);
		while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
		{
			bool isnull;

			if (examined >= options.max_examined)
			{
				partial_reason = "heavy_entry_budget";
				break;
			}
			AttrNumber endpoint_attnum = scope.direction == 1 ?
				mapping.source_key : mapping.destination_key;
			int64 endpoint = DatumGetInt64(slot_getattr(slot, endpoint_attnum, &isnull));

			if (isnull)
				elog(ERROR, "null graph endpoint despite mapping validation");
			for (uint32 i = 0; i < options.heavy_limit; i++)
				if (heavy[i].used && heavy[i].key == endpoint)
					heavy[i].exact_degree++;
			examined++;
			ExecClearTuple(slot);
			if (!collect_checkpoint(&options, started, examined))
			{
				partial_reason = "deadline";
				break;
			}
		}
		table_endscan(scan);
		scan = NULL;
		ExecDropSingleTupleTableSlot(slot);
		slot = NULL;
	}

	if (partial_reason)
	{
		MemoryContextSwitchTo(old_context);
		return_value = status_json("PARTIAL", partial_reason, 0,
								   scan_pages, examined, peak);
		MemoryContextDelete(collect_context);
		table_close(edge, NoLock);
		table_close(vertex, NoLock);
		PG_RETURN_JSONB_P(return_value);
	}
	else
	{
		WeightedDegree *degrees = palloc(sizeof(*degrees) * sample_count);
		double		weighted_sum = 0;
		double		weighted_zero = 0;
		double		weight_square_sum = 0;
		double		cumulative = 0;
		double		p50 = 0;
		double		p95 = 0;
		double		p99 = 0;
		bool		p50_set = false;
		bool		p95_set = false;
		bool		p99_set = false;
		uint64		observed_max = 0;
		bool		censored = false;
		StringInfoData profiles;
		StringInfoData metadata;
		StringInfoData diagnostics;
		int			profile_count = 0;
		char	   *started_text;
		char	   *finished_text;
		Jsonb	   *profile_json;
		int64		generation;

		for (uint32 i = 0; i < sample_count; i++)
		{
			SampleStratum *s = &strata[samples[i].stratum];
			double weight = (double) s->population / s->count;

			degrees[i] = (WeightedDegree) {.degree = samples[i].degree,
				.weight = weight};
			weighted_sum += weight * samples[i].degree;
			weighted_zero += samples[i].degree == 0 ? weight : 0;
			weight_square_sum += weight * weight;
			observed_max = Max(observed_max, samples[i].degree);
			censored |= samples[i].censored;
		}
		qsort(degrees, sample_count, sizeof(*degrees), degree_compare);
		for (uint32 i = 0; i < sample_count; i++)
		{
			cumulative += degrees[i].weight;
			if (cumulative >= population * 0.50 && !p50_set)
			{
				p50 = degrees[i].degree;
				p50_set = true;
			}
			if (cumulative >= population * 0.95 && !p95_set)
			{
				p95 = degrees[i].degree;
				p95_set = true;
			}
			if (cumulative >= population * 0.99 && !p99_set)
			{
				p99 = degrees[i].degree;
				p99_set = true;
			}
		}
		started_text = DatumGetCString(DirectFunctionCall1(timestamptz_out,
			TimestampTzGetDatum(started)));
		finished_text = DatumGetCString(DirectFunctionCall1(timestamptz_out,
			TimestampTzGetDatum(GetCurrentTimestamp())));
		initStringInfo(&metadata);
		appendStringInfo(&metadata,
			"{\"started_at\":\"%s\",\"finished_at\":\"%s\","
			"\"snapshot_source\":\"active_mvcc_tableam\","
			"\"sample_method\":\"%s_stratified_reservoir_edge_scan\","
			"\"truncation_reason\":\"%s\",\"seed\":" UINT64_FORMAT ","
			"\"population\":" UINT64_FORMAT ",\"sample_count\":%u,"
			"\"effective_sample_size\":%.17g,\"complete_depth\":1,"
			"\"layout_version\":1,\"model_version\":1,"
			"\"change_baseline\":{\"source\":\"explicit_collection\",\"counters\":%s},"
			"\"resources\":{\"scan_pages\":" UINT64_FORMAT ","
			"\"examined_entries\":" UINT64_FORMAT ",\"peak_bytes\":%zu}}",
			started_text, finished_text, options.full ? "full" : "uniform",
			censored ? "max_entries_per_seed" : "none", options.seed,
			population, sample_count,
			weight_square_sum ? population * population / weight_square_sum : 0,
			counter_baseline, scan_pages, examined, peak);
		initStringInfo(&profiles);
		appendStringInfoChar(&profiles, '[');
#define ADD_KNOWN(id,pop,name,value,samples_n) \
	append_profile(&profiles, &profile_count, id, pop, name, "", metadata.data, \
		options.full ? GGC_METRIC_EXACT_AT_SAMPLE : GGC_METRIC_ESTIMATED, \
		GGC_STATS_READY, value, samples_n, false)
		append_profile(&profiles, &profile_count, GGC_METRIC_VERTEX_POPULATION,
			"uniform_vertices", "vertex_population", "", metadata.data,
			GGC_METRIC_EXACT_AT_SAMPLE, GGC_STATS_READY, population, population, false);
		append_profile(&profiles, &profile_count, GGC_METRIC_VISIBLE_EDGES,
			"visible_edges", "visible_edges", "", metadata.data,
			GGC_METRIC_EXACT_AT_SAMPLE, GGC_STATS_READY, visible_edges,
			visible_edges, false);
		append_profile(&profiles, &profile_count, GGC_METRIC_MEAN_DEGREE,
			"uniform_vertices", "mean_degree", "", metadata.data,
			population == 0 ? GGC_METRIC_UNKNOWN : censored ? GGC_METRIC_LOWER_BOUND :
				(options.full ? GGC_METRIC_EXACT_AT_SAMPLE : GGC_METRIC_ESTIMATED),
			population == 0 || censored ? GGC_STATS_PARTIAL : GGC_STATS_READY,
			population ? weighted_sum / population : 0, sample_count, censored);
		append_profile(&profiles, &profile_count, GGC_METRIC_ZERO_DEGREE_FRACTION,
			"uniform_vertices", "zero_degree_fraction", "", metadata.data,
			population == 0 ? GGC_METRIC_UNKNOWN :
				(options.full ? GGC_METRIC_EXACT_AT_SAMPLE : GGC_METRIC_ESTIMATED),
			population == 0 ? GGC_STATS_PARTIAL : GGC_STATS_READY,
			population ? weighted_zero / population : 0, sample_count, false);
		append_profile(&profiles, &profile_count, GGC_METRIC_DEGREE_P50,
			"uniform_vertices", "degree_p50", "", metadata.data,
			population == 0 ? GGC_METRIC_UNKNOWN : censored ? GGC_METRIC_LOWER_BOUND :
				(options.full ? GGC_METRIC_EXACT_AT_SAMPLE : GGC_METRIC_ESTIMATED),
			population == 0 || censored ? GGC_STATS_PARTIAL : GGC_STATS_READY,
			p50, sample_count, censored);
		append_profile(&profiles, &profile_count, GGC_METRIC_DEGREE_P95,
			"uniform_vertices", "degree_p95", "", metadata.data,
			population == 0 ? GGC_METRIC_UNKNOWN : censored ? GGC_METRIC_LOWER_BOUND :
				(options.full ? GGC_METRIC_EXACT_AT_SAMPLE : GGC_METRIC_ESTIMATED),
			population == 0 || censored ? GGC_STATS_PARTIAL : GGC_STATS_READY,
			p95, sample_count, censored);
		append_profile(&profiles, &profile_count, GGC_METRIC_DEGREE_P99,
			"uniform_vertices", "degree_p99", "", metadata.data,
			population == 0 ? GGC_METRIC_UNKNOWN : censored ? GGC_METRIC_LOWER_BOUND :
				(options.full ? GGC_METRIC_EXACT_AT_SAMPLE : GGC_METRIC_ESTIMATED),
			population == 0 || censored ? GGC_STATS_PARTIAL : GGC_STATS_READY,
			p99, sample_count, censored);
		append_profile(&profiles, &profile_count, GGC_METRIC_OBSERVED_SAMPLE_MAX,
			"uniform_vertices", "observed_sample_max", "", metadata.data,
			population == 0 ? GGC_METRIC_UNKNOWN : censored ? GGC_METRIC_LOWER_BOUND :
				(options.full ? GGC_METRIC_EXACT_AT_SAMPLE : GGC_METRIC_ESTIMATED),
			population == 0 || censored ? GGC_STATS_PARTIAL : GGC_STATS_READY,
			observed_max, sample_count, censored);
		append_profile(&profiles, &profile_count, GGC_METRIC_SELF_LOOPS,
			"visible_edges", "self_loops", "", metadata.data,
			GGC_METRIC_EXACT_AT_SAMPLE, GGC_STATS_READY, self_loops,
			visible_edges, false);
		append_profile(&profiles, &profile_count, GGC_METRIC_PARALLEL_EXTRA_EDGES,
			"visible_edges", "parallel_extra_edges", "", metadata.data,
			options.full ? GGC_METRIC_EXACT_AT_SAMPLE : GGC_METRIC_UNKNOWN,
			options.full ? GGC_STATS_READY : GGC_STATS_PARTIAL,
			parallel_extra, visible_edges, false);
		for (uint32 i = 0; i < options.strata; i++)
		{
			char *extra = psprintf(",\"stratum\":%u,\"stratum_population\":"
				UINT64_FORMAT ",\"stratum_samples\":%u", i,
				strata[i].population, strata[i].count);
			double weight = strata[i].count ?
				(double) strata[i].population / strata[i].count : 0;

			append_profile(&profiles, &profile_count,
				GGC_METRIC_STRATUM_SURVEY_WEIGHT, "uniform_vertices",
				"stratum_survey_weight", extra, metadata.data,
				GGC_METRIC_EXACT_AT_SAMPLE, GGC_STATS_READY, weight,
				strata[i].count, false);
		}
		for (uint32 i = 0; i < options.heavy_limit; i++)
			if (heavy[i].used)
			{
				char *extra = psprintf(",\"endpoint\":" INT64_FORMAT
					",\"discovery_upper\":" UINT64_FORMAT
					",\"discovery_error\":" UINT64_FORMAT,
					heavy[i].key, heavy[i].discovery_count,
					heavy[i].discovery_error);

				append_profile(&profiles, &profile_count,
					GGC_METRIC_DISCOVERED_HEAVY_DEGREE, "discovered_heavy_hitters",
					"exact_degree", extra, metadata.data,
					GGC_METRIC_EXACT_AT_SAMPLE, GGC_STATS_READY,
					heavy[i].exact_degree, heavy[i].exact_degree, false);
			}
#undef ADD_KNOWN
		appendStringInfoChar(&profiles, ']');
		if (profile_count > options.max_profiles)
			partial_reason = "profile_budget";
		profile_json = DatumGetJsonbP(DirectFunctionCall1(jsonb_in,
			CStringGetDatum(profiles.data)));
		if (VARSIZE_ANY(profile_json) > options.max_payload)
			partial_reason = "payload_budget";
		if (partial_reason)
		{
			MemoryContextSwitchTo(old_context);
			return_value = status_json("PARTIAL", partial_reason, 0,
									   scan_pages, examined, peak);
			MemoryContextDelete(collect_context);
			table_close(edge, NoLock);
			table_close(vertex, NoLock);
			PG_RETURN_JSONB_P(return_value);
		}
		generation = graph_stats_publish_collected(stats_oid, profile_json);
		initStringInfo(&diagnostics);
		appendStringInfo(&diagnostics,
			"{\"status\":\"READY\",\"published\":true,\"reason\":\"none\","
			"\"generation\":" INT64_FORMAT ",\"profile_count\":%d,"
			"\"population\":" UINT64_FORMAT ",\"visible_edges\":" UINT64_FORMAT ","
			"\"sample_count\":%u,\"censored\":%s,\"samples\":[",
			generation, profile_count, population, visible_edges, sample_count,
			censored ? "true" : "false");
		for (uint32 i = 0; i < sample_count; i++)
			appendStringInfo(&diagnostics,
				"%s{\"key\":" INT64_FORMAT ",\"degree\":" UINT64_FORMAT
				",\"stratum\":%u,\"censored\":%s}",
				i ? "," : "", samples[i].key, samples[i].degree,
				samples[i].stratum, samples[i].censored ? "true" : "false");
		appendStringInfo(&diagnostics,
			"],\"resources\":{\"scan_pages\":" UINT64_FORMAT
			",\"examined_entries\":" UINT64_FORMAT ",\"peak_bytes\":%zu}}",
			scan_pages, examined, peak);
		MemoryContextSwitchTo(old_context);
		return_value = DatumGetJsonbP(DirectFunctionCall1(jsonb_in,
			CStringGetDatum(diagnostics.data)));
		MemoryContextDelete(collect_context);
		table_close(edge, NoLock);
		table_close(vertex, NoLock);
		PG_RETURN_JSONB_P(return_value);
	}
}
