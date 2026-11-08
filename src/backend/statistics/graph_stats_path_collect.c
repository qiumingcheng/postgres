/*-------------------------------------------------------------------------
 * graph_stats_path_collect.c
 *
 * Explicit, bounded fixed-path profile collection.  It deliberately starts
 * below GraphPath: uniform vertex seeds are expanded through the public
 * adjacency provider and GgcBatch, so the planner never has to select a graph
 * plan in order to obtain its first statistics.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/detoast.h"
#include "access/genam.h"
#include "access/heapam.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_graph_statistic.h"
#include "catalog/pg_propgraph_element.h"
#include "catalog/pg_type_d.h"
#include "commands/explain.h"
#include "common/pg_prng.h"
#include "executor/executor.h"
#include "executor/graph_batch.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "statistics/graph_stats.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"

#define PATH_OPTION_FIELDS 18
#define PATH_MIN_HOPS 2
#define PATH_MAX_HOPS 4
#define PATH_MAX_SEEDS 16
#define PATH_CANDIDATE_BATCH 64
#define PATH_MAX_ENDPOINT_BATCH 64

typedef struct PathOptions
{
	char	   *counter_baseline;	/* captured before scanning; not a user option */
	bool		conditional;	/* read from registered statistics options */
	int64		anchor_key;
	uint64		seed;
	uint32		seed_target;
	uint32		hops;
	AttrNumber	predicate_attnum;
	int64		predicate_min;
	int64		predicate_max;
	uint32		predicate_clause_id;
	uint64		max_scan_pages;
	uint64		max_examined;
	uint64		max_bindings;
	uint64		max_bindings_per_seed;
	uint64		max_entries_per_seed;
	Size		max_memory;
	Size		max_payload;
	uint32		max_profiles;
	uint32		max_parent_batch;
	uint32		deadline_ms;
	uint32		lock_timeout_ms;
} PathOptions;

typedef struct PathMapping
{
	Oid			graph_relid;
	Oid			vertex_relid;
	Oid			edge_relid;
	AttrNumber	vertex_key;
	AttrNumber	edge_identity;
	AttrNumber	source_key;
	AttrNumber	destination_key;
} PathMapping;

typedef struct PathSeed
{
	int64		key;
	double		weight;
} PathSeed;

typedef struct PathBinding
{
	uint32		seed;
	uint32		depth;
	uint64		ordinal;
	int64		vertices[PATH_MAX_HOPS + 1];
	int64		edges[PATH_MAX_HOPS];
	GgcEndpointHandleV2 handle;
} PathBinding;

typedef struct SeedStage
{
	double		frontier;
	double		bindings;
	double		logical_edges;
	double		accepted_edges;
	double		next_bindings;
	double		next_frontier;
	double		arrival_degree_sum;
	double		arrival_count;
} SeedStage;

typedef struct PathStage
{
	SeedStage  *seeds;
	uint64		requests;
	uint64		physical_entries;
	uint64		edge_fetches;
	uint64		heap_page_groups;
	uint64		tableam_calls;
	uint64		directory_lookups;
	uint64		ref_direct_lookups;
	uint64		ref_fallbacks;
	uint64		slot_dereferences;
	uint64		zero_degree_requests;
	Size		peak_restore_bytes;
	bool		partial;
} PathStage;

typedef struct DegreeEntry
{
	int64		key;
	uint64		degree;
} DegreeEntry;

typedef struct SeedEndpointKey
{
	uint32		seed;
	uint32		padding;
	int64		endpoint;
} SeedEndpointKey;

typedef struct CandidateKey
{
	uint32		seed;
	uint32		padding;
	int64		endpoint;
	int64		edge;
} CandidateKey;

typedef struct SeenEntry
{
	CandidateKey key;
} SeenEntry;

typedef struct EndpointEntry
{
	SeedEndpointKey key;
} EndpointEntry;

typedef struct GlobalEndpointEntry
{
	int64		key;
} GlobalEndpointEntry;

PG_FUNCTION_INFO_V1(pg_graph_statistic_collect_path);

static JsonbValue *
path_field(Jsonb *object, const char *name)
{
	JsonbValue	key;
	JsonbValue *value;

	key.type = jbvString;
	key.val.string.val = (char *) name;
	key.val.string.len = strlen(name);
	value = findJsonbValueFromContainer(&object->root, JB_FOBJECT, &key);
	if (!value)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("missing fixed-path collection option \"%s\"", name)));
	return value;
}

static int64
path_int(Jsonb *object, const char *name)
{
	JsonbValue *value = path_field(object, name);
	int64		number;
	Numeric		integral;

	if (value->type != jbvNumeric)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("fixed-path collection option \"%s\" must be an integer", name)));
	number = DatumGetInt64(DirectFunctionCall1(numeric_int8,
		NumericGetDatum(value->val.numeric)));
	integral = DatumGetNumeric(DirectFunctionCall1(int8_numeric,
		Int64GetDatum(number)));
	if (DatumGetInt32(DirectFunctionCall2(numeric_cmp,
		NumericGetDatum(value->val.numeric), NumericGetDatum(integral))) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("fixed-path collection option \"%s\" must be integral", name)));
	return number;
}

static uint64
path_uint(Jsonb *object, const char *name, uint64 maximum)
{
	int64		number = path_int(object, name);

	if (number < 0 || (uint64) number > maximum)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("fixed-path collection option \"%s\" is out of range", name)));
	return (uint64) number;
}

static void
parse_path_options(Jsonb *object, PathOptions *options)
{
	Size		binding_bytes;

	if (!JB_ROOT_IS_OBJECT(object) || JB_ROOT_COUNT(object) != PATH_OPTION_FIELDS)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("fixed-path collection requires exactly its versioned options")));
	options->seed = path_uint(object, "seed", PG_INT64_MAX);
	options->seed_target = path_uint(object, "seed_target", PATH_MAX_SEEDS);
	options->hops = path_uint(object, "hops", PATH_MAX_HOPS);
	options->predicate_attnum = path_uint(object, "predicate_attnum", PG_INT16_MAX);
	options->predicate_min = path_int(object, "predicate_min");
	options->predicate_max = path_int(object, "predicate_max");
	options->predicate_clause_id = path_uint(object, "predicate_clause_id", PG_UINT32_MAX);
	options->max_scan_pages = path_uint(object, "max_scan_pages", PG_INT64_MAX);
	options->max_examined = path_uint(object, "max_examined_entries", PG_INT64_MAX);
	options->max_bindings = path_uint(object, "max_bindings", PG_INT64_MAX);
	options->max_bindings_per_seed = path_uint(object, "max_bindings_per_seed", PG_INT64_MAX);
	options->max_entries_per_seed = path_uint(object, "max_entries_per_seed", PG_INT64_MAX);
	options->max_memory = path_uint(object, "max_memory_bytes", MaxAllocSize);
	options->max_payload = path_uint(object, "max_payload_bytes", GGC_STATS_MAX_PAYLOAD_BYTES);
	options->max_profiles = path_uint(object, "max_profiles", GGC_STATS_MAX_PROFILES);
	options->max_parent_batch = path_uint(object, "max_parent_batch", PATH_MAX_ENDPOINT_BATCH);
	options->deadline_ms = path_uint(object, "deadline_ms", PG_UINT32_MAX);
	options->lock_timeout_ms = path_uint(object, "lock_timeout_ms", PG_UINT32_MAX);
	if (options->seed_target == 0 || options->hops < PATH_MIN_HOPS ||
		options->predicate_min > options->predicate_max ||
		/* An absent predicate has one canonical encoding. Half-specified
		 * options must not silently turn a filtered population into all rows. */
		((options->predicate_attnum == 0 || options->predicate_clause_id == 0) &&
		 (options->predicate_attnum != 0 || options->predicate_clause_id != 0 ||
		  options->predicate_min != 0 || options->predicate_max != 0)) ||
		options->max_scan_pages == 0 ||
		options->max_examined == 0 || options->max_bindings < options->seed_target ||
		options->max_bindings_per_seed == 0 || options->max_entries_per_seed == 0 ||
		options->max_memory < 256 * 1024 || options->max_payload == 0 ||
		options->max_profiles == 0 || options->max_parent_batch == 0 ||
		options->deadline_ms == 0 || options->lock_timeout_ms == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("fixed-path collection budgets and predicate must be finite")));
	if (options->max_bindings > MaxAllocSize / sizeof(PathBinding))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("fixed-path binding budget exceeds allocation limits")));
	binding_bytes = (Size) options->max_bindings * sizeof(PathBinding);
	if (binding_bytes > options->max_memory / 2)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("fixed-path binding arrays exceed the memory budget")));
}

