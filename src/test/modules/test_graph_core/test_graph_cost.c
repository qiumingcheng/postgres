/* Test-only entry points for planner statistics/cost and bounded feedback. */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/parsenodes.h"
#include "optimizer/graphstats.h"
#include "statistics/graph_stats_feedback.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/rls.h"

PG_FUNCTION_INFO_V1(test_graph_path_cost);
PG_FUNCTION_INFO_V1(test_graph_feedback_buffer);
PG_FUNCTION_INFO_V1(test_graph_feedback_security);
PG_FUNCTION_INFO_V1(test_graph_counter_matches);

Datum
test_graph_counter_matches(PG_FUNCTION_ARGS)
{
	GgcStatsScope scope;
	Jsonb *current;

	if (!superuser())
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						errmsg("counter test requires superuser")));
	graph_stats_scope_decode(PG_GETARG_BYTEA_PP(0), &scope);
	current = DatumGetJsonbP(DirectFunctionCall1(jsonb_in,
		CStringGetDatum(graph_stats_counter_baseline(&scope))));
	/* Explicit unit-test budget, not a production freshness policy. */
	PG_RETURN_BOOL(graph_stats_counters_unchanged(PG_GETARG_JSONB_P(1), current,
		PG_NARGS() > 2 ? PG_GETARG_UINT32(2) : 60000));
}

Datum
test_graph_feedback_security(PG_FUNCTION_ARGS)
{
	Oid			owner;
	int			context;
	bool		saved_rls = row_security;
	volatile bool passed = true;
	GgcGraphFeedbackBuffer *buffer;
	GgcGraphFeedbackSummary summary;
	GgcGraphFeedbackRecord record = {
		.semantic_fingerprint = 1, .stage_count = 1, .complete = true,
		.total_time_us = 10, .actual_output_rows = 1
	};

	if (!superuser())
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						errmsg("feedback security test requires superuser")));
	GetUserIdAndSecContext(&owner, &context);
	if (owner == PG_GETARG_OID(0))
		ereport(ERROR, (errmsg("feedback test requires a different role")));
	buffer = GgcGraphFeedbackCreate(CurrentMemoryContext, 8, 1);
	(void) GgcGraphFeedbackAdd(buffer, &record);
	PG_TRY();
	{
		for (int mode = 0; mode < 3; mode++)
		{
			SetUserIdAndSecContext(mode == 0 ? PG_GETARG_OID(0) : owner,
								  mode == 1 ? context ^ SECURITY_RESTRICTED_OPERATION : context);
			row_security = mode == 2 ? !saved_rls : saved_rls;
			if (GgcGraphFeedbackAdd(buffer, &record))
				passed = false;
			GgcGraphFeedbackSummarize(buffer, &summary);
			if (summary.accepted != 0 || summary.timing_records != 0 ||
				summary.cardinality_records != 0 || summary.dropped != 0)
				passed = false;
		}
	}
	PG_FINALLY();
	{
		SetUserIdAndSecContext(owner, context);
		row_security = saved_rls;
	}
	PG_END_TRY();
	GgcGraphFeedbackSummarize(buffer, &summary);
	PG_RETURN_BOOL(passed && summary.accepted == 1 && summary.dropped == 0 &&
				   summary.complete_records == 1);
}

