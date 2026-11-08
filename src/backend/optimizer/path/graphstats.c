/*-------------------------------------------------------------------------
 * graphstats.c
 *
 * Bounded planner reads of the independent graph statistics catalogs.  This
 * Payload reads use index scans. Relation opens only check metadata/ACLs;
 * this code never scans graph data, invokes a provider or traverses an edge.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/detoast.h"
#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/pg_graph_statistic.h"
#include "catalog/pg_graph_statistic_data.h"
#include "nodes/parsenodes.h"
#include "nodes/plannodes.h"
#include "optimizer/graphstats.h"
#include "miscadmin.h"
#include "statistics/graph_stats.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/jsonb.h"
#include "utils/memutils.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

static void
record_graph_statistics_dependency(PlannerInfo *root, Oid stats_oid)
{
	PlanInvalItem *item;
	uint32		hash;

	if (root == NULL || root->glob == NULL || !OidIsValid(stats_oid))
		return;
	hash = GetSysCacheHashValue1(GRAPHSTATISTICOID,
		ObjectIdGetDatum(stats_oid));
	foreach_ptr(PlanInvalItem, existing, root->glob->invalItems)
	{
		if (existing->cacheId == GRAPHSTATISTICOID &&
			existing->hashValue == hash)
			return;
	}
	item = makeNode(PlanInvalItem);
	item->cacheId = GRAPHSTATISTICOID;
	item->hashValue = hash;
	root->glob->invalItems = lappend(root->glob->invalItems, item);
}

static void
record_graph_scope_dependencies(PlannerInfo *root, const GgcStatsScope *scope)
{
	List *relations;

	if (root == NULL || root->glob == NULL)
		return;
	/* Even absence is role-specific: SET ROLE must not reuse another role's
	 * estimates. ANALYZE/DDL and graph publication use normal relcache plan
	 * invalidation. Ordinary DML does not force a generic-plan rebuild. */
	root->glob->dependsOnRole = true;
	/* A shared marker for graph statistics consumers, including logical-only
	 * and missing scopes. Role-membership/attribute callbacks use it to revoke
	 * cached population visibility without invalidating unrelated plans.
	 * Explicit cost-scope creation also uses it: an absent cost scope has no
	 * object or graph OID on which its first publication can invalidate. */
	root->glob->relationOids = list_append_unique_oid(root->glob->relationOids,
		GraphStatisticRelationId);
	relations = graph_stats_relation_oids(scope);
	foreach_oid(relid, relations)
		root->glob->relationOids = list_append_unique_oid(root->glob->relationOids,
			relid);
	list_free(relations);
}

typedef enum LoadKind
{
	LOAD_LOGICAL,
	LOAD_PHYSICAL,
	LOAD_COST
} LoadKind;

static JsonbValue *
json_field(Jsonb *object, const char *name)
{
	JsonbValue key;

	if (!object || !JB_ROOT_IS_OBJECT(object))
		return NULL;
	key.type = jbvString;
	key.val.string.val = (char *) name;
	key.val.string.len = strlen(name);
	return findJsonbValueFromContainer(&object->root, JB_FOBJECT, &key);
}

static bool
json_uint64(Jsonb *object, const char *name, uint64 *result)
{
	JsonbValue *value = json_field(object, name);
	int64		number;

	if (!value || value->type != jbvNumeric)
		return false;
	number = DatumGetInt64(DirectFunctionCall1(numeric_int8,
									 NumericGetDatum(value->val.numeric)));
	if (number < 0)
		return false;
	*result = (uint64) number;
	return true;
}

static Jsonb *
ggc_json_subobject(Jsonb *object, const char *name)
{
	JsonbValue *value = json_field(object, name);
	Jsonb	   *result;

	if (!value)
		return NULL;
	result = JsonbValueToJsonb(value);
	if (!JB_ROOT_IS_OBJECT(result))
		return NULL;
	return result;
}

static Datum
bounded_value(Relation relation, HeapTuple tuple, int attnum, Size maximum)
{
	bool		isnull;
	Datum		value = heap_getattr(tuple, attnum,
								  RelationGetDescr(relation), &isnull);

	if (isnull || toast_raw_datum_size(value) > maximum)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("invalid bounded graph statistics catalog value")));
	return value;
}

static bool
same_bytes(const bytea *left, const bytea *right)
{
	return VARSIZE_ANY_EXHDR(left) == VARSIZE_ANY_EXHDR(right) &&
		memcmp(VARDATA_ANY(left), VARDATA_ANY(right),
			   VARSIZE_ANY_EXHDR(left)) == 0;
}