static int16
path_single_int2_array(HeapTuple tuple, TupleDesc desc, AttrNumber attnum,
					   const char *description)
{
	bool		isnull;
	Datum		value = heap_getattr(tuple, attnum, desc, &isnull);
	ArrayType  *array;
	Datum	   *values;
	bool	   *nulls;
	int			count;

	if (isnull)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("%s mapping is missing", description)));
	array = DatumGetArrayTypeP(value);
	deconstruct_array(array, INT2OID, sizeof(int16), true, TYPALIGN_SHORT,
					  &values, &nulls, &count);
	if (count != 1 || nulls[0])
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("%s mapping must contain one column", description)));
	return DatumGetInt16(values[0]);
}

static bool
load_stats_scope(Oid stats_oid, GgcStatsScope *scope, int64 *anchor_key)
{
	Relation	head;
	HeapTuple	tuple;
	Form_pg_graph_statistic form;
	bool		isnull;
	Datum		encoded;
	bool		conditional;

	head = table_open(GraphStatisticRelationId, AccessShareLock);
	tuple = SearchSysCache1(GRAPHSTATISTICOID, ObjectIdGetDatum(stats_oid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("graph statistics object %u does not exist", stats_oid)));
	form = (Form_pg_graph_statistic) GETSTRUCT(tuple);
	if (form->gstowner != GetUserId())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("fixed-path collector must own its statistics scope")));
	encoded = heap_getattr(tuple, Anum_pg_graph_statistic_gstscope,
		RelationGetDescr(head), &isnull);
	if (isnull || toast_raw_datum_size(encoded) > 84)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("graph statistics scope is missing or oversized")));
	graph_stats_scope_decode(DatumGetByteaPP(encoded), scope);
	encoded = heap_getattr(tuple, Anum_pg_graph_statistic_gstoptions,
		RelationGetDescr(head), &isnull);
	if (isnull || toast_raw_datum_size(encoded) > 8192)
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
						errmsg("graph statistics options are missing or oversized")));
	conditional = graph_stats_anchor_condition(DatumGetJsonbP(encoded), anchor_key);
	ReleaseSysCache(tuple);
	table_close(head, AccessShareLock);
	return conditional;
}

static void
load_path_mapping(const GgcStatsScope *scope, PathMapping *mapping)
{
	HeapTuple	edge;
	HeapTuple	vertex;
	Form_pg_propgraph_element edge_form;
	Form_pg_propgraph_element vertex_form;
	Relation	maprel;
	Oid			vertex_element;

	edge = SearchSysCache1(PROPGRAPHELOID,
		ObjectIdGetDatum(scope->edge_element_oid));
	if (!HeapTupleIsValid(edge))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("graph edge mapping no longer exists")));
	edge_form = (Form_pg_propgraph_element) GETSTRUCT(edge);
	if (edge_form->pgepgid != scope->graph_oid ||
		edge_form->pgekind != PGEKIND_EDGE ||
		edge_form->pgesrcvertexid != edge_form->pgedestvertexid)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("fixed-path collector requires one repeated vertex domain")));
	mapping->graph_relid = scope->graph_oid;
	mapping->edge_relid = edge_form->pgerelid;
	vertex_element = edge_form->pgesrcvertexid;
	maprel = table_open(PropgraphElementRelationId, AccessShareLock);
	mapping->edge_identity = path_single_int2_array(edge,
		RelationGetDescr(maprel), Anum_pg_propgraph_element_pgekey,
		"edge identity key");
	mapping->source_key = path_single_int2_array(edge,
		RelationGetDescr(maprel), Anum_pg_propgraph_element_pgesrckey,
		"edge source key");
	mapping->destination_key = path_single_int2_array(edge,
		RelationGetDescr(maprel), Anum_pg_propgraph_element_pgedestkey,
		"edge destination key");
	ReleaseSysCache(edge);
	vertex = SearchSysCache1(PROPGRAPHELOID, ObjectIdGetDatum(vertex_element));
	if (!HeapTupleIsValid(vertex))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("graph vertex mapping no longer exists")));
	vertex_form = (Form_pg_propgraph_element) GETSTRUCT(vertex);
	if (vertex_form->pgepgid != scope->graph_oid ||
		vertex_form->pgekind != PGEKIND_VERTEX)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("fixed-path vertex mapping differs from its scope")));
	mapping->vertex_relid = vertex_form->pgerelid;
	mapping->vertex_key = path_single_int2_array(vertex,
		RelationGetDescr(maprel), Anum_pg_propgraph_element_pgekey,
		"vertex key");
	ReleaseSysCache(vertex);
	table_close(maprel, AccessShareLock);
}

static void
validate_path_scopes(const GgcStatsScope *logical,
					 const GgcStatsScope *physical,
					 const PathMapping *mapping,
					 const PathOptions *options)
{
	Relation	edge;
	Relation	index;
	TupleDesc	desc;
	AttrNumber	endpoint;
	bool		endpoint_found = false;
	AttrNumber	keys[4];

	if (logical->scope_kind != GGC_STATS_LOGICAL ||
		physical->scope_kind != GGC_STATS_PHYSICAL ||
		logical->catalog_provider_oid || physical->catalog_provider_oid ||
		logical->effective_role != GetUserId() ||
		physical->effective_role != GetUserId() ||
		logical->graph_oid != physical->graph_oid ||
		logical->edge_element_oid != physical->edge_element_oid ||
		logical->endpoint_domain != physical->endpoint_domain ||
		logical->direction != physical->direction ||
		logical->path_mode != physical->path_mode ||
		logical->result_mode != physical->result_mode ||
		logical->catalog_generation != physical->catalog_generation ||
		logical->security_generation != physical->security_generation ||
		logical->semantic_fingerprint != physical->semantic_fingerprint ||
		!OidIsValid(physical->index_relid) || physical->build_generation == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("logical and physical fixed-path scopes do not match")));
	if ((logical->direction != GGC_OUT && logical->direction != GGC_IN) ||
		(logical->result_mode != GGC_STATS_ALL_BINDINGS &&
		 logical->result_mode != GGC_STATS_ENDPOINT_SETS) ||
		logical->path_mode < 1 || logical->path_mode > 4 ||
		(logical->result_mode == GGC_STATS_ENDPOINT_SETS && logical->path_mode != GGC_WALK))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("fixed-path collection requires bindings or WALK endpoint sets")));
	edge = table_open(mapping->edge_relid, AccessShareLock);
	index = index_open(physical->index_relid, AccessShareLock);
	desc = RelationGetDescr(edge);
	keys[0] = mapping->edge_identity;
	keys[1] = mapping->source_key;
	keys[2] = mapping->destination_key;
	keys[3] = options->predicate_attnum;
	if (edge->rd_rel->relkind != RELKIND_RELATION ||
		edge->rd_rel->relpersistence != RELPERSISTENCE_PERMANENT ||
		edge->rd_rel->relrowsecurity || RELATION_IS_OTHER_TEMP(edge) ||
		pg_class_aclcheck(mapping->edge_relid, GetUserId(), ACL_SELECT) != ACLCHECK_OK)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("fixed-path collection requires a selectable logged heap without RLS")));
	for (int i = 0; i < lengthof(keys); i++)
	{
		Form_pg_attribute attribute;

		if (i == 3 && keys[i] == 0)
			continue;
		if (keys[i] <= 0 || keys[i] > desc->natts)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
					 errmsg("fixed-path collection attribute is out of range")));
		attribute = TupleDescAttr(desc, keys[i] - 1);
		if (attribute->attisdropped || attribute->atttypid != INT8OID ||
			(i < 3 && !attribute->attnotnull))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("fixed-path identity, endpoints and predicate must be bigint")));
	}
	endpoint = logical->direction == GGC_OUT ?
		mapping->source_key : mapping->destination_key;
	if (index->rd_index)
		for (int i = 0; i < index->rd_index->indnkeyatts; i++)
			if (index->rd_index->indkey.values[i] == endpoint)
				endpoint_found = true;
	/* AM-specific key order and shape are checked by provider->validate. */
	if (!index->rd_index || index->rd_index->indrelid != mapping->edge_relid ||
		RelationGetIndexExpressions(index) != NIL ||
		RelationGetIndexPredicate(index) != NIL ||
		!endpoint_found)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("physical fixed-path scope index does not match its direction")));
	index_close(index, NoLock);
	table_close(edge, NoLock);
}

static bool
path_lock_relation(Oid relid, TimestampTz started, uint32 timeout_ms)
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