Datum
test_graph_path_cost(PG_FUNCTION_ARGS)
{
	Oid			graph_oid = PG_GETARG_OID(0);
	Oid			edge_oid = PG_GETARG_OID(1);
	Oid			index_oid = PG_GETARG_OID(2);
	uint32		endpoint_domain = PG_GETARG_INT32(3);
	uint32		path_mode = PG_GETARG_INT32(4);
	uint64		catalog_generation = PG_GETARG_INT64(5);
	uint64		security_generation = PG_GETARG_INT64(6);
	uint64		build_generation = PG_GETARG_INT64(7);
	uint64		fingerprint = PG_GETARG_INT64(8);
	uint32		hops = PG_GETARG_INT32(9);
	uint32		clause_id = PG_GETARG_INT32(10);
	double		anchors = PG_GETARG_FLOAT8(11);
	ArrayType  *fractions_array = PG_GETARG_ARRAYTYPE_P(12);
	uint32		max_profiles = PG_GETARG_INT32(13);
	Size		max_bytes = PG_GETARG_INT64(14);
	Datum	   *datums;
	bool	   *nulls;
	int			count;
	double	   *fractions;
	GgcGraphEstimateRequest request = {0};
	GgcGraphPathCost result;
	StringInfoData output;

	deconstruct_array_builtin(fractions_array, FLOAT8OID, &datums, &nulls,
						  &count);
	if (count != hops)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("reference fraction array must contain one value per hop")));
	fractions = palloc(sizeof(*fractions) * count);
	for (int i = 0; i < count; i++)
	{
		if (nulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("reference fraction cannot be null")));
		fractions[i] = DatumGetFloat8(datums[i]);
	}
	request.logical_scope = (GgcStatsScope) {
		.graph_oid = graph_oid, .edge_element_oid = edge_oid,
		.effective_role = GetUserId(), .endpoint_domain = endpoint_domain,
		.scope_kind = GGC_STATS_LOGICAL, .direction = 1,
		.path_mode = path_mode, .result_mode = 1,
		.catalog_generation = catalog_generation,
		.security_generation = security_generation,
		.semantic_fingerprint = fingerprint
	};
	request.physical_scope = request.logical_scope;
	request.physical_scope.scope_kind = GGC_STATS_PHYSICAL;
	request.physical_scope.index_relid = index_oid;
	request.physical_scope.build_generation = build_generation;
	request.cost_scope = (GgcStatsScope) {
		.effective_role = GetUserId(), .scope_kind = GGC_STATS_COST,
		.semantic_fingerprint = GGC_GRAPH_COST_PROFILE_VERSION
	};
	request.hops = hops;
	request.predicate_clause_id = clause_id;
	request.anchor_bindings = anchors;
	request.anchor_distinct = anchors;
	request.ref_fractions = fractions;
	request.max_profiles = max_profiles;
	request.max_decoded_bytes = max_bytes;
	request.max_statistics_age_ms = 60000; /* unit-test budget only */
	/* Optional test overload models the post-rewrite RLS planner boundary. */
	if (PG_NARGS() > 15 && PG_GETARG_BOOL(15))
	{
		request.planner_root = makeNode(PlannerInfo);
		request.planner_root->parse = makeNode(Query);
		request.planner_root->parse->hasRowSecurity = true;
	}
	/* Optional diagnostic overload: edge/vertex/prefix selectivities for
	 * each stage, followed by exact sampled edge-template IDs (zero=extra).
	 * These are estimator inputs only; no query execution is overridden. */
	if (PG_NARGS() > 17)
	{
		GgcGraphStageQualEstimate *quals = palloc0(sizeof(*quals) * hops);

		if (PG_NARGS() != 19)
			ereport(ERROR, (errmsg("stage cost test requires selectivities and predicate IDs")));
		deconstruct_array_builtin(PG_GETARG_ARRAYTYPE_P(17), FLOAT8OID,
			&datums, &nulls, &count);
		if (count != hops * 3)
			ereport(ERROR, (errmsg("stage cost test requires three selectivities per hop")));
		for (int i = 0; i < count; i++)
		{
			if (nulls[i])
				ereport(ERROR, (errmsg("stage selectivity cannot be null")));
			if (i % 3 == 0)
				quals[i / 3].edge_selectivity = DatumGetFloat8(datums[i]);
			else if (i % 3 == 1)
				quals[i / 3].vertex_selectivity = DatumGetFloat8(datums[i]);
			else
				quals[i / 3].binding_selectivity = DatumGetFloat8(datums[i]);
		}
		deconstruct_array_builtin(PG_GETARG_ARRAYTYPE_P(18), INT4OID,
			&datums, &nulls, &count);
		if (count != hops)
			ereport(ERROR, (errmsg("stage cost test requires one predicate ID per hop")));
		for (int i = 0; i < count; i++)
		{
			if (nulls[i] || DatumGetInt32(datums[i]) < 0)
				ereport(ERROR, (errmsg("stage predicate ID must be nonnegative")));
			quals[i].sampled_edge_clause_id = DatumGetInt32(datums[i]);
		}
		request.stage_quals = quals;
	}
	if (PG_NARGS() > 16)
	{
		request.anchor_distinct = 1.0;
		(void) GgcEstimateGraphPathForAnchor(&request, PG_GETARG_INT64(16),
			CurrentMemoryContext, &result);
	}
	else
		(void) GgcEstimateGraphPath(&request, CurrentMemoryContext, &result);

	initStringInfo(&output);
	appendStringInfo(&output,
		"{\"logical_state\":%d,\"physical_state\":%d,"
		"\"cost_state\":%d,\"calibrated\":%s,\"uncertainty_unknown\":%s,"
		"\"logical_generation\":" UINT64_FORMAT ","
		"\"physical_generation\":" UINT64_FORMAT ","
		"\"cost_generation\":" UINT64_FORMAT ","
		"\"cost_profile_version\":%u,\"loaded_profiles\":%u,"
		"\"decoded_bytes\":%zu,\"startup_cost\":%.17g,"
		"\"total_cost\":%.17g,\"stages\":[",
		result.logical_state, result.physical_state, result.cost_state,
		result.cost_profile.calibrated ? "true" : "false",
		result.uncertainty_unknown ? "true" : "false",
		result.logical_generation, result.physical_generation,
		result.cost_profile.statistics_generation,
		result.cost_profile.version, result.loaded_profiles,
		result.decoded_bytes, result.startup_cost, result.total_cost);
	for (uint32 stage = 0; stage < result.stage_count; stage++)
	{
		GgcGraphStageCost *s = &result.stages[stage];

		appendStringInfo(&output,
			"%s{\"stage\":%u,\"F\":%.17g,\"B\":%.17g,"
			"\"N\":%.17g,\"E\":%.17g,\"A\":%.17g,\"P\":%.17g,"
			"\"X\":%.17g,\"H\":%.17g,\"page_groups\":%.17g,"
			"\"peak_restore_bytes\":%.17g,"
			"\"K\":%.17g,\"R\":%.17g,"
			"\"logical_fallback\":%s,\"physical_fallback\":%s,"
			"\"startup_cost\":%.17g,\"total_cost\":%.17g}",
			stage ? "," : "", stage + 1, s->frontier, s->bindings,
			s->requests, s->logical_edges, s->accepted_edges,
			s->next_bindings, s->physical_entries, s->edge_heap_fetches,
			s->heap_page_groups, s->peak_restore_bytes,
			s->key_requests, s->ref_requests,
			s->logical_fallback ? "true" : "false",
			s->physical_fallback ? "true" : "false",
			s->startup_cost, s->total_cost);
	}
	appendStringInfoString(&output, "]}");
	PG_RETURN_TEXT_P(cstring_to_text(output.data));
}