static void
merge_state(GgcStatsState *aggregate, const GgcMetricEstimate *metric)
{
	GgcStatsState state = metric->state;

	if (state != GGC_STATS_STALE && state != GGC_STATS_INVALID &&
		(metric->kind == GGC_METRIC_UNKNOWN ||
		 metric->kind == GGC_METRIC_LOWER_BOUND || metric->censored))
		state = GGC_STATS_PARTIAL;
	if (state == GGC_STATS_INVALID || *aggregate == GGC_STATS_INVALID)
		*aggregate = GGC_STATS_INVALID;
	else if (state == GGC_STATS_STALE || *aggregate == GGC_STATS_STALE)
		*aggregate = GGC_STATS_STALE;
	else if (state != GGC_STATS_READY || *aggregate == GGC_STATS_PARTIAL)
		*aggregate = GGC_STATS_PARTIAL;
	else if (*aggregate == GGC_STATS_ABSENT)
		*aggregate = GGC_STATS_READY;
}

static bool
path_key_matches(Jsonb *key, const GgcGraphEstimateRequest *request,
				 uint32 *stage)
{
	Jsonb	   *predicate;
	uint64		value;

	if (!json_uint64(key, "stage", &value) || value == 0 ||
		value > request->hops)
		return false;
	*stage = value - 1;
	if (!json_uint64(key, "hops", &value) || value != request->hops)
		return false;
	predicate = ggc_json_subobject(key, "predicate");
	if (!predicate || !json_uint64(predicate, "clause_id", &value) ||
		value != request->predicate_clause_id)
		return false;
	return true;
}

static GgcGraphLoadedValue *
path_destination(GgcGraphLoadedPath *loaded, LoadKind kind, int metric,
				 uint32 stage)
{
	GgcGraphLoadedStage *s = &loaded->stages[stage];

	if (kind == LOAD_LOGICAL)
	{
		switch (metric)
		{
			case GGC_METRIC_PATH_FRONTIER:
				return &s->frontier;
			case GGC_METRIC_PATH_BINDINGS:
				return &s->bindings;
			case GGC_METRIC_PATH_REQUESTS:
				return &s->requests;
			case GGC_METRIC_PATH_LOGICAL_EDGES:
				return &s->logical_edges;
			case GGC_METRIC_PATH_ACCEPTED_EDGES:
				return &s->accepted_edges;
			case GGC_METRIC_PATH_NEXT_BINDINGS:
				return &s->next_bindings;
			case GGC_METRIC_PATH_NEXT_ENDPOINTS:
				return &s->next_endpoints;
		}
	}
	else if (kind == LOAD_PHYSICAL)
	{
		switch (metric)
		{
			case GGC_METRIC_PHYSICAL_ENTRIES:
				return &s->physical_entries;
			case GGC_METRIC_EDGE_FETCHES:
				return &s->edge_fetches;
			case GGC_METRIC_HEAP_PAGE_GROUPS:
				return &s->heap_page_groups;
			case GGC_METRIC_PEAK_RESTORE_BYTES:
				return &s->peak_restore_bytes;
		}
	}
	return NULL;
}

static double *
cost_destination(GgcGraphCostProfile *profile, int metric)
{
	switch (metric)
	{
		case GGC_METRIC_COST_BATCH_STARTUP:
			return &profile->batch_startup;
		case GGC_METRIC_COST_KEY_PROBE:
			return &profile->key_probe;
		case GGC_METRIC_COST_REF_DEREF:
			return &profile->ref_deref;
		case GGC_METRIC_COST_ADJ_ITEM:
			return &profile->adj_item;
		case GGC_METRIC_COST_HEAP_FETCH:
			return &profile->heap_fetch;
		case GGC_METRIC_COST_HEAP_PAGE_GROUP:
			return &profile->heap_page_group;
		case GGC_METRIC_COST_BINDING_EXTEND:
			return &profile->binding_extend;
	}
	return NULL;
}