static bool
path_deadline(TimestampTz started, uint32 deadline_ms)
{
	CHECK_FOR_INTERRUPTS();
	return !TimestampDifferenceExceeds(started, GetCurrentTimestamp(), deadline_ms);
}

static HTAB *
create_hash(const char *name, Size keysize, Size entrysize,
			MemoryContext context, long initial)
{
	HASHCTL		ctl = {0};

	ctl.keysize = keysize;
	ctl.entrysize = entrysize;
	ctl.hcxt = context;
	return hash_create(name, initial, &ctl,
		HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

static uint64
degree_lookup(HTAB *degrees, int64 key)
{
	DegreeEntry *entry = hash_search(degrees, &key, HASH_FIND, NULL);

	return entry ? entry->degree : 0;
}

static bool
predicate_passes(TupleTableSlot *slot, const PathOptions *options)
{
	bool		isnull;
	Datum		value;
	int64		number;

	if (options->predicate_attnum == 0)
		return true;
	value = slot_getattr(slot, options->predicate_attnum, &isnull);
	if (isnull)
		return false;
	number = DatumGetInt64(value);
	return number >= options->predicate_min && number <= options->predicate_max;
}

static bool
mode_allows(const PathBinding *parent, int64 edge, int64 opposite,
			uint32 next_depth, uint32 hops, uint32 mode)
{
	if (mode == 1)
		return true;
	if (mode == 2)
	{
		for (uint32 i = 0; i < parent->depth; i++)
			if (parent->edges[i] == edge)
				return false;
		return true;
	}
	if (mode == 3)
	{
		for (uint32 i = 0; i <= parent->depth; i++)
			if (parent->vertices[i] == opposite)
				return false;
		return true;
	}
	Assert(mode == 4);
	for (uint32 i = 0; i <= parent->depth; i++)
		if (parent->vertices[i] == opposite)
			return opposite == parent->vertices[0] && next_depth == hops;
	return true;
}

static bool
safe_batch_next(GgcBatchState **batch, GgcBatchResult *result)
{
	bool		found;

	PG_TRY();
	{
		found = GgcBatchNext(*batch, result);
	}
	PG_CATCH();
	{
		/* GgcBatchNext destroys its own state before rethrowing. */
		*batch = NULL;
		PG_RE_THROW();
	}
	PG_END_TRY();
	return found;
}

static double
weighted_mean(const double *values, const PathSeed *seeds, uint32 count)
{
	double		sum = 0;
	double		weight = 0;

	for (uint32 i = 0; i < count; i++)
	{
		sum += values[i] * seeds[i].weight;
		weight += seeds[i].weight;
	}
	return weight > 0 ? sum / weight : 0;
}

typedef struct WeightedValue
{
	double		value;
	double		weight;
} WeightedValue;

static int
weighted_value_compare(const void *left, const void *right)
{
	const WeightedValue *a = left;
	const WeightedValue *b = right;

	return a->value < b->value ? -1 : a->value > b->value;
}

static double
weighted_p95(const double *values, const PathSeed *seeds, uint32 count)
{
	WeightedValue *sorted;
	double		total = 0;
	double		cumulative = 0;
	double		result = 0;

	if (count == 0)
		return 0;
	sorted = palloc(sizeof(*sorted) * count);
	for (uint32 i = 0; i < count; i++)
	{
		sorted[i] = (WeightedValue) {.value = values[i], .weight = seeds[i].weight};
		total += seeds[i].weight;
	}
	qsort(sorted, count, sizeof(*sorted), weighted_value_compare);
	for (uint32 i = 0; i < count; i++)
	{
		cumulative += sorted[i].weight;
		result = sorted[i].value;
		if (cumulative >= total * 0.95)
			break;
	}
	pfree(sorted);
	return result;
}

static const char *
mode_name(uint32 mode)
{
	switch (mode)
	{
		case 1:
			return "WALK";
		case 2:
			return "TRAIL";
		case 3:
			return "ACYCLIC";
		case 4:
			return "SIMPLE";
	}
	return "UNKNOWN";
}

static void
append_profile(StringInfo profiles, int *count, int metric_id,
			   const char *key_extra, const char *metadata,
			   GgcMetricKind kind, GgcStatsState state, double value,
			   double p95, bool p95_known, uint64 sample_count, bool censored)
{
	appendStringInfo(profiles,
		"%s{\"metric_kind\":%d,\"profile_key\":{%s},"
		"\"metadata\":%s,\"estimate\":{"
		"\"kind\":%d,\"state\":%d,\"sample_count\":" UINT64_FORMAT ","
		"\"p95_known\":%s,\"uncertainty_known\":false,\"censored\":%s,"
		"\"expected\":%s%s,\"workload_p95\":%s,"
		"\"uncertainty_low\":null,\"uncertainty_high\":null}}",
		*count ? "," : "", metric_id, key_extra, metadata, kind, state,
		sample_count, p95_known ? "true" : "false", censored ? "true" : "false",
		kind == GGC_METRIC_LOWER_BOUND || kind == GGC_METRIC_UNKNOWN ?
			"null" : psprintf("%.17g", value),
		kind == GGC_METRIC_LOWER_BOUND ?
			psprintf(",\"lower_bound\":%.17g", value) : "",
		p95_known ? psprintf("%.17g", p95) : "null");
	(*count)++;
}

static char *
profile_key(const GgcStatsScope *scope, const PathOptions *options,
			const char *population, const char *metric, uint32 stage)
{
	return psprintf(
		"\"population\":\"%s\",\"metric\":\"%s\",\"stage\":%u,"
		"\"hops\":%u,\"direction\":%u,\"path_mode\":%u,"
		"\"result_mode\":%u,\"semantic_fingerprint\":" UINT64_FORMAT ","
		"\"predicate\":{\"clause_id\":%u,\"attnum\":%d,"
		"\"min\":" INT64_FORMAT ",\"max\":" INT64_FORMAT "}",
		population, metric, stage, options->hops, scope->direction,
		scope->path_mode, scope->result_mode, scope->semantic_fingerprint,
		options->predicate_clause_id, options->predicate_attnum,
		options->predicate_min, options->predicate_max);
}

static char *
build_metadata(const GgcStatsScope *scope, const PathOptions *options,
			   TimestampTz started, uint64 population, uint32 sample_count,
			   uint32 complete_depth, const char *reason, uint64 scan_pages,
			   uint64 examined, Size peak, const char *extra)
{
	char	   *started_text = DatumGetCString(DirectFunctionCall1(
		timestamptz_out, TimestampTzGetDatum(started)));
	char	   *finished_text = DatumGetCString(DirectFunctionCall1(
		timestamptz_out, TimestampTzGetDatum(GetCurrentTimestamp())));
	double		effective = sample_count;

	return psprintf(
		"{\"started_at\":\"%s\",\"finished_at\":\"%s\","
		"\"snapshot_source\":\"active_mvcc_provider_tableam\","
		"\"sample_method\":\"%s\","
		"\"truncation_reason\":\"%s\",\"seed\":" UINT64_FORMAT ","
		"\"population\":" UINT64_FORMAT ",\"sample_count\":%u,"
		"\"effective_sample_size\":%.17g,\"complete_depth\":%u,"
		"\"layout_version\":1,\"model_version\":1,"
		"\"change_baseline\":{\"source\":\"explicit_fixed_path_collection\","
		"\"path_mode\":\"%s\",\"semantic_fingerprint\":" UINT64_FORMAT ",\"counters\":%s},"
		"\"resources\":{\"scan_pages\":" UINT64_FORMAT ","
		"\"examined_entries\":" UINT64_FORMAT ",\"peak_bytes\":%zu}%s%s}",
		started_text, finished_text,
		options->conditional ? "registered_singleton_fixed_path" : "uniform_vertex_reservoir_fixed_path",
		reason, options->seed, population,
		sample_count, effective, complete_depth, mode_name(scope->path_mode),
		scope->semantic_fingerprint, options->counter_baseline, scan_pages, examined, peak,
		extra ? extra : "",
		options->conditional ? psprintf(",\"anchor_condition\":{\"version\":1,\"key\":" INT64_FORMAT "}",
			options->anchor_key) : "");
}

static bool
collect_seeds(Relation vertex, AttrNumber key_attnum, const PathOptions *options,
			  TimestampTz started, PathSeed *seeds, uint32 *seed_count,
			  uint64 *population, uint64 *examined)
{
	TableScanDesc scan;
	TupleTableSlot *slot;
	pg_prng_state random;

	pg_prng_seed(&random, options->seed);
	scan = table_beginscan(vertex, GetActiveSnapshot(), 0, NULL, SO_NONE);
	slot = table_slot_create(vertex, NULL);
	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		bool		isnull;
		int64		key;

		if (*examined >= options->max_examined ||
			!path_deadline(started, options->deadline_ms))
		{
			table_endscan(scan);
			ExecDropSingleTupleTableSlot(slot);
			return false;
		}
		key = DatumGetInt64(slot_getattr(slot, key_attnum, &isnull));
		if (isnull)
			elog(ERROR, "null fixed-path vertex key despite mapping validation");
		(*examined)++;
		if (options->conditional && key != options->anchor_key)
		{
			ExecClearTuple(slot);
			continue;
		}
		(*population)++;
		if (options->conditional && *population > 1)
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("registered anchor must identify a unique visible vertex")));
		if (*seed_count < options->seed_target)
			seeds[(*seed_count)++] = (PathSeed) {.key = key};
		else
		{
			uint64 position = pg_prng_uint64_range(&random, 0, *population - 1);

			if (position < options->seed_target)
				seeds[position].key = key;
		}
		ExecClearTuple(slot);
	}
	table_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
	if (*population > 0)
		for (uint32 i = 0; i < *seed_count; i++)
			seeds[i].weight = (double) *population / *seed_count;
	return true;
}