Datum
test_graph_feedback_buffer(PG_FUNCTION_ARGS)
{
	GgcGraphFeedbackBuffer *buffer =
		GgcGraphFeedbackCreate(CurrentMemoryContext, 2, 64);
	GgcGraphFeedbackRecord complete = {
		.semantic_fingerprint = 1, .stage_count = 4, .complete = true,
		.total_time_us = 100, .actual_output_rows = 8
	};
	GgcGraphFeedbackRecord partial = {
		.semantic_fingerprint = 1, .stage_count = 4, .complete = false,
		.total_time_us = 40, .actual_output_rows = 2
	};
	GgcGraphFeedbackRecord dropped = {
		.semantic_fingerprint = 2, .stage_count = 8, .complete = true,
		.total_time_us = 200, .actual_output_rows = 16
	};
	GgcGraphFeedbackSummary summary;
	bool		first = GgcGraphFeedbackAdd(buffer, &complete);
	bool		second = GgcGraphFeedbackAdd(buffer, &partial);
	bool		third = GgcGraphFeedbackAdd(buffer, &dropped);
	char	   *output;

	GgcGraphFeedbackSummarize(buffer, &summary);
	output = psprintf(
		"{\"first\":%s,\"second\":%s,\"third\":%s,"
		"\"accepted\":%u,\"dropped\":%u,\"complete_records\":%u,"
		"\"incomplete_records\":%u,\"cardinality_records\":%u,"
		"\"timing_records\":%u}",
		first ? "true" : "false", second ? "true" : "false",
		third ? "true" : "false", summary.accepted, summary.dropped,
		summary.complete_records, summary.incomplete_records,
		summary.cardinality_records, summary.timing_records);
	PG_RETURN_TEXT_P(cstring_to_text(output));
}
