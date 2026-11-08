/*-------------------------------------------------------------------------
 * graphpath.c
 *
 * Admission and standard Path construction for graph alternatives.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "nodes/pathnodes.h"
#include "optimizer/graphpath.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"

GgcGraphIndexAllowedHook ggc_graph_index_allowed_hook = NULL;
GgcGraphSubqueryBoundaryHook ggc_graph_subquery_boundary_hook = NULL;
GgcGraphKeepBindingsHook ggc_graph_keep_bindings_hook = NULL;
bool enable_graph_btree = false;
int graph_btree_max_candidates = 8;

bool
GgcGraphKeepSubqueryBoundary(PlannerInfo *root, Query *subquery)
{
	return (subquery->graph_program != NULL || root->parse->graph_program != NULL) &&
		ggc_graph_subquery_boundary_hook != NULL &&
		ggc_graph_subquery_boundary_hook(root, subquery);
}

bool
GgcGraphKeepVariableBindings(PlannerInfo *root)
{
	return root->parse->graph_program != NULL &&
		!root->parse->graph_program_disabled &&
		ggc_graph_keep_bindings_hook != NULL &&
		ggc_graph_keep_bindings_hook(root);
}

static bool
same_restrictinfo_set(List *left, List *right)
{
	if (list_length(left) != list_length(right))
		return false;
	foreach_ptr(RestrictInfo, rinfo, left)
	{
		if (!list_member_ptr(right, rinfo))
			return false;
	}
	return true;
}

static bool
quals_stay_inside_boundary(List *quals, Relids covered_relids)
{
	foreach_ptr(RestrictInfo, rinfo, quals)
	{
		if (!bms_is_subset(rinfo->clause_relids, covered_relids))
			return false;
	}
	return true;
}

const char *
GgcGraphPathRejectReasonName(GgcGraphPathRejectReason reason)
{
	switch (reason)
	{
		case GGC_GRAPH_PATH_ACCEPTED:
			return "accepted";
		case GGC_GRAPH_PATH_NOT_INNER_JOIN:
			return "not-an-inner-join";
		case GGC_GRAPH_PATH_RELIDS_MISMATCH:
			return "covered-relids-mismatch";
		case GGC_GRAPH_PATH_QUALS_MISMATCH:
			return "consumed-quals-mismatch";
		case GGC_GRAPH_PATH_PARAMETERIZATION_MISMATCH:
			return "required-outer-mismatch";
		case GGC_GRAPH_PATH_INVALID_CHILD:
			return "invalid-anchor-child";
		case GGC_GRAPH_PATH_INVALID_ESTIMATE:
			return "invalid-graph-estimate";
	}
	return "unknown";
}

bool
GgcCreateGraphPath(const GgcGraphPathRequest *request,
				   MemoryContext planner_context,
				   GgcGraphPathResult *result)
{
	CustomPath *path;
	List	   *param_quals;
	double		rows;

	Assert(request != NULL);
	Assert(result != NULL);
	MemSet(result, 0, sizeof(*result));

	if (request->jointype != JOIN_INNER || request->sjinfo == NULL)
	{
		result->reject_reason = GGC_GRAPH_PATH_NOT_INNER_JOIN;
		return false;
	}
	if (request->joinrel == NULL ||
		!bms_equal(request->covered_relids, request->joinrel->relids))
	{
		result->reject_reason = GGC_GRAPH_PATH_RELIDS_MISMATCH;
		return false;
	}
	if (!same_restrictinfo_set(request->available_quals,
							   request->consumed_quals) ||
		!quals_stay_inside_boundary(request->consumed_quals,
								request->covered_relids))
	{
		result->reject_reason = GGC_GRAPH_PATH_QUALS_MISMATCH;
		return false;
	}
	if (request->anchor_path == NULL || request->methods == NULL ||
		!bms_is_subset(request->anchor_path->parent->relids,
					   request->covered_relids))
	{
		result->reject_reason = GGC_GRAPH_PATH_INVALID_CHILD;
		return false;
	}
	if (bms_overlap(request->required_outer, request->covered_relids) ||
		!bms_is_subset(request->joinrel->lateral_relids,
					   request->required_outer) ||
		!bms_equal(request->required_outer,
				   PATH_REQ_OUTER(request->anchor_path)))
	{
		result->reject_reason = GGC_GRAPH_PATH_PARAMETERIZATION_MISMATCH;
		return false;
	}
	if (request->estimate.hops == 0 ||
		request->estimate.ref_fractions == NULL ||
		!(request->has_anchor_key ?
		  GgcEstimateGraphPathForAnchor(&request->estimate, request->anchor_key,
									planner_context, &result->cost) :
		  GgcEstimateGraphPath(&request->estimate, planner_context, &result->cost)) ||
		!isfinite(result->cost.startup_cost) ||
		!isfinite(result->cost.total_cost) ||
		result->cost.startup_cost < 0 || result->cost.total_cost < 0)
	{
		result->reject_reason = GGC_GRAPH_PATH_INVALID_ESTIMATE;
		return false;
	}

	path = makeNode(CustomPath);
	path->path.pathtype = T_CustomScan;
	path->path.parent = request->joinrel;
	path->path.pathtarget = request->joinrel->reltarget;
	param_quals = list_copy(request->consumed_quals);
	path->path.param_info = get_joinrel_parampathinfo(request->root,
		request->joinrel,
		request->ppi_outer_path ? request->ppi_outer_path : request->anchor_path,
		request->ppi_inner_path ? request->ppi_inner_path : request->anchor_path,
		request->sjinfo, request->required_outer, &param_quals);
	path->path.parallel_aware = false;
	path->path.parallel_safe = false;
	path->path.parallel_workers = 0;
	rows = result->cost.stage_count > 0 ?
		(request->estimate.endpoint_sets ?
		 result->cost.stages[result->cost.stage_count - 1].next_endpoints :
		 result->cost.stages[result->cost.stage_count - 1].next_bindings) :
		request->anchor_path->rows;
	/* Keep the same row-count convention as ordinary PG paths.  A fractional
	 * estimate otherwise loses add_path's fuzzy-cost tie to an equivalent
	 * DISTINCT path whose estimate_num_groups() rounded the same input. */
	path->path.rows = path->path.param_info ? path->path.param_info->ppi_rows :
		clamp_row_est(rows);
	path->path.disabled_nodes = request->anchor_path->disabled_nodes;
	path->path.startup_cost = request->anchor_path->startup_cost +
		result->cost.startup_cost;
	path->path.total_cost = request->anchor_path->total_cost +
		result->cost.total_cost;
	path->path.pathkeys = NIL;
	path->flags = 0;
	path->custom_paths = list_make1(request->anchor_path);
	path->custom_restrictinfo = param_quals;
	path->custom_private = request->custom_private;
	path->methods = request->methods;

	result->path = path;
	result->reject_reason = GGC_GRAPH_PATH_ACCEPTED;
	return true;
}