static bool
collect_degrees(Relation edge, AttrNumber endpoint_attnum,
				const PathOptions *options, TimestampTz started,
				HTAB *degrees, uint64 *examined,
				MemoryContext context, Size *peak)
{
	TableScanDesc scan = table_beginscan(edge, GetActiveSnapshot(), 0, NULL, SO_NONE);
	TupleTableSlot *slot = table_slot_create(edge, NULL);
	bool		complete = true;

	while (table_scan_getnextslot(scan, ForwardScanDirection, slot))
	{
		bool		isnull;
		bool		found;
		int64		key;
		DegreeEntry *entry;

		/* Account for scan/slot allocations even on a work/deadline exit. */
		*peak = Max(*peak, MemoryContextMemAllocated(context, true));
		if (*peak > options->max_memory ||
			*examined >= options->max_examined ||
			!path_deadline(started, options->deadline_ms))
		{
			complete = false;
			break;
		}
		key = DatumGetInt64(slot_getattr(slot, endpoint_attnum, &isnull));
		if (isnull)
			elog(ERROR, "null fixed-path endpoint despite mapping validation");
		entry = hash_search(degrees, &key, HASH_ENTER, &found);
		if (!found)
			entry->degree = 0;
		entry->degree++;
		(*examined)++;

		/*
		 * HASH_ENTER may allocate an element block or grow the directory.
		 * Stop after this insertion, not after scanning every distinct key.
		 * As with other collector contexts this is a sampled allocation
		 * budget: at most one hash growth can cross the configured limit.
		 */
		*peak = Max(*peak, MemoryContextMemAllocated(context, true));
		if (*peak > options->max_memory)
		{
			complete = false;
			break;
		}
		ExecClearTuple(slot);
	}
	*peak = Max(*peak, MemoryContextMemAllocated(context, true));
	table_endscan(scan);
	ExecDropSingleTupleTableSlot(slot);
	return complete;
}

static void
accumulate_batch(PathStage *stage, const GgcBatchInstrumentation *batch)
{
	stage->requests += batch->endpoint_requests;
	stage->physical_entries += batch->candidates_received;
	stage->edge_fetches += batch->tableam_fetches;
	stage->heap_page_groups += batch->heap_page_groups;
	stage->tableam_calls += batch->tableam_fetches;
	stage->directory_lookups += batch->provider_directory_lookups;
	stage->ref_direct_lookups += batch->provider_ref_direct_lookups;
	stage->ref_fallbacks += batch->provider_ref_fallbacks;
	stage->slot_dereferences += batch->provider_slot_dereferences;
	stage->peak_restore_bytes = Max(stage->peak_restore_bytes,
		batch->peak_restore_bytes);
}

static void
initialize_request(GgcOpenRequestV2 *request, EState *estate,
				   MemoryContext context, const GgcStatsScope *scope,
				   const PathMapping *mapping, Oid index_relid,
				   Oid opfamily, uint64 build_generation)
{
	MemSet(request, 0, sizeof(*request));
	request->struct_size = sizeof(*request);
	request->abi_version = GGC_ADJ_ABI_VERSION;
	request->estate = estate;
	request->scan_context = context;
	request->resource_owner = CurrentResourceOwner;
	request->snapshot = estate->es_snapshot;
	request->command_id = GetCurrentCommandId(false);
	GetUserIdAndSecContext(&request->effective_role, &request->security_context);
	request->graph_oid = scope->graph_oid;
	request->edge_relid = mapping->edge_relid;
	request->index_relid = index_relid;
	request->source_attnum = mapping->source_key;
	request->destination_attnum = mapping->destination_key;
	request->endpoint_type = INT8OID;
	request->endpoint_opfamily = opfamily;
	request->endpoint_domain = scope->endpoint_domain;
	request->catalog_generation = scope->catalog_generation;
	request->build_generation = build_generation;
	request->direction = scope->direction;
	request->required_caps = GGC_CAP_ROOT_TID | GGC_CAP_COMPLETE_CANDIDATES |
		GGC_CAP_BATCH | GGC_CAP_PAUSE_RESUME | GGC_CAP_SSI;
	request->budget.max_endpoints = PATH_MAX_ENDPOINT_BATCH;
	request->budget.max_candidates = PATH_CANDIDATE_BATCH;
	request->budget.max_inflight_pages = 8;
	request->budget.max_pins = 8;
	request->budget.max_candidate_bytes =
		PATH_CANDIDATE_BATCH * sizeof(GgcAdjItemV2);
	request->budget.max_restore_bytes = 1024 * 1024;
}