static void
load_scope(const GgcStatsScope *scope, LoadKind kind,
		   const GgcGraphEstimateRequest *request, Snapshot snapshot,
		   GgcGraphLoadedPath *loaded, uint32 max_profiles, Size max_bytes,
		   const int64 *anchor_key)
{
	Relation	head;
	Relation	data;
	ScanKeyData head_key;
	ScanKeyData data_keys[2];
	SysScanDesc head_scan;
	SysScanDesc data_scan;
	HeapTuple	tuple;
	bytea	   *encoded = graph_stats_scope_encode(scope);
	Oid			stats_oid;
	int64		generation;
	GgcStatsState *aggregate = kind == LOAD_LOGICAL ? &loaded->logical_state :
		kind == LOAD_PHYSICAL ? &loaded->physical_state : &loaded->cost_state;
	uint32		matched = 0;
	uint32		scanned = 0;
	Jsonb	   *current_counters = NULL;

	if (scope->effective_role != GetUserId())
	{
		*aggregate = GGC_STATS_INVALID;
		return;
	}
	record_graph_scope_dependencies(request->planner_root, scope);
	head = table_open(GraphStatisticRelationId, AccessShareLock);
	ScanKeyInit(&head_key, Anum_pg_graph_statistic_gstscope,
				BTEqualStrategyNumber, F_BYTEAEQ, PointerGetDatum(encoded));
	head_scan = systable_beginscan(head, GraphStatisticScopeIndexId, true,
								   snapshot, 1, &head_key);
	tuple = systable_getnext(head_scan);
	if (!HeapTupleIsValid(tuple))
	{
		systable_endscan(head_scan);
		table_close(head, AccessShareLock);
		return;
	}
	stats_oid = ((Form_pg_graph_statistic) GETSTRUCT(tuple))->oid;
	generation = ((Form_pg_graph_statistic) GETSTRUCT(tuple))->gstgeneration;
	record_graph_statistics_dependency(request->planner_root, stats_oid);
	if (generation <= 0 ||
		!same_bytes(encoded, DatumGetByteaPP(bounded_value(head, tuple,
			Anum_pg_graph_statistic_gstscope, 80 + VARHDRSZ))))
	{
		systable_endscan(head_scan);
		table_close(head, AccessShareLock);
		return;
	}
	if (!graph_stats_dependencies_current(scope,
			DatumGetByteaPP(bounded_value(head, tuple,
				Anum_pg_graph_statistic_gstdependencies,
				GGC_STATS_MAX_METADATA_BYTES))))
	{
		*aggregate = GGC_STATS_STALE;
		systable_endscan(head_scan);
		table_close(head, AccessShareLock);
		return;
	}
	if (kind != LOAD_COST)
	{
		int64 registered_key = 0;
		Jsonb *options = DatumGetJsonbP(bounded_value(head, tuple,
			Anum_pg_graph_statistic_gstoptions, 8192));
		bool conditional = graph_stats_anchor_condition(options, &registered_key);

		/* Never substitute a global mean for a requested conditional profile,
		 * or let historical callers consume singleton data as global data. */
		if (conditional != (anchor_key != NULL) ||
			(conditional && registered_key != *anchor_key))
		{
			systable_endscan(head_scan);
			table_close(head, AccessShareLock);
			return;
		}
	}
	systable_endscan(head_scan);
	table_close(head, AccessShareLock);

	data = table_open(GraphStatisticDataRelationId, AccessShareLock);
	ScanKeyInit(&data_keys[0], Anum_pg_graph_statistic_data_gstdstatid,
				BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(stats_oid));
	ScanKeyInit(&data_keys[1], Anum_pg_graph_statistic_data_gstdgeneration,
				BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum(generation));
	data_scan = systable_beginscan(data, GraphStatisticDataProfileIndexId, true,
								   snapshot, 2, data_keys);
	while (HeapTupleIsValid(tuple = systable_getnext(data_scan)))
	{
		Form_pg_graph_statistic_data form =
			(Form_pg_graph_statistic_data) GETSTRUCT(tuple);
		Jsonb	   *key;
		Jsonb	   *metadata;
		bytea	   *payload;
		GgcMetricEstimate metric;
		GgcGraphLoadedValue *destination = NULL;
		double	   *cost = NULL;
		uint32		stage = 0;
		uint64		collection_samples = 1;
		uint64		complete_depth = 0;
		uint64		profile_version = 0;
		GgcStatsState freshness = GGC_STATS_READY;

		CHECK_FOR_INTERRUPTS();
		if (++scanned > GGC_STATS_MAX_PROFILES ||
			form->gstdformat != GGC_STATS_FORMAT_VERSION)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("bounded graph statistics planner load exceeded its profile budget")));
		key = DatumGetJsonbP(bounded_value(data, tuple,
			Anum_pg_graph_statistic_data_gstdkey,
			GGC_STATS_MAX_PROFILE_KEY_BYTES));
		metadata = DatumGetJsonbP(bounded_value(data, tuple,
			Anum_pg_graph_statistic_data_gstdmetadata,
			GGC_STATS_MAX_METADATA_BYTES));
		payload = DatumGetByteaPP(bounded_value(data, tuple,
			Anum_pg_graph_statistic_data_gstdpayload, 52 + VARHDRSZ));
		loaded->decoded_bytes += VARSIZE_ANY(key) + VARSIZE_ANY(metadata) +
			VARSIZE_ANY(payload);
		if (loaded->decoded_bytes > max_bytes)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("bounded graph statistics planner load exceeded its byte budget")));
		graph_stats_metric_decode(payload, &metric);
		if (kind == LOAD_COST)
		{
			cost = cost_destination(&loaded->cost_profile, form->gstdmetric);
			if (!cost || !json_uint64(key, "cost_profile_version", &profile_version) ||
				profile_version != GGC_GRAPH_COST_PROFILE_VERSION)
				continue;
		}
		else
		{
			int64 profile_anchor = 0;
			bool conditional = graph_stats_anchor_condition(metadata, &profile_anchor);

			if (conditional != (anchor_key != NULL) ||
				(conditional && profile_anchor != *anchor_key))
				continue;
			if (!path_key_matches(key, request, &stage))
				continue;
			destination = path_destination(loaded, kind, form->gstdmetric, stage);
			if (!destination)
				continue;
			/* A changed counter invalidates the estimate itself. Missing counters
			 * cannot establish freshness. Neither case may retain READY values
			 * merely with an uncertainty annotation. No wall-clock TTL applies. */
			if (current_counters == NULL)
				current_counters = DatumGetJsonbP(DirectFunctionCall1(jsonb_in,
					CStringGetDatum(graph_stats_counter_baseline(scope))));
			freshness = graph_stats_counter_state(
				ggc_json_subobject(ggc_json_subobject(metadata, "change_baseline"),
					"counters"), current_counters);
			if (freshness != GGC_STATS_READY)
			{
				loaded->counter_freshness_unknown = true;
				metric.state = freshness;
			}
			(void) json_uint64(metadata, "sample_count", &collection_samples);
			(void) json_uint64(metadata, "complete_depth", &complete_depth);
			if (complete_depth < stage + 1)
			{
				if (metric.state != GGC_STATS_STALE && metric.state != GGC_STATS_INVALID)
					metric.state = GGC_STATS_PARTIAL;
				metric.censored = true;
			}
		}
		/* Unchanged data counters do not supply a statistical confidence
		 * interval. Preserve the matched profile's uncertainty contract. */
		if (!metric.uncertainty_known)
			loaded->metric_uncertainty_unknown = true;
		if (++loaded->loaded_profiles > max_profiles)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("bounded graph statistics planner load exceeded its selected profile budget")));
		merge_state(aggregate, &metric);
		matched++;
		if (kind == LOAD_COST)
			*cost = metric.expected;
		else if (freshness == GGC_STATS_READY)
		{
			destination->value = metric.expected;
			destination->sample_count = metric.sample_count;
			destination->collection_samples = Max(collection_samples, UINT64CONST(1));
			destination->kind = metric.kind;
			destination->state = metric.state;
			destination->censored = metric.censored;
			destination->present = true;
		}
	}
	systable_endscan(data_scan);
	table_close(data, AccessShareLock);
	if (kind == LOAD_LOGICAL)
		loaded->logical_generation = generation;
	else if (kind == LOAD_PHYSICAL)
		loaded->physical_generation = generation;
	else
	{
		loaded->cost_profile.statistics_generation = generation;
		loaded->cost_profile.version = GGC_GRAPH_COST_PROFILE_VERSION;
	}
	if (matched == 0)
		*aggregate = GGC_STATS_ABSENT;
	else if (*aggregate == GGC_STATS_READY &&
			 ((kind == LOAD_LOGICAL && matched != request->hops *
			  (scope->result_mode == GGC_STATS_ENDPOINT_SETS ? 7 : 6)) ||
			 (kind == LOAD_PHYSICAL && matched != request->hops * 4) ||
			 (kind == LOAD_COST && matched != 7)))
		*aggregate = GGC_STATS_PARTIAL;
}

