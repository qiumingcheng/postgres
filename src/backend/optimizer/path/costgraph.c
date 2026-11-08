/*-------------------------------------------------------------------------
 * costgraph.c
 *
 * Convert logical topology, provider work and calibrated coefficients into
 * PostgreSQL Cost units.  Sampled predicates have already produced A/P;
 * additional stage predicates reduce the subsequent frontier exactly once.
 * Heap page groups and Table AM CPU are deliberately charged once each.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "optimizer/graphstats.h"
#include "optimizer/optimizer.h"

#define GGC_DEFAULT_DEGREE 10.0

static double
safe_graph_estimate(double value)
{
	if (!isfinite(value) || value <= 1.0)
		return 1.0;
	return Min(value, 1.0e100);
}

static double
scaled_mean(const GgcGraphLoadedValue *value, double anchors)
{
	return safe_graph_estimate(value->value * anchors);
}

static double
scaled_collection_total(const GgcGraphLoadedValue *value, double anchors)
{
	double		samples = Max((double) value->collection_samples, 1.0);

	return safe_graph_estimate(value->value / samples * anchors);
}

static void
default_cost_profile(GgcGraphCostProfile *profile)
{
	MemSet(profile, 0, sizeof(*profile));
	profile->batch_startup = 4.0 * cpu_operator_cost;
	profile->key_probe = 12.0 * cpu_operator_cost;
	profile->ref_deref = 2.0 * cpu_operator_cost;
	profile->adj_item = cpu_operator_cost;
	profile->heap_fetch = cpu_tuple_cost;
	profile->heap_page_group = random_page_cost;
	profile->binding_extend = 2.0 * cpu_operator_cost;
	profile->version = GGC_GRAPH_COST_PROFILE_VERSION;
	profile->calibrated = false;
}

static bool
estimate_graph_path(const GgcGraphEstimateRequest *request,
					MemoryContext planner_context, GgcGraphPathCost *result,
					const int64 *anchor_key)
{
	GgcGraphLoadedPath loaded;
	bool		logical_ready;
	bool		physical_ready;
	bool		cost_ready;
	double		additional_survival = 1.0;

	if (!result)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("missing graph path cost result")));
	if (anchor_key)
		GgcLoadGraphPathStatisticsForAnchor(request, *anchor_key, planner_context, &loaded);
	else
		GgcLoadGraphPathStatistics(request, planner_context, &loaded);
	MemSet(result, 0, sizeof(*result));
	result->stage_count = request->hops;
	result->stages = MemoryContextAllocZero(planner_context,
		(Size) request->hops * sizeof(*result->stages));
	result->logical_generation = loaded.logical_generation;
	result->physical_generation = loaded.physical_generation;
	result->logical_state = loaded.logical_state;
	result->physical_state = loaded.physical_state;
	result->cost_state = loaded.cost_state;
	result->loaded_profiles = loaded.loaded_profiles;
	result->decoded_bytes = loaded.decoded_bytes;
	logical_ready = loaded.logical_state == GGC_STATS_READY;
	physical_ready = loaded.physical_state == GGC_STATS_READY;
	cost_ready = loaded.cost_state == GGC_STATS_READY &&
		loaded.cost_profile.calibrated;
	if (cost_ready)
		result->cost_profile = loaded.cost_profile;
	else
		default_cost_profile(&result->cost_profile);

	for (uint32 stage = 0; stage < request->hops; stage++)
	{
		const GgcGraphLoadedStage *source = &loaded.stages[stage];
		GgcGraphStageCost *target = &result->stages[stage];
		double		anchors = request->anchor_bindings;
		double		distinct_anchors = request->anchor_distinct;
		double		ref_fraction = request->ref_fractions[stage];
		double		fanout = GGC_DEFAULT_DEGREE;
		Cost		probe_cost;
		const GgcGraphStageQualEstimate *quals = request->stage_quals ?
			&request->stage_quals[stage] : NULL;
		double		edge_selectivity = 1.0;
		double		vertex_selectivity = 1.0;
		double		binding_selectivity = 1.0;
		double		edge_qual_rows;
		double		prior_survival = additional_survival;
		double		request_survival = prior_survival;

		if (quals)
		{
			edge_selectivity = quals->edge_selectivity;
			vertex_selectivity = quals->vertex_selectivity;
			binding_selectivity = quals->binding_selectivity;
			if (!isfinite(edge_selectivity) || edge_selectivity < 0 || edge_selectivity > 1 ||
				!isfinite(vertex_selectivity) || vertex_selectivity < 0 || vertex_selectivity > 1 ||
				!isfinite(binding_selectivity) || binding_selectivity < 0 || binding_selectivity > 1)
				ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								errmsg("graph stage selectivity must be between zero and one")));
			/* A matched logical sample already includes this exact predicate.
			 * Missing samples must still apply its ordinary PG selectivity. */
			if (logical_ready && quals->sampled_edge_clause_id != 0 &&
				quals->sampled_edge_clause_id == request->predicate_clause_id)
				edge_selectivity = 1.0;
		}

		if (request->fallback_fanouts &&
			isfinite(request->fallback_fanouts[stage]) &&
			request->fallback_fanouts[stage] > 0)
			fanout = request->fallback_fanouts[stage];

		if (!isfinite(distinct_anchors) || distinct_anchors <= 0)
			distinct_anchors = anchors;
		distinct_anchors = Min(distinct_anchors, anchors);

		if (!isfinite(ref_fraction) || ref_fraction < 0 || ref_fraction > 1)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("graph reference fraction must be between zero and one")));
		if (logical_ready)
		{
			target->frontier = scaled_mean(&source->frontier, anchors);
			target->bindings = scaled_mean(&source->bindings, anchors);
			/* The executor expands every binding but probes identical endpoints
			 * once per batch.  Collection-total physical work therefore scales
			 * with distinct anchor keys, while output multiplicity continues to
			 * scale with anchor bindings. */
			target->requests = scaled_collection_total(&source->requests,
				distinct_anchors);
			target->logical_edges = scaled_mean(&source->logical_edges, anchors);
			target->accepted_edges = scaled_mean(&source->accepted_edges, anchors);
			target->next_bindings = scaled_mean(&source->next_bindings, anchors);
			/* B/P count paths; F/E/A deduplicate (seed, endpoint[, edge]),
			 * while N counts actual endpoint requests across collection batches.
			 * Filtering parallel paths need not remove their shared endpoint.
			 * Without conditional overlap statistics, retain sampled endpoints
			 * and requests up to the surviving binding count. Price E/A with
			 * endpoint survival and physical work with request survival below.
			 * This is a conservative heuristic, not an upper-bound proof:
			 * degree correlation and changed batch packing remain unknown. */
			if (prior_survival != 1.0)
			{
				double raw_frontier = Min(source->frontier.value * anchors, 1.0e100);
				double raw_requests = Min(source->requests.value /
					Max((double) source->requests.collection_samples, 1.0) * distinct_anchors,
					1.0e100);
				double endpoint_survival;

				target->bindings = Min(source->bindings.value * anchors, 1.0e100) * prior_survival;
				target->next_bindings = Min(source->next_bindings.value * anchors, 1.0e100) * prior_survival;
				target->frontier = Min(raw_frontier, target->bindings);
				target->requests = Min(raw_requests, target->bindings);
				endpoint_survival = raw_frontier > 0 ? target->frontier / raw_frontier : 0;
				request_survival = raw_requests > 0 ? target->requests / raw_requests : 0;
				target->logical_edges = Min(source->logical_edges.value * anchors, 1.0e100) * endpoint_survival;
				target->accepted_edges = Min(source->accepted_edges.value * anchors, 1.0e100) * endpoint_survival;
			}
		}
		else
		{
			target->logical_fallback = true;
			target->bindings = stage == 0 ? anchors :
				result->stages[stage - 1].next_bindings;
			target->frontier = stage == 0 ? anchors :
				result->stages[stage - 1].next_endpoints;
			target->requests = target->bindings;
			if (request->endpoint_sets)
				target->requests = target->frontier;
			target->logical_edges = safe_graph_estimate(target->requests * fanout);
			target->accepted_edges = target->logical_edges;
			target->next_bindings = safe_graph_estimate(target->bindings * fanout);
			/* Keep fractional surviving frontiers until the public Path row
			 * estimate is clamped. Re-inflating a selective prefix to one row
			 * on every hop would erase the effect of its earlier predicates. */
			if (quals)
			{
				target->logical_edges = Min(target->requests * fanout, 1.0e100);
				target->accepted_edges = target->logical_edges;
				target->next_bindings = Min(target->bindings * fanout, 1.0e100);
			}
		}
		edge_qual_rows = Max(target->logical_edges, target->next_bindings);
		target->accepted_edges *= edge_selectivity;
		target->vertex_probes = target->next_bindings * edge_selectivity;
		target->binding_qual_rows = target->vertex_probes * vertex_selectivity;
		target->next_bindings = target->binding_qual_rows * binding_selectivity;
		additional_survival *= edge_selectivity * vertex_selectivity * binding_selectivity;
		if (quals)
		{
			target->qual_startup_cost = quals->edge_cost.startup +
				quals->vertex_cost.startup + quals->binding_cost.startup;
			target->qual_cost = target->qual_startup_cost +
				quals->edge_cost.per_tuple * edge_qual_rows +
				quals->vertex_cost.per_tuple * target->vertex_probes +
				quals->binding_cost.per_tuple * target->binding_qual_rows;
		}
		/* Do not report path multiplicity as the output of endpoint-set
		 * execution. Ordinary NDV is only a conservative domain estimate;
		 * it does not capture all cross-hop correlations or authorize DISTINCT
		 * elimination. Keep path counts separately for bag execution. */
		target->next_endpoints = target->next_bindings;
		if (request->endpoint_ndistinct &&
			isfinite(request->endpoint_ndistinct[stage]) &&
			request->endpoint_ndistinct[stage] > 0)
			target->next_endpoints = Min(target->next_endpoints,
				safe_graph_estimate(request->endpoint_ndistinct[stage] * anchors));
		if (request->endpoint_sets && !logical_ready)
			target->next_endpoints = Min(target->next_endpoints, target->accepted_edges);
		if (request->endpoint_sets && logical_ready &&
			request->logical_scope.result_mode == GGC_STATS_ENDPOINT_SETS)
		{
			/* These samples were actually expanded with per-seed endpoint
			 * deduplication. Do not multiply a mean ratio by mean bag counts;
			 * that loses correlations, and is wrong even for a dead end. */
			target->next_endpoints = Min(source->next_endpoints.value * anchors, 1.0e100);
			if (additional_survival != 1.0)
				target->next_endpoints = Min(target->next_endpoints, target->next_bindings);
			if (prior_survival == 1.0)
			{
				target->frontier = Min(source->frontier.value * anchors, 1.0e100);
				target->requests = source->requests.value /
					Max((double) source->requests.collection_samples, 1.0) * distinct_anchors;
			}
		}
		/* K/R is a scope decision from GraphProgram, never reconstructed from a
		 * low-confidence statistic.  It always partitions actual requests. */
		target->ref_requests = target->requests * ref_fraction;
		target->key_requests = target->requests - target->ref_requests;
		if (physical_ready)
		{
			target->physical_entries = scaled_collection_total(
				&source->physical_entries, distinct_anchors);
			target->edge_heap_fetches =
				scaled_collection_total(&source->edge_fetches, distinct_anchors);
			target->heap_page_groups =
				scaled_collection_total(&source->heap_page_groups, distinct_anchors);
			if (logical_ready && request_survival != 1.0)
			{
				/* Preserve true zero counts while scaling; the legacy row floor
				 * in scaled_collection_total is not a physical-work observation. */
				target->physical_entries = Min(source->physical_entries.value /
					Max((double) source->physical_entries.collection_samples, 1.0) * distinct_anchors,
					1.0e100) * request_survival;
				target->edge_heap_fetches = Min(source->edge_fetches.value /
					Max((double) source->edge_fetches.collection_samples, 1.0) * distinct_anchors,
					1.0e100) * request_survival;
				target->heap_page_groups = Min(source->heap_page_groups.value /
					Max((double) source->heap_page_groups.collection_samples, 1.0) * distinct_anchors,
					1.0e100) * request_survival;
			}
			else if (!logical_ready)
			{
				/* Keep the existing missing-logical-statistics fallback. */
				target->physical_entries *= prior_survival;
				target->edge_heap_fetches *= prior_survival;
				target->heap_page_groups *= prior_survival;
			}
			target->peak_restore_bytes = source->peak_restore_bytes.value;
		}
		else
		{
			target->physical_fallback = true;
			target->physical_entries = Max(target->logical_edges, target->requests);
			target->edge_heap_fetches = target->physical_entries;
			/* Conservative missing-locality fallback: one group per fetch. */
			target->heap_page_groups = target->edge_heap_fetches;
			target->peak_restore_bytes = 0;
		}
		probe_cost = result->cost_profile.batch_startup +
			target->key_requests * result->cost_profile.key_probe +
			target->ref_requests * result->cost_profile.ref_deref;
		target->startup_cost = probe_cost + target->qual_startup_cost;
		target->total_cost = probe_cost +
			target->physical_entries * result->cost_profile.adj_item +
			target->edge_heap_fetches * result->cost_profile.heap_fetch +
			target->heap_page_groups * result->cost_profile.heap_page_group +
			(request->endpoint_sets ? target->next_endpoints : target->next_bindings) *
			result->cost_profile.binding_extend + target->qual_cost;
		result->startup_cost += target->startup_cost;
		result->total_cost += target->total_cost;
	}
	result->uncertainty_unknown = !logical_ready || !physical_ready || !cost_ready ||
		loaded.counter_freshness_unknown || loaded.metric_uncertainty_unknown;
	return true;
}

bool
GgcEstimateGraphPath(const GgcGraphEstimateRequest *request,
					 MemoryContext planner_context, GgcGraphPathCost *result)
{
	return estimate_graph_path(request, planner_context, result, NULL);
}

bool
GgcEstimateGraphPathForAnchor(const GgcGraphEstimateRequest *request,
							  int64 anchor_key, MemoryContext planner_context,
							  GgcGraphPathCost *result)
{
	/* Singleton contract: callers must prove that all anchor bindings have
	 * this key. It is not inferred from a global NDV estimate. */
	if (!request || request->anchor_distinct != 1.0)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("conditional graph estimate requires one distinct anchor")));
	return estimate_graph_path(request, planner_context, result, &anchor_key);
}