static bool
expand_one_stage(PathBinding *current, uint64 current_count,
				 PathBinding *next, uint64 *next_count, uint32 stage_index,
				 PathStage *stage, const PathSeed *seeds, uint32 seed_count,
				 const PathOptions *options, const GgcStatsScope *scope,
				 const PathMapping *mapping, Oid index_relid, Oid opfamily,
				 uint64 build_generation, const GgcAdjProviderV2 *provider,
				 EState *estate, MemoryContext context, HTAB *degrees,
				 TimestampTz started, uint64 *examined, uint64 *next_ordinal,
				 const char **truncation_reason)
{
	MemoryContext stage_context;
	HTAB	   *seed_endpoints;
	HTAB	   *global_endpoints;
	HTAB	   *logical_seen;
	HTAB	   *accepted_seen;
	HTAB	   *set_outputs = NULL;
	uint64	   *seed_outputs;
	GgcBatchOptions batch_options = {0};
	GgcOpenRequestV2 request;
	GgcBatchState *active_batch = NULL;
	bool		complete = true;

	stage_context = AllocSetContextCreate(context, "fixed-path stage profile",
		ALLOCSET_DEFAULT_SIZES);
	seed_endpoints = create_hash("fixed-path seed endpoints",
		sizeof(SeedEndpointKey), sizeof(EndpointEntry), stage_context,
		Max((long) current_count, 16L));
	global_endpoints = create_hash("fixed-path global endpoints",
		sizeof(int64), sizeof(GlobalEndpointEntry), stage_context,
		Max((long) current_count, 16L));
	logical_seen = create_hash("fixed-path logical candidates",
		sizeof(CandidateKey), sizeof(SeenEntry), stage_context, 64);
	accepted_seen = create_hash("fixed-path accepted candidates",
		sizeof(CandidateKey), sizeof(SeenEntry), stage_context, 64);
	if (scope->result_mode == GGC_STATS_ENDPOINT_SETS)
		set_outputs = create_hash("fixed-path endpoint-set outputs",
			sizeof(SeedEndpointKey), sizeof(EndpointEntry), stage_context, 64);
	seed_outputs = MemoryContextAllocZero(stage_context,
		sizeof(*seed_outputs) * seed_count);
	for (uint64 i = 0; i < current_count; i++)
	{
		SeedEndpointKey seed_key = {0};
		int64		global_key = current[i].vertices[current[i].depth];
		bool		found;

		stage->seeds[current[i].seed].bindings++;
		seed_key.seed = current[i].seed;
		seed_key.endpoint = global_key;
		hash_search(seed_endpoints, &seed_key, HASH_ENTER, &found);
		if (!found)
			stage->seeds[current[i].seed].frontier++;
		hash_search(global_endpoints, &global_key, HASH_ENTER, &found);
		if (!found && degree_lookup(degrees, global_key) == 0)
			stage->zero_degree_requests++;
	}
	batch_options.struct_size = sizeof(batch_options);
	batch_options.version = GGC_BATCH_EXEC_VERSION;
	batch_options.prefetch = true;
	batch_options.small_parent_threshold = 2;
	batch_options.small_candidate_limit = 2;
	initialize_request(&request, estate, context, scope, mapping,
		index_relid, opfamily, build_generation);

	PG_TRY();
	{
		for (uint64 start = 0; start < current_count && complete;)
		{
			uint32		count = Min((uint64) options->max_parent_batch,
				current_count - start);
			GgcBatchParent *parents = MemoryContextAllocZero(stage_context,
				sizeof(*parents) * count);
			GgcBatchResult row;
			GgcBatchInstrumentation instrumentation;

			for (uint32 i = 0; i < count; i++)
			{
				PathBinding *binding = &current[start + i];

				parents[i].parent_id = start + i + 1;
				parents[i].parent_ordinal = binding->ordinal;
				parents[i].key = Int64GetDatum(binding->vertices[binding->depth]);
				parents[i].handle = binding->handle;
			}
			active_batch = GgcBatchBegin(provider, &request,
				parents, count, &batch_options);
			while (safe_batch_next(&active_batch, &row))
			{
				PathBinding *parent;
				CandidateKey candidate_key = {0};
				bool		found;
				bool		accepted;
				bool		isnull;
				int64		edge;
				int64		opposite;

				/*
				 * Count every restored candidate, including predicate and path-mode
				 * rejects.  This is a work budget, not an output-row budget.
				 */
				if (*examined >= options->max_examined ||
					!path_deadline(started, options->deadline_ms) ||
					MemoryContextMemAllocated(context, true) > options->max_memory)
				{
					*truncation_reason = *examined >= options->max_examined ?
						"max_examined_entries" :
						MemoryContextMemAllocated(context, true) > options->max_memory ?
						"max_memory_bytes" : "deadline";
					complete = false;
					break;
				}
				(*examined)++;
				if (row.parent_id == 0 || row.parent_id > current_count)
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("fixed-path batch returned an unknown parent")));
				parent = &current[row.parent_id - 1];
				edge = DatumGetInt64(slot_getattr(row.slot,
					mapping->edge_identity, &isnull));
				if (isnull || !row.candidate.opposite_key_available ||
					row.candidate.opposite_isnull)
					ereport(ERROR,
							(errcode(ERRCODE_DATA_CORRUPTED),
							 errmsg("fixed-path candidate lacks identity or opposite endpoint")));
				opposite = DatumGetInt64(row.candidate.opposite_key);
				candidate_key.seed = parent->seed;
				candidate_key.endpoint = parent->vertices[parent->depth];
				candidate_key.edge = edge;
				hash_search(logical_seen, &candidate_key, HASH_ENTER, &found);
				if (!found)
				{
					if (stage->seeds[parent->seed].logical_edges >=
						options->max_entries_per_seed)
					{
						*truncation_reason = "max_entries_per_seed";
						complete = false;
						break;
					}
					stage->seeds[parent->seed].logical_edges++;
				}
				accepted = predicate_passes(row.slot, options);
				if (!accepted)
					continue;
				hash_search(accepted_seen, &candidate_key, HASH_ENTER, &found);
				if (!found)
					stage->seeds[parent->seed].accepted_edges++;
				if (!mode_allows(parent, edge, opposite, parent->depth + 1,
					options->hops, scope->path_mode))
					continue;
				if (set_outputs)
				{
					SeedEndpointKey output_key = {0};

					output_key.seed = parent->seed;
					output_key.endpoint = opposite;
					hash_search(set_outputs, &output_key, HASH_ENTER, &found);
					if (found)
						continue;
				}
				if (*next_count >= options->max_bindings)
				{
					*truncation_reason = "max_bindings";
					complete = false;
					break;
				}
				if (seed_outputs[parent->seed] >= options->max_bindings_per_seed)
				{
					*truncation_reason = "max_bindings_per_seed";
					complete = false;
					break;
				}
				if (*next_ordinal == PG_UINT64_MAX)
					ereport(ERROR,
							(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
							 errmsg("fixed-path collection binding ordinal overflow")));
				next[*next_count] = *parent;
				next[*next_count].ordinal = (*next_ordinal)++;
				next[*next_count].edges[parent->depth] = edge;
				next[*next_count].vertices[parent->depth + 1] = opposite;
				next[*next_count].handle = row.candidate.opposite_handle;
				next[*next_count].depth++;
				(*next_count)++;
				seed_outputs[parent->seed]++;
				stage->seeds[parent->seed].next_bindings++;
				stage->seeds[parent->seed].arrival_degree_sum +=
					degree_lookup(degrees, opposite);
				stage->seeds[parent->seed].arrival_count++;
			}
			if (active_batch)
			{
				GgcBatchGetInstrumentation(active_batch, &instrumentation);
				accumulate_batch(stage, &instrumentation);
				GgcBatchEnd(active_batch);
				active_batch = NULL;
			}
			start += count;
		}
	}
	PG_CATCH();
	{
		if (active_batch)
			GgcBatchEnd(active_batch);
		MemoryContextDelete(stage_context);
		PG_RE_THROW();
	}
	PG_END_TRY();

	/* Distinct next endpoints are a logical property, not a physical request. */
	if (*next_count > 0)
	{
		HTAB *next_endpoints = create_hash("fixed-path next endpoints",
			sizeof(SeedEndpointKey), sizeof(EndpointEntry), stage_context,
			Max((long) *next_count, 16L));

		for (uint64 i = 0; i < *next_count; i++)
		{
			SeedEndpointKey key = {0};
			bool found;

			key.seed = next[i].seed;
			key.endpoint = next[i].vertices[next[i].depth];
			hash_search(next_endpoints, &key, HASH_ENTER, &found);
			if (!found)
				stage->seeds[next[i].seed].next_frontier++;
		}
	}
	stage->partial = !complete;
	MemoryContextDelete(stage_context);
	return complete;
}

static void
summarize_values(const double *values, const bool *defined,
				 const PathSeed *seeds, uint32 seed_count,
				 double *mean, double *p95, uint64 *samples,
				 uint32 *undefined_count)
{
	PathSeed   *selected_seeds = palloc(sizeof(*selected_seeds) * seed_count);
	double	   *selected_values = palloc(sizeof(*selected_values) * seed_count);
	uint32		selected = 0;

	*undefined_count = 0;
	for (uint32 i = 0; i < seed_count; i++)
	{
		if (defined && !defined[i])
		{
			(*undefined_count)++;
			continue;
		}
		selected_seeds[selected] = seeds[i];
		selected_values[selected] = values[i];
		selected++;
	}
	*mean = weighted_mean(selected_values, selected_seeds, selected);
	*p95 = weighted_p95(selected_values, selected_seeds, selected);
	*samples = selected;
	pfree(selected_values);
	pfree(selected_seeds);
}

static void
append_seed_metric(StringInfo profiles, int *profile_count, int metric_id,
				   const char *name, uint32 stage_index,
				   const double *values, const bool *defined,
				   const PathSeed *seeds, uint32 seed_count,
				   const GgcStatsScope *scope, const PathOptions *options,
				   const char *metadata, bool partial)
{
	double		mean;
	double		p95;
	uint64		samples;
	uint32		undefined_count;
	char	   *base_key;
	char	   *key;
	GgcMetricKind kind;
	GgcStatsState state;

	summarize_values(values, defined, seeds, seed_count, &mean, &p95,
		&samples, &undefined_count);
	base_key = profile_key(scope, options,
		options->conditional ? "registered_singleton" : "uniform_vertex_seeds", name,
		stage_index + 1);
	key = psprintf("%s,\"undefined_seed_count\":%u", base_key,
		undefined_count);
	if (samples == 0)
	{
		kind = GGC_METRIC_UNKNOWN;
		state = GGC_STATS_PARTIAL;
		mean = 0;
		p95 = 0;
	}
	else if (partial)
	{
		kind = GGC_METRIC_LOWER_BOUND;
		state = GGC_STATS_PARTIAL;
	}
	else
	{
		kind = options->conditional ? GGC_METRIC_EXACT_AT_SAMPLE : GGC_METRIC_ESTIMATED;
		state = GGC_STATS_READY;
	}
	append_profile(profiles, profile_count, metric_id, key, metadata,
		kind, state, mean, p95, samples > 0 && !partial,
		samples, partial);
}