static bool
stage_value_ready(const GgcGraphLoadedValue *value)
{
	return value->present && value->state == GGC_STATS_READY &&
		(value->kind == GGC_METRIC_EXACT_AT_SAMPLE ||
		 value->kind == GGC_METRIC_ESTIMATED) && !value->censored;
}

static void
load_graph_path_statistics(const GgcGraphEstimateRequest *request,
						   MemoryContext planner_context,
						   GgcGraphLoadedPath *loaded, const int64 *anchor_key,
						   bool logical_only)
{
	Snapshot	snapshot;
	uint32		max_profiles;
	Size		max_bytes;

	if (!request || !planner_context || !loaded || request->hops == 0 ||
		request->anchor_bindings <= 0 || !request->ref_fractions ||
		request->logical_scope.scope_kind != GGC_STATS_LOGICAL ||
		request->physical_scope.scope_kind != GGC_STATS_PHYSICAL ||
		request->cost_scope.scope_kind != GGC_STATS_COST)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid graph statistics planner load request")));
	max_profiles = request->max_profiles ? request->max_profiles :
		GGC_GRAPH_STATS_DEFAULT_MAX_PROFILES;
	max_bytes = request->max_decoded_bytes ? request->max_decoded_bytes :
		GGC_GRAPH_STATS_DEFAULT_MAX_BYTES;
	if (max_profiles > GGC_STATS_MAX_PROFILES ||
		max_bytes > GGC_STATS_MAX_PAYLOAD_BYTES ||
		!AllocSizeIsValid((Size) request->hops * sizeof(GgcGraphLoadedStage)))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unbounded graph statistics planner load request")));
	MemSet(loaded, 0, sizeof(*loaded));
	loaded->stage_count = request->hops;
	loaded->stages = MemoryContextAllocZero(planner_context,
		(Size) request->hops * sizeof(*loaded->stages));
	/*
	 * V1 statistics describe the global graph population.  Do not even open
	 * their payload rows for a query whose rewrite contains RLS quals: a role
	 * scoped population model has not been implemented yet.  The estimator
	 * will use its bounded low-confidence defaults instead.
	 */
	if (request->planner_root != NULL && request->planner_root->parse != NULL &&
		request->planner_root->parse->hasRowSecurity)
	{
		loaded->logical_state = GGC_STATS_INVALID;
		loaded->physical_state = GGC_STATS_INVALID;
		loaded->cost_state = GGC_STATS_INVALID;
		loaded->cost_profile.version = GGC_GRAPH_COST_PROFILE_VERSION;
		return;
	}
	snapshot = GetActiveSnapshot();
	load_scope(&request->logical_scope, LOAD_LOGICAL, request, snapshot,
		loaded, max_profiles, max_bytes, anchor_key);
	if (!logical_only)
	{
		load_scope(&request->physical_scope, LOAD_PHYSICAL, request, snapshot,
			loaded, max_profiles, max_bytes, anchor_key);
		load_scope(&request->cost_scope, LOAD_COST, request, snapshot,
			loaded, max_profiles, max_bytes, NULL);
	}

	for (uint32 stage = 0; stage < request->hops; stage++)
	{
		GgcGraphLoadedStage *s = &loaded->stages[stage];

		if (!stage_value_ready(&s->frontier) ||
			!stage_value_ready(&s->bindings) ||
			!stage_value_ready(&s->requests) ||
			!stage_value_ready(&s->logical_edges) ||
			!stage_value_ready(&s->accepted_edges) ||
			!stage_value_ready(&s->next_bindings) ||
			(request->logical_scope.result_mode == GGC_STATS_ENDPOINT_SETS &&
			 !stage_value_ready(&s->next_endpoints)))
		{
			if (loaded->logical_state == GGC_STATS_READY)
				loaded->logical_state = GGC_STATS_PARTIAL;
		}
		if (!stage_value_ready(&s->physical_entries) ||
			!stage_value_ready(&s->edge_fetches) ||
			!stage_value_ready(&s->heap_page_groups) ||
			!stage_value_ready(&s->peak_restore_bytes))
		{
			if (loaded->physical_state == GGC_STATS_READY)
				loaded->physical_state = GGC_STATS_PARTIAL;
		}
	}
	loaded->cost_profile.calibrated = loaded->cost_state == GGC_STATS_READY;
}

void
GgcLoadGraphPathStatistics(const GgcGraphEstimateRequest *request,
						   MemoryContext planner_context, GgcGraphLoadedPath *loaded)
{
	load_graph_path_statistics(request, planner_context, loaded, NULL, false);
}

void
GgcLoadGraphPathStatisticsForAnchor(const GgcGraphEstimateRequest *request,
									 int64 anchor_key, MemoryContext planner_context,
									 GgcGraphLoadedPath *loaded)
{
	load_graph_path_statistics(request, planner_context, loaded, &anchor_key, false);
}

void
GgcLoadGraphLogicalStatisticsForAnchor(const GgcGraphEstimateRequest *request,
										 int64 anchor_key, MemoryContext planner_context,
										 GgcGraphLoadedPath *loaded)
{
	load_graph_path_statistics(request, planner_context, loaded, &anchor_key, true);
}