static void
append_total_metric(StringInfo profiles, int *profile_count, int metric_id,
					const char *population, const char *name,
					uint32 stage_index, double value,
					const GgcStatsScope *scope, const PathOptions *options,
					const char *metadata, bool partial)
{
	char	   *key = profile_key(scope, options, population, name,
		stage_index + 1);

	append_profile(profiles, profile_count, metric_id, key, metadata,
		partial ? GGC_METRIC_LOWER_BOUND : GGC_METRIC_EXACT_AT_SAMPLE,
		partial ? GGC_STATS_PARTIAL : GGC_STATS_READY, value, 0, false,
		1, partial);
}

static Jsonb *
build_logical_profiles(const GgcStatsScope *scope, const PathOptions *options,
					   const PathSeed *seeds, uint32 seed_count,
					   const PathStage *stages, uint32 recorded_stages,
					   uint32 complete_depth, const char *reason,
					   TimestampTz started, uint64 population,
					   uint64 scan_pages, uint64 examined, Size peak,
					   int *profile_count)
{
	StringInfoData profiles;
	char	   *metadata = build_metadata(scope, options, started, population,
		seed_count, complete_depth, reason, scan_pages, examined, peak, NULL);

	initStringInfo(&profiles);
	appendStringInfoChar(&profiles, '[');
	*profile_count = 0;
	for (uint32 stage = 0; stage < recorded_stages; stage++)
	{
		double	   *frontier = palloc(sizeof(double) * seed_count);
		double	   *bindings = palloc(sizeof(double) * seed_count);
		double	   *logical = palloc(sizeof(double) * seed_count);
		double	   *accepted = palloc(sizeof(double) * seed_count);
		double	   *next_bindings = palloc(sizeof(double) * seed_count);
		double	   *next_endpoints = palloc(sizeof(double) * seed_count);
		double	   *overlap = palloc0(sizeof(double) * seed_count);
		double	   *next_ratio = palloc0(sizeof(double) * seed_count);
		double	   *arrival = palloc0(sizeof(double) * seed_count);
		bool	   *overlap_defined = palloc0(sizeof(bool) * seed_count);
		bool	   *ratio_defined = palloc0(sizeof(bool) * seed_count);
		bool	   *arrival_defined = palloc0(sizeof(bool) * seed_count);
		bool		partial = stages[stage].partial;

		for (uint32 seed = 0; seed < seed_count; seed++)
		{
			const SeedStage *s = &stages[stage].seeds[seed];

			frontier[seed] = s->frontier;
			bindings[seed] = s->bindings;
			logical[seed] = s->logical_edges;
			accepted[seed] = s->accepted_edges;
			next_bindings[seed] = s->next_bindings;
			next_endpoints[seed] = s->next_frontier;
			if (s->bindings > 0)
			{
				overlap[seed] = 1.0 - s->frontier / s->bindings;
				overlap_defined[seed] = true;
			}
			if (s->accepted_edges > 0)
			{
				next_ratio[seed] = s->next_frontier / s->accepted_edges;
				ratio_defined[seed] = true;
			}
			if (s->arrival_count > 0)
			{
				arrival[seed] = s->arrival_degree_sum / s->arrival_count;
				arrival_defined[seed] = true;
			}
		}
		/* Input F/B remain exact even when output enumeration was truncated. */
		append_seed_metric(&profiles, profile_count, GGC_METRIC_PATH_FRONTIER,
			"frontier", stage, frontier, NULL, seeds, seed_count,
			scope, options, metadata, false);
		append_seed_metric(&profiles, profile_count, GGC_METRIC_PATH_BINDINGS,
			"bindings", stage, bindings, NULL, seeds, seed_count,
			scope, options, metadata, false);
		append_total_metric(&profiles, profile_count, GGC_METRIC_PATH_REQUESTS,
			"collection_batch", "shared_requests", stage, stages[stage].requests,
			scope, options, metadata, partial);
		append_seed_metric(&profiles, profile_count, GGC_METRIC_PATH_LOGICAL_EDGES,
			"logical_edges", stage, logical, NULL, seeds, seed_count,
			scope, options, metadata, partial);
		append_seed_metric(&profiles, profile_count, GGC_METRIC_PATH_ACCEPTED_EDGES,
			"accepted_edges", stage, accepted, NULL, seeds, seed_count,
			scope, options, metadata, partial);
		append_seed_metric(&profiles, profile_count, GGC_METRIC_PATH_NEXT_BINDINGS,
			"next_bindings", stage, next_bindings, NULL, seeds, seed_count,
			scope, options, metadata, partial);
		if (scope->result_mode == GGC_STATS_ENDPOINT_SETS)
			append_seed_metric(&profiles, profile_count, GGC_METRIC_PATH_NEXT_ENDPOINTS,
				"next_endpoints", stage, next_endpoints, NULL, seeds, seed_count,
				scope, options, metadata, partial);
		append_seed_metric(&profiles, profile_count, GGC_METRIC_PATH_OVERLAP,
			"binding_overlap", stage, overlap, overlap_defined, seeds, seed_count,
			scope, options, metadata, false);
		append_seed_metric(&profiles, profile_count,
			GGC_METRIC_PATH_NEXT_DISTINCT_RATIO, "next_distinct_ratio", stage,
			next_ratio, ratio_defined, seeds, seed_count,
			scope, options, metadata, partial);
		append_seed_metric(&profiles, profile_count, GGC_METRIC_PATH_ARRIVAL_DEGREE,
			"arrival_degree", stage, arrival, arrival_defined, seeds, seed_count,
			scope, options, metadata, partial);
		pfree(arrival_defined);
		pfree(ratio_defined);
		pfree(overlap_defined);
		pfree(arrival);
		pfree(next_ratio);
		pfree(overlap);
		pfree(next_bindings);
		pfree(next_endpoints);
		pfree(accepted);
		pfree(logical);
		pfree(bindings);
		pfree(frontier);
	}
	/* Bounded joint vectors retain correlations; raw endpoint values are absent. */
	for (uint32 seed = 0; seed < seed_count; seed++)
	{
		StringInfoData key;
		bool		joint_partial = complete_depth < recorded_stages;

		initStringInfo(&key);
		/*
		 * The catalog head already carries the complete security/provider
		 * scope.  Keep this per-seed key compact but retain the exact
		 * registered predicate template so it stays below the 512-byte key
		 * budget even at four hops.
		 */
		appendStringInfo(&key,
			"\"population\":\"%s\","
			"\"seed_ordinal\":%u,"
			"\"template\":{\"hops\":%u,\"clause_id\":%u,\"attnum\":%d,"
			"\"min\":" INT64_FORMAT ",\"max\":" INT64_FORMAT "},\"F\":[",
			options->conditional ? "registered_singleton" : "uniform_vertex_seed",
			seed, options->hops, options->predicate_clause_id,
			options->predicate_attnum, options->predicate_min,
			options->predicate_max);
		for (uint32 stage = 0; stage < recorded_stages; stage++)
			appendStringInfo(&key, "%s%.0f", stage ? "," : "",
				stages[stage].seeds[seed].frontier);
		appendStringInfoString(&key, "],\"B\":[");
		for (uint32 stage = 0; stage < recorded_stages; stage++)
			appendStringInfo(&key, "%s%.0f", stage ? "," : "",
				stages[stage].seeds[seed].bindings);
		appendStringInfoString(&key, "],\"E\":[");
		for (uint32 stage = 0; stage < recorded_stages; stage++)
			appendStringInfo(&key, "%s%.0f", stage ? "," : "",
				stages[stage].seeds[seed].logical_edges);
		appendStringInfoString(&key, "],\"A\":[");
		for (uint32 stage = 0; stage < recorded_stages; stage++)
			appendStringInfo(&key, "%s%.0f", stage ? "," : "",
				stages[stage].seeds[seed].accepted_edges);
		appendStringInfoString(&key, "],\"P\":[");
		for (uint32 stage = 0; stage < recorded_stages; stage++)
			appendStringInfo(&key, "%s%.0f", stage ? "," : "",
				stages[stage].seeds[seed].next_bindings);
		appendStringInfoChar(&key, ']');
		append_profile(&profiles, profile_count, GGC_METRIC_PATH_JOINT_VECTOR,
			key.data, metadata,
			joint_partial ? GGC_METRIC_LOWER_BOUND : GGC_METRIC_EXACT_AT_SAMPLE,
			joint_partial ? GGC_STATS_PARTIAL : GGC_STATS_READY,
			seeds[seed].weight, 0, false, 1, joint_partial);
	}
	appendStringInfoChar(&profiles, ']');
	return DatumGetJsonbP(DirectFunctionCall1(jsonb_in,
		CStringGetDatum(profiles.data)));
}

static Jsonb *
build_physical_profiles(const GgcStatsScope *scope, const PathOptions *options,
						const PathStage *stages, uint32 recorded_stages,
						uint32 complete_depth, const char *reason,
						TimestampTz started, uint64 population,
						uint32 seed_count, uint64 scan_pages,
						uint64 examined, Size peak, int *profile_count)
{
	StringInfoData profiles;
	char	   *metadata = build_metadata(scope, options, started, population,
		seed_count, complete_depth, reason, scan_pages, examined, peak, NULL);

	initStringInfo(&profiles);
	appendStringInfoChar(&profiles, '[');
	*profile_count = 0;
	for (uint32 stage = 0; stage < recorded_stages; stage++)
	{
		bool partial = stages[stage].partial;

		append_total_metric(&profiles, profile_count, GGC_METRIC_PHYSICAL_ENTRIES,
			"collection_batch", "physical_entries", stage,
			stages[stage].physical_entries, scope, options, metadata, partial);
		append_total_metric(&profiles, profile_count, GGC_METRIC_EDGE_FETCHES,
			"collection_batch", "edge_fetches", stage,
			stages[stage].edge_fetches, scope, options, metadata, partial);
		append_total_metric(&profiles, profile_count, GGC_METRIC_HEAP_PAGE_GROUPS,
			"collection_batch", "heap_page_groups", stage,
			stages[stage].heap_page_groups, scope, options, metadata, partial);
		append_total_metric(&profiles, profile_count, GGC_METRIC_TABLEAM_CALLS,
			"collection_batch", "tableam_calls", stage,
			stages[stage].tableam_calls, scope, options, metadata, partial);
		append_total_metric(&profiles, profile_count,
			GGC_METRIC_ZERO_DEGREE_REQUESTS, "collection_batch",
			"zero_degree_requests", stage, stages[stage].zero_degree_requests,
			scope, options, metadata, false);
		append_total_metric(&profiles, profile_count,
			GGC_METRIC_PEAK_RESTORE_BYTES, "collection_batch",
			"peak_restore_bytes", stage, stages[stage].peak_restore_bytes,
			scope, options, metadata, false);
	}
	appendStringInfoChar(&profiles, ']');
	return DatumGetJsonbP(DirectFunctionCall1(jsonb_in,
		CStringGetDatum(profiles.data)));
}

static Jsonb *
path_status(const char *status, bool published, const char *reason,
			int64 logical_generation, int64 physical_generation,
			uint32 complete_depth, uint64 population, uint32 seed_count,
			uint64 scan_pages, uint64 examined, Size peak,
			const PathStage *stages, uint32 recorded_stages)
{
	StringInfoData result;

	initStringInfo(&result);
	appendStringInfo(&result,
		"{\"status\":\"%s\",\"published\":%s,\"reason\":\"%s\","
		"\"logical_generation\":" INT64_FORMAT ","
		"\"physical_generation\":" INT64_FORMAT ","
		"\"complete_depth\":%u,\"population\":" UINT64_FORMAT ","
		"\"seed_count\":%u,\"stages\":[",
		status, published ? "true" : "false", reason, logical_generation,
		physical_generation, complete_depth, population, seed_count);
	for (uint32 stage = 0; stage < recorded_stages; stage++)
	{
		double B = 0;
		double F = 0;
		double E = 0;
		double A = 0;
		double P = 0;

		for (uint32 seed = 0; seed < seed_count; seed++)
		{
			B += stages[stage].seeds[seed].bindings;
			F += stages[stage].seeds[seed].frontier;
			E += stages[stage].seeds[seed].logical_edges;
			A += stages[stage].seeds[seed].accepted_edges;
			P += stages[stage].seeds[seed].next_bindings;
		}
		appendStringInfo(&result,
			"%s{\"stage\":%u,\"F\":%.0f,\"B\":%.0f,"
			"\"N\":" UINT64_FORMAT ",\"E\":%.0f,\"A\":%.0f,"
			"\"P\":%.0f,\"X\":" UINT64_FORMAT ","
			"\"H_edge\":" UINT64_FORMAT ",\"heap_page_groups\":" UINT64_FORMAT ","
			"\"directory_lookups\":" UINT64_FORMAT ","
			"\"ref_direct_lookups\":" UINT64_FORMAT ","
			"\"ref_fallbacks\":" UINT64_FORMAT ","
			"\"slot_dereferences\":" UINT64_FORMAT ","
			"\"zero_degree_requests\":" UINT64_FORMAT ",\"partial\":%s}",
			stage ? "," : "", stage + 1, F, B, stages[stage].requests,
			E, A, P, stages[stage].physical_entries,
			stages[stage].edge_fetches, stages[stage].heap_page_groups,
			stages[stage].directory_lookups,
			stages[stage].ref_direct_lookups,
			stages[stage].ref_fallbacks,
			stages[stage].slot_dereferences,
			stages[stage].zero_degree_requests,
			stages[stage].partial ? "true" : "false");
	}
	appendStringInfo(&result,
		"],\"resources\":{\"scan_pages\":" UINT64_FORMAT ","
		"\"examined_entries\":" UINT64_FORMAT ",\"peak_bytes\":%zu}}",
		scan_pages, examined, peak);
	return DatumGetJsonbP(DirectFunctionCall1(jsonb_in,
		CStringGetDatum(result.data)));
}

Datum
pg_graph_statistic_collect_path(PG_FUNCTION_ARGS)
{
	Oid			logical_stats_oid = PG_GETARG_OID(0);
	Oid			physical_stats_oid = PG_GETARG_OID(1);
	PathOptions options = {0};
	GgcStatsScope logical_scope;
	GgcStatsScope physical_scope;
	PathMapping mapping = {0};
	EState	   *estate;
	Snapshot	snapshot;
	MemoryContext collection_context;
	MemoryContext old_context;
	Relation	vertex = NULL;
	Relation	edge = NULL;
	Relation	index = NULL;
	PathSeed   *seeds;
	PathBinding *bindings[2];
	PathStage  *stages;
	HTAB	   *degrees;
	uint64		population = 0;
	uint64		examined = 0;
	uint64		scan_pages;
	uint64		binding_count;
	uint64		next_ordinal = 1;
	uint32		seed_count = 0;
	uint32		current_arena = 0;
	uint32		recorded_stages = 0;
	uint32		complete_depth = 0;
	Size		peak;
	bool		precollection_complete;
	Oid			opfamily;
	uint64		provider_build_generation;
	const GgcAdjProviderV2 *provider;
	Oid			lock_oids[4];
	TimestampTz started = GetCurrentTimestamp();
	TimestampTz lock_started;
	const char *truncation_reason = "none";
	Jsonb	   *logical_profiles;
	Jsonb	   *physical_profiles;
	int			logical_profile_count;
	int			physical_profile_count;
	int64		logical_generation;
	int64		physical_generation;
	Jsonb	   *result;

	parse_path_options(PG_GETARG_JSONB_P(2), &options);
	options.conditional = load_stats_scope(logical_stats_oid, &logical_scope, &options.anchor_key);
	{
		int64 physical_anchor = 0;
		bool physical_conditional = load_stats_scope(physical_stats_oid, &physical_scope, &physical_anchor);

		if (physical_conditional != options.conditional ||
			(options.conditional && (physical_anchor != options.anchor_key || options.seed_target != 1)))
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							errmsg("fixed-path scopes must register the same singleton anchor and seed_target=1")));
	}
	load_path_mapping(&logical_scope, &mapping);
	lock_oids[0] = mapping.graph_relid;
	lock_oids[1] = mapping.vertex_relid;
	lock_oids[2] = mapping.edge_relid;
	lock_oids[3] = physical_scope.index_relid;
	for (int i = 0; i < lengthof(lock_oids); i++)
		for (int j = i + 1; j < lengthof(lock_oids); j++)
			if (lock_oids[j] < lock_oids[i])
			{
				Oid swap = lock_oids[i];

				lock_oids[i] = lock_oids[j];
				lock_oids[j] = swap;
			}
	lock_started = GetCurrentTimestamp();
	for (int i = 0; i < lengthof(lock_oids); i++)
	{
		if (i && lock_oids[i] == lock_oids[i - 1])
			continue;
		if (!path_lock_relation(lock_oids[i], lock_started,
				options.lock_timeout_ms))
			PG_RETURN_JSONB_P(path_status("PARTIAL", false, "lock_unavailable",
				0, 0, 0, 0, 0, 0, 0, 0, NULL, 0));
	}
	graph_stats_check_collect_privileges(&logical_scope);
	graph_stats_check_collect_privileges(&physical_scope);
	validate_path_scopes(&logical_scope, &physical_scope, &mapping, &options);
	options.counter_baseline = graph_stats_counter_baseline(&logical_scope);
	vertex = table_open(mapping.vertex_relid, NoLock);
	edge = table_open(mapping.edge_relid, NoLock);
	index = index_open(physical_scope.index_relid, NoLock);
	if (mapping.vertex_key <= 0 ||
		mapping.vertex_key > RelationGetDescr(vertex)->natts)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("fixed-path vertex key is out of range")));
	{
		Form_pg_attribute key = TupleDescAttr(RelationGetDescr(vertex),
			mapping.vertex_key - 1);

		if (vertex->rd_rel->relkind != RELKIND_RELATION ||
			vertex->rd_rel->relpersistence != RELPERSISTENCE_PERMANENT ||
			vertex->rd_rel->relrowsecurity || RELATION_IS_OTHER_TEMP(vertex) ||
			key->attisdropped || key->atttypid != INT8OID || !key->attnotnull ||
			pg_class_aclcheck(mapping.vertex_relid, GetUserId(), ACL_SELECT) != ACLCHECK_OK)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("fixed-path collection requires a selectable bigint vertex heap without RLS")));
	}
	provider = GgcGetIndexAdjProvider(index);
	if (provider == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("index does not provide graph adjacency access")));
	provider_build_generation =
		(provider->capabilities & GGC_CAP_BUILD_GENERATION) ?
		physical_scope.build_generation : 0;
	opfamily = InvalidOid;
	for (int i = 0; i < index->rd_index->indnkeyatts; i++)
		if (index->rd_index->indkey.values[i] ==
			(logical_scope.direction == GGC_OUT ?
			 mapping.source_key : mapping.destination_key))
		{
			opfamily = index->rd_opfamily[i];
			break;
		}
	scan_pages = (uint64) RelationGetNumberOfBlocks(vertex) +
		(uint64) RelationGetNumberOfBlocks(edge) +
		(uint64) RelationGetNumberOfBlocks(index);
	if (scan_pages > options.max_scan_pages)
	{
		index_close(index, NoLock);
		table_close(edge, NoLock);
		table_close(vertex, NoLock);
		PG_RETURN_JSONB_P(path_status("PARTIAL", false, "scan_page_budget",
			0, 0, 0, 0, 0, scan_pages, 0, 0, NULL, 0));
	}
	if (!ActiveSnapshotSet())
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("fixed-path collection requires an active MVCC snapshot")));
	estate = CreateExecutorState();
	snapshot = RegisterSnapshot(GetActiveSnapshot());
	estate->es_snapshot = snapshot;
	collection_context = AllocSetContextCreate(estate->es_query_cxt,
		"fixed-path graph statistics", ALLOCSET_DEFAULT_SIZES);
	old_context = MemoryContextSwitchTo(collection_context);
	/* Validate even an empty graph: no unsupported index may publish stats. */
	{
		GgcOpenRequestV2 request;
		GgcEndpointRequestV2 endpoint = {0};
		GgcRejectReason reason;

		initialize_request(&request, estate, collection_context, &logical_scope,
			&mapping, physical_scope.index_relid, opfamily, provider_build_generation);
		endpoint.request_id = 1;
		endpoint.key = Int64GetDatum(0);
		request.request_count = 1;
		request.requests = &endpoint;
		if (!provider->validate(&request, &reason))
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("fixed-path graph provider rejected scope (%d)", reason)));
	}
	seeds = palloc0(sizeof(*seeds) * options.seed_target);
	bindings[0] = palloc0(sizeof(PathBinding) * options.max_bindings);
	bindings[1] = palloc0(sizeof(PathBinding) * options.max_bindings);
	stages = palloc0(sizeof(*stages) * options.hops);
	for (uint32 i = 0; i < options.hops; i++)
		stages[i].seeds = palloc0(sizeof(SeedStage) * options.seed_target);
	degrees = create_hash("fixed-path endpoint degrees", sizeof(int64),
		sizeof(DegreeEntry), collection_context, 128);
	peak = MemoryContextMemAllocated(collection_context, true);
	precollection_complete = peak <= options.max_memory &&
		collect_seeds(vertex, mapping.vertex_key, &options, started,
			seeds, &seed_count, &population, &examined) &&
		(!options.conditional || seed_count != 0) &&
		collect_degrees(edge,
			logical_scope.direction == GGC_OUT ? mapping.source_key :
			mapping.destination_key, &options, started, degrees, &examined,
			collection_context, &peak);
	/* Do not let a scan/deadline failure short-circuit peak accounting. */
	peak = Max(peak, MemoryContextMemAllocated(collection_context, true));
	if (!precollection_complete || peak > options.max_memory)
	{
		const char *reason = peak > options.max_memory ? "max_memory_bytes" :
			examined >= options.max_examined ? "max_examined_entries" :
			!path_deadline(started, options.deadline_ms) ? "deadline" : "anchor_not_found";

		MemoryContextSwitchTo(old_context);
		result = path_status("PARTIAL", false, reason, 0, 0, 0,
			population, seed_count, scan_pages, examined, peak, NULL, 0);
		MemoryContextDelete(collection_context);
		UnregisterSnapshot(snapshot);
		FreeExecutorState(estate);
		index_close(index, NoLock);
		table_close(edge, NoLock);
		table_close(vertex, NoLock);
		PG_RETURN_JSONB_P(result);
	}
	for (uint32 seed = 0; seed < seed_count; seed++)
	{
		bindings[0][seed].seed = seed;
		bindings[0][seed].depth = 0;
		bindings[0][seed].ordinal = next_ordinal++;
		bindings[0][seed].vertices[0] = seeds[seed].key;
	}
	binding_count = seed_count;
	for (uint32 stage = 0; stage < options.hops; stage++)
	{
		uint64 next_count = 0;
		bool complete;

		complete = expand_one_stage(bindings[current_arena], binding_count,
			bindings[1 - current_arena], &next_count, stage, &stages[stage],
			seeds, seed_count, &options, &logical_scope, &mapping,
			physical_scope.index_relid, opfamily, provider_build_generation,
			provider, estate, collection_context,
			degrees, started, &examined, &next_ordinal, &truncation_reason);
		recorded_stages = stage + 1;
		peak = Max(peak, MemoryContextMemAllocated(collection_context, true));
		if (!complete)
			break;
		complete_depth = stage + 1;
		binding_count = next_count;
		current_arena = 1 - current_arena;
	}
	logical_profiles = build_logical_profiles(&logical_scope, &options,
		seeds, seed_count, stages, recorded_stages, complete_depth,
		truncation_reason, started, population, scan_pages, examined, peak,
		&logical_profile_count);
	physical_profiles = build_physical_profiles(&physical_scope, &options,
		stages, recorded_stages, complete_depth, truncation_reason, started,
		population, seed_count, scan_pages, examined, peak,
		&physical_profile_count);
	if (logical_profile_count > options.max_profiles ||
		physical_profile_count > options.max_profiles ||
		VARSIZE_ANY(logical_profiles) > options.max_payload ||
		VARSIZE_ANY(physical_profiles) > options.max_payload)
	{
		MemoryContextSwitchTo(old_context);
		result = path_status("PARTIAL", false,
			logical_profile_count > options.max_profiles ||
			physical_profile_count > options.max_profiles ?
			"profile_budget" : "payload_budget", 0, 0, complete_depth,
			population, seed_count, scan_pages, examined, peak,
			stages, recorded_stages);
		MemoryContextDelete(collection_context);
		UnregisterSnapshot(snapshot);
		FreeExecutorState(estate);
		index_close(index, NoLock);
		table_close(edge, NoLock);
		table_close(vertex, NoLock);
		PG_RETURN_JSONB_P(result);
	}
	logical_generation = graph_stats_publish_collected(logical_stats_oid,
		logical_profiles);
	physical_generation = graph_stats_publish_collected(physical_stats_oid,
		physical_profiles);
	MemoryContextSwitchTo(old_context);
	result = path_status(complete_depth == options.hops ? "READY" : "PARTIAL",
		true, truncation_reason, logical_generation, physical_generation,
		complete_depth, population, seed_count, scan_pages, examined, peak,
		stages, recorded_stages);
	MemoryContextDelete(collection_context);
	UnregisterSnapshot(snapshot);
	FreeExecutorState(estate);
	index_close(index, NoLock);
	table_close(edge, NoLock);
	table_close(vertex, NoLock);
	PG_RETURN_JSONB_P(result);
}
