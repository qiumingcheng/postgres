/* SQL/PGQ planning and Custom Scan integration for the fixed-N runtime. */
#include "postgres.h"

#include <float.h>
#include <math.h>

#include "access/htup_details.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/genam.h"
#include "access/nbtree.h"
#include "commands/explain.h"
#include "commands/explain_format.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_type_d.h"
#include "catalog/pg_operator_d.h"
#include "executor/executor.h"
#include "executor/graph_pgq.h"
#include "executor/graph_pgq_plan.h"
#include "executor/graph_fixed.h"
#include "executor/graph_access.h"
#include "executor/graph_index_capabilities.h"
#include "executor/graph_visible_adj.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/cost.h"
#include "optimizer/graphpath.h"
#include "optimizer/plancat.h"
#include "optimizer/geqo.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/paths.h"
#include "optimizer/planner.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"
#include "parser/parsetree.h"
#include "parser/parse_relation.h"
#include "portability/instr_time.h"
#include "utils/array.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/index_selfuncs.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"

static const GgcGraphDiagnosticOps *diagnostic_ops;

/* Resolve the actual AM. The common layer links to no concrete provider. */
static const GgcAdjProviderV2 *
graph_index_provider(Oid indexid)
{
	Relation index = index_open(indexid, AccessShareLock);
	const GgcAdjProviderV2 *provider = GgcGetIndexAdjProvider(index);

	index_close(index, AccessShareLock);
	if (!provider)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("index does not provide graph adjacency access")));
	return provider;
}

/* Fault injection exists only when the explicit test module registers it. */
static const GgcAdjProviderV2 *
graph_diagnostic_wrapper(const GgcAdjProviderV2 *provider, int mode, int pause_ms)
{
	if (!diagnostic_ops)
		elog(ERROR, "graph diagnostics are not registered");
	return diagnostic_ops->wrap(provider, mode, pause_ms);
}

static Oid pgq_vertex_index(Oid vertex_relid, AttrNumber key_attnum);
static GgcVariable *pgq_program_variable(GgcFixedProgram *program, int variable_id);
static GgcVariable *pgq_rti_variable(GgcFixedProgram *program, Index rti);
static AttrNumber pgq_key_attnum(GgcFixedProgram *program, Index rti);
static AttrNumber pgq_payload_attnum(GgcVariable *edge);

/* Chosen once while costing, then copied verbatim into the executable plan.
 * Do not reselect by catalog OID during serialization or ExecutorStart. */
typedef struct PgqStageIndexes
{
	Oid			edge_index;
	Oid			reverse_index;
	Oid			vertex_index;
} PgqStageIndexes;

static bool
pgq_key_type_supported(Oid type)
{
	return type == INT8OID ||
		(!enable_graph_age_adjacency && GgcIntegerKeyType(type));
}

/* Extended PGQ stage metadata; legacy diagnostic stages retain 13/15/16
 * fields. The reverse index slot is present even for directed stages. */
enum
{
	PGQ_STAGE_REVERSE = 15,
	PGQ_STAGE_IDENTITY,
	PGQ_STAGE_SOURCE,
	PGQ_STAGE_DESTINATION,
	PGQ_STAGE_PAYLOAD,
	PGQ_STAGE_VERTEX_KEY,
	PGQ_STAGE_FIELDS
};

static bool
pgq_stage_undirected(List *stage)
{
	return list_length(stage) > PGQ_STAGE_REVERSE &&
		OidIsValid((Oid) intVal(list_nth(stage, PGQ_STAGE_REVERSE)));
}
static bool graph_edge_probe_batch = true;
static bool graph_vertex_probe_batch = true;
static bool graph_endpoint_pipeline = true;
static bool graph_edge_range_probe = false;
static bool graph_vertex_range_probe = false;
static bool graph_vertex_pointer_sort = false;
static bool graph_btree_seek_hint = false;
static bool graph_btree_batch_route = false;
static bool graph_btree_int8_compare = false;
static int graph_endpoint_batch_size = 8;
static int graph_vertex_batch_size = 32;
static bool graph_edge_index_only = true;

static double
test_anchor_distinct(PlannerInfo *root, Index request_rti, double rows)
{
	Var		   *anchor = makeVar(request_rti,
							pgq_key_attnum(root->parse->graph_program, request_rti),
							get_atttype(planner_rt_fetch(request_rti, root)->relid,
								pgq_key_attnum(root->parse->graph_program, request_rti)), -1,
							InvalidOid, 0);
	double		distinct = estimate_num_groups(root, list_make1(anchor), rows,
											 NULL, NULL);

	return clamp_row_est(Min(rows, distinct));
}

typedef struct TestFixedConfig
{
	bool		enabled;
	Oid			request_relid;
	uint32		stage_count;
	Oid		   *edge_relids;
	Oid		   *index_relids;
	Oid		   *fallback_index_relids;
	int32	   *directions;
	GgcProviderRef primary_provider;
	GgcProviderRef fallback_provider;
	uint64		primary_build_generation;
	uint64		catalog_generation;
	uint64		statistics_generation;
	Oid			graph_oid;
	bool		has_fallback;
	bool		endpoint_domain_is_edge;
	int			wrapper_mode;
	int			pause_ms;
	uint32		anchor_batch_size;
	uint32		max_parent_batch;
	uint32		max_bindings;
	uint32		max_endpoints;
	uint32		max_candidates;
	uint32		max_inflight_pages;
	Size		max_restore_bytes;
	bool		prefetch;
	bool		preserve_order;
	Size		factor_bytes;
	bool		endpoint_set;
} TestFixedConfig;

typedef struct TestCboConfig
{
	bool		enabled;
	Oid			request_relid;
	Oid			edge_relid;
	Oid			index_relid;
	GgcStatsScope logical_scope;
	GgcStatsScope physical_scope;
	GgcStatsScope cost_scope;
	uint32		stage_count;
	uint32		predicate_clause_id;
} TestCboConfig;

typedef struct TestCboDiagnostic
{
	uint64		exact_relid_attempts;
	uint64		chain_matches;
	uint64		contract_accepts;
	uint64		paths_retained;
	uint64		plan_builds;
	uint64		pgq_attempts;
	uint64		pgq_security_rejects;
	uint64		pgq_contract_rejects;
	uint64		pgq_paths_retained;
	int			available_quals;
	int			consumed_quals;
	GgcGraphPathRejectReason reject_reason;
	double		startup_cost;
	double		total_cost;
	char		qual_shape[256];
} TestCboDiagnostic;


static TestFixedConfig test_config;
static TestCboConfig cbo_config;
static TestCboDiagnostic cbo_diagnostic;
static set_rel_pathlist_hook_type previous_set_rel_pathlist_hook;
static set_join_pathlist_hook_type previous_set_join_pathlist_hook;
static create_upper_paths_hook_type previous_graph_upper_paths_hook;

static Plan *pgq_plan_fixed_path(PlannerInfo *root, RelOptInfo *rel,
								  CustomPath *best_path, List *tlist,
								  List *clauses, List *custom_plans);
static Node *pgq_create_fixed_state(CustomScan *cscan);
static void pgq_begin_fixed(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *pgq_exec_fixed(CustomScanState *node);
static void pgq_end_fixed(CustomScanState *node);
static void pgq_rescan_fixed(CustomScanState *node);
static void pgq_explain_fixed(CustomScanState *node, List *ancestors,
							   ExplainState *es);
static void test_fixed_set_join_pathlist(PlannerInfo *root,
									 RelOptInfo *joinrel,
									 RelOptInfo *outerrel,
									 RelOptInfo *innerrel,
									 JoinType jointype,
									 JoinPathExtraData *extra);

static const CustomScanMethods pgq_native_scan_methods;
static RelOptInfo *pgq_native_join_search(PlannerInfo *root, GgcFixedProgram *program,
										bool retain_fixed);

static const CustomPathMethods pgq_fixed_path_methods = {
	.CustomName = "GraphExpandFixed",
	.PlanCustomPath = pgq_plan_fixed_path
};

static const CustomScanMethods pgq_fixed_scan_methods = {
	.CustomName = "GraphExpandFixed",
	.CreateCustomScanState = pgq_create_fixed_state
};

static const CustomExecMethods pgq_fixed_exec_methods = {
	.CustomName = "GraphExpandFixed",
	.BeginCustomScan = pgq_begin_fixed,
	.ExecCustomScan = pgq_exec_fixed,
	.EndCustomScan = pgq_end_fixed,
	.ReScanCustomScan = pgq_rescan_fixed,
	.ExplainCustomScan = pgq_explain_fixed
};

#include "graph_route_hint.inc"

static bool
target_has_request_columns(RelOptInfo *rel, Index rti)
{
	bool seen[4] = {false, false, false, false};

	foreach_ptr(Node, expression, rel->reltarget->exprs)
	{
		if (IsA(expression, Var))
		{
			Var *var = (Var *) expression;

			if (var->varno == rti && var->varattno >= 1 && var->varattno <= 4)
				seen[var->varattno - 1] = true;
		}
	}
	if (test_config.endpoint_set)
		return seen[0] && !seen[1] && !seen[2] && !seen[3];
	return seen[0] && seen[1] && seen[2] && seen[3];
}

static List *
serialize_provider_ref(const GgcProviderRef *provider)
{
	List	   *result = NIL;

	result = lappend(result, makeInteger(provider->struct_size));
	result = lappend(result, makeInteger(provider->abi_version));
	result = lappend(result, makeInteger((int) provider->owner_extension_oid));
	result = lappend(result, makeInteger(provider->kind));
	result = lappend(result, makeString(pstrdup(NameStr(provider->name))));
	return result;
}

static List *
serialize_stage(Oid edge_relid, Oid primary_index_relid,
				GgcDirection direction, const GgcProviderRef *primary_provider,
				uint64 primary_build_generation, bool has_fallback,
				Oid fallback_index_relid,
				const GgcProviderRef *fallback_provider,
				Oid graph_oid, uint64 catalog_generation,
				uint64 statistics_generation, uint32 endpoint_domain)
{
	List	   *stage = NIL;

	stage = lappend(stage, makeInteger((int) edge_relid));
	stage = lappend(stage, makeInteger(direction));
	stage = lappend(stage, serialize_provider_ref(primary_provider));
	stage = lappend(stage, makeInteger((int) primary_index_relid));
	stage = lappend(stage,
		makeString(psprintf(UINT64_FORMAT, primary_build_generation)));
	stage = lappend(stage, makeInteger(has_fallback));
	stage = lappend(stage, serialize_provider_ref(fallback_provider));
	stage = lappend(stage, makeInteger((int) fallback_index_relid));
	stage = lappend(stage, makeString(pstrdup("0")));
	stage = lappend(stage, makeInteger((int) graph_oid));
	stage = lappend(stage,
		makeString(psprintf(UINT64_FORMAT, catalog_generation)));
	stage = lappend(stage,
		makeString(psprintf(UINT64_FORMAT, statistics_generation)));
	stage = lappend(stage, makeInteger((int) endpoint_domain));
	return stage;
}

static List *
serialize_config(void)
{
	List	   *stages = NIL;
	List	   *options;

	for (uint32 i = 0; i < test_config.stage_count; i++)
		stages = lappend(stages, serialize_stage(
			test_config.edge_relids[i], test_config.index_relids[i],
			test_config.directions[i], &test_config.primary_provider,
			test_config.primary_build_generation, test_config.has_fallback,
			test_config.has_fallback ? test_config.fallback_index_relids[i] :
				InvalidOid, &test_config.fallback_provider,
			test_config.graph_oid, test_config.catalog_generation,
			test_config.statistics_generation,
			test_config.endpoint_domain_is_edge ? test_config.edge_relids[i] : 77));
	options = list_make4(makeInteger(test_config.pause_ms),
		makeInteger(test_config.anchor_batch_size),
		makeInteger(test_config.max_parent_batch),
		makeInteger(test_config.max_bindings));
	options = lappend(options, makeInteger(test_config.wrapper_mode));
	options = lappend(options, makeInteger(test_config.max_endpoints));
	options = lappend(options, makeInteger(test_config.max_candidates));
	options = lappend(options, makeInteger(test_config.max_inflight_pages));
	options = lappend(options,
		makeString(psprintf("%zu", test_config.max_restore_bytes)));
	options = lappend(options, makeInteger(test_config.prefetch));
	options = lappend(options, makeInteger(test_config.preserve_order));
	options = lappend(options, makeString(psprintf("%zu", test_config.factor_bytes)));
	options = lappend(options, makeInteger(test_config.endpoint_set));
	return list_make2(stages, options);
}

static List *
serialize_join_config(GgcFixedProgram *program,
					  const int *stage_rtis, const int *vertex_rtis,
					  const GgcDirection *directions,
					  const GgcGraphPathCost *cost,
					  int covered_relids, int consumed_quals,
					  int required_outer, Index request_rti,
					  const PgqStageIndexes *index_choices)
{
	List	   *private = NIL;
	List	   *stages = NIL;
	List	   *options;
	List	   *metadata = NIL;
	List	   *rtis = NIL;
	List	   *vertices = NIL;

	for (uint32 i = 0; i < cbo_config.stage_count; i++)
	{
		GgcDirection direction = directions ? directions[i] : GGC_OUT;
		GgcVariable *edge = pgq_rti_variable(program, stage_rtis[i]);
		GgcVariable *vertex = vertex_rtis ?
			pgq_rti_variable(program, vertex_rtis[i + 1]) : NULL;
		Oid edgeoid = edge ? edge->relation_oid : cbo_config.edge_relid;
		AttrNumber src = edge ? edge->source_attnum : 2;
		AttrNumber dst = edge ? edge->destination_attnum : 3;
		Oid indexoid = cbo_config.index_relid;
		const GgcAdjProviderV2 *provider;
		List *serialized;

		if (index_choices)
			indexoid = index_choices[i].edge_index;
		else if (route_uses_btree() && directions)
			indexoid = route_choose_index(edgeoid,
				direction == GGC_IN ? dst : src, direction == GGC_IN ? src : dst);
		provider = graph_index_provider(indexoid);
		serialized = serialize_stage(edgeoid,
			indexoid,
			direction == GGC_UNDIRECTED ? GGC_OUT : direction,
			&provider->identity,
			(provider->capabilities & GGC_CAP_BUILD_GENERATION) ?
			cbo_config.physical_scope.build_generation : 0, false, InvalidOid,
			&provider->identity, cbo_config.logical_scope.graph_oid,
			cbo_config.logical_scope.catalog_generation,
			cost->cost_profile.statistics_generation,
			program ? edgeoid : cbo_config.logical_scope.endpoint_domain);
		if (vertex_rtis)
		{
			Assert(vertex && edge);
			serialized = lappend(serialized, makeInteger((int) vertex->relation_oid));
			serialized = lappend(serialized, makeInteger((int)
				(index_choices ? index_choices[i].vertex_index :
				 pgq_vertex_index(vertex->relation_oid, vertex->identity_attnum))));
			serialized = lappend(serialized, makeInteger(direction == GGC_UNDIRECTED ?
				(int) (index_choices ? index_choices[i].reverse_index :
				 route_choose_index(edgeoid, dst, src)) : InvalidOid));
			serialized = lappend(serialized, makeInteger(edge->identity_attnum));
			serialized = lappend(serialized, makeInteger(src));
			serialized = lappend(serialized, makeInteger(dst));
			serialized = lappend(serialized, makeInteger(pgq_payload_attnum(edge)));
			serialized = lappend(serialized, makeInteger(vertex->identity_attnum));
		}
		stages = lappend(stages, serialized);
		rtis = lappend(rtis, makeInteger(stage_rtis[i]));
	}
	if (vertex_rtis)
		for (uint32 i = 0; i <= cbo_config.stage_count; i++)
			vertices = lappend(vertices, makeInteger(vertex_rtis[i]));
	options = list_make4(makeInteger(0), makeInteger(16),
		makeInteger(64), makeString(psprintf(UINT64_FORMAT, PG_UINT64_MAX)));
	options = lappend(options, makeInteger(0));
	options = lappend(options, makeInteger(8));
	options = lappend(options, makeInteger(16));
	options = lappend(options, makeInteger(4));
	options = lappend(options, makeString(pstrdup("1048576")));
	options = lappend(options, makeInteger(true));
	options = lappend(options, makeInteger(true));
	metadata = lappend(metadata, makeInteger(1));	/* join mode */
	metadata = lappend(metadata, makeInteger(cost->logical_state));
	metadata = lappend(metadata, makeInteger(cost->physical_state));
	metadata = lappend(metadata, makeInteger(cost->cost_state));
	metadata = lappend(metadata, makeInteger(cost->uncertainty_unknown));
	metadata = lappend(metadata, makeInteger(cost->cost_profile.calibrated));
	metadata = lappend(metadata,
		makeString(psprintf(UINT64_FORMAT, cost->logical_generation)));
	metadata = lappend(metadata,
		makeString(psprintf(UINT64_FORMAT, cost->physical_generation)));
	metadata = lappend(metadata,
		makeString(psprintf(UINT64_FORMAT,
			cost->cost_profile.statistics_generation)));
	metadata = lappend(metadata,
		makeInteger(cost->cost_profile.version));
	metadata = lappend(metadata, makeInteger(covered_relids));
	metadata = lappend(metadata, makeInteger(consumed_quals));
	metadata = lappend(metadata, makeInteger(required_outer));
	metadata = lappend(metadata, makeInteger(request_rti));
	metadata = lappend(metadata, rtis);
	metadata = lappend(metadata, vertices);
	private = lappend(private, stages);
	private = lappend(private, options);
	private = lappend(private, metadata);
	return private;
}

static bool
join_var_pair(RestrictInfo *rinfo, Index left_rti, AttrNumber left_attno,
			  Index right_rti, AttrNumber right_attno)
{
	OpExpr	   *op;
	Node	   *left;
	Node	   *right;
	Var		   *left_var;
	Var		   *right_var;

	if (!IsA(rinfo->clause, OpExpr))
		return false;
	op = castNode(OpExpr, rinfo->clause);
	if (list_length(op->args) != 2 || op->opretset || OidIsValid(op->inputcollid))
		return false;
	left = strip_implicit_coercions(linitial(op->args));
	right = strip_implicit_coercions(lsecond(op->args));
	if (!IsA(left, Var) || !IsA(right, Var))
		return false;
	left_var = castNode(Var, left);
	right_var = castNode(Var, right);
	/* Most available clauses belong to another hop. Match the variable pair
	 * before looking up operator semantics, retaining the full type and
	 * equality-family checks for every clause that can match this hop. */
	if (!((left_var->varno == left_rti && left_var->varattno == left_attno &&
		   right_var->varno == right_rti && right_var->varattno == right_attno) ||
		  (left_var->varno == right_rti && left_var->varattno == right_attno &&
		   right_var->varno == left_rti && right_var->varattno == left_attno)))
		return false;
	return GgcIntegerKeyType(left_var->vartype) &&
		GgcIntegerKeyType(right_var->vartype) &&
		op->opno == get_opfamily_member(INTEGER_BTREE_FAM_OID,
			left_var->vartype, right_var->vartype, BTEqualStrategyNumber);
}

static List *
internal_join_quals(PlannerInfo *root, Relids covered_relids)
{
	List	   *quals = NIL;
	int			rti = -1;

	/* Mergejoinable source clauses migrate from baserel joininfo into ECs. */
	foreach_ptr(EquivalenceClass, ec, root->eq_classes)
	{
		if (ec->ec_merged != NULL)
			continue;
		foreach_ptr(RestrictInfo, rinfo, ec->ec_sources)
		{
			if (bms_num_members(rinfo->clause_relids) >= 2 &&
				bms_is_subset(rinfo->clause_relids, covered_relids) &&
				!list_member_ptr(quals, rinfo))
				quals = lappend(quals, rinfo);
		}
	}

	/* Retain non-EC join clauses as part of the exact boundary as well. */
	while ((rti = bms_next_member(covered_relids, rti)) >= 0)
	{
		RelOptInfo *rel = root->simple_rel_array[rti];

		if (rel == NULL)
			return NIL;
		foreach_ptr(RestrictInfo, rinfo, rel->joininfo)
		{
			if (bms_is_subset(rinfo->clause_relids, covered_relids) &&
				!list_member_ptr(quals, rinfo))
				quals = lappend(quals, rinfo);
		}
	}
	return quals;
}

static bool
find_graph_chain(PlannerInfo *root, Relids covered_relids, List *quals,
				 Index *request_rti, int *stage_rtis, List **consumed)
{
	Bitmapset  *unused_edges = NULL;
	Index		predecessor;
	AttrNumber predecessor_attno;
	int			rti = -1;

	*request_rti = 0;
	*consumed = NIL;
	while ((rti = bms_next_member(covered_relids, rti)) >= 0)
	{
		RangeTblEntry *rte = planner_rt_fetch(rti, root);

		if ((rte->rtekind == RTE_RELATION &&
			 rte->relid == cbo_config.request_relid) ||
			(rte->rtekind == RTE_SUBQUERY && rte->lateral &&
			 list_length(rte->subquery->targetList) == 1))
		{
			if (*request_rti != 0)
				return false;
			*request_rti = rti;
		}
		else if (rte->rtekind == RTE_RELATION &&
				 rte->relid == cbo_config.edge_relid)
			unused_edges = bms_add_member(unused_edges, rti);
		else
			return false;
	}
	if (*request_rti == 0 ||
		bms_num_members(unused_edges) != cbo_config.stage_count)
		return false;

	predecessor = *request_rti;
	predecessor_attno = 1;
	for (uint32 stage = 0; stage < cbo_config.stage_count; stage++)
	{
		Index		matched_rti = 0;
		RestrictInfo *matched_qual = NULL;
		int			edge_rti = -1;

		while ((edge_rti = bms_next_member(unused_edges, edge_rti)) >= 0)
		{
			foreach_ptr(RestrictInfo, rinfo, quals)
			{
				if (join_var_pair(rinfo, edge_rti, 2,
							  predecessor, predecessor_attno))
				{
					if (matched_qual != NULL)
						return false;
					matched_rti = edge_rti;
					matched_qual = rinfo;
				}
			}
		}
		if (matched_qual == NULL)
			return false;
		stage_rtis[stage] = matched_rti;
		*consumed = lappend(*consumed, matched_qual);
		unused_edges = bms_del_member(unused_edges, matched_rti);
		predecessor = matched_rti;
		predecessor_attno = 3;
	}
	return bms_is_empty(unused_edges);
}

static GgcVariable *
pgq_program_variable(GgcFixedProgram *program, int variable_id)
{
	foreach_node(GgcVariable, variable, program->variables)
		if (variable->variable_id == variable_id)
			return variable;
	return NULL;
}

static bool
pgq_vertex_index_eligible(Relation idx, Oid vertex_relid,
						  AttrNumber key_attnum, bool primary_only)
{
	return idx->rd_rel->relam == BTREE_AM_OID &&
		idx->rd_index->indisunique && idx->rd_index->indimmediate &&
		(!primary_only || idx->rd_index->indisprimary) &&
		idx->rd_index->indisvalid && idx->rd_index->indisready &&
		idx->rd_index->indislive && !idx->rd_index->indcheckxmin &&
		idx->rd_index->indnkeyatts == 1 &&
		idx->rd_index->indkey.values[0] == key_attnum &&
		RelationGetIndexPredicate(idx) == NIL &&
		RelationGetIndexExpressions(idx) == NIL &&
		pgq_key_type_supported(idx->rd_opcintype[0]) &&
		idx->rd_opcintype[0] == get_atttype(vertex_relid, key_attnum) &&
		get_opfamily_proc(idx->rd_opfamily[0], INT8OID, INT8OID,
			BTORDER_PROC) == F_BTINT8CMP;
}

/* Non-planning callers ask only for a legal unique row-identity lookup.
 * Stage visibility indexes are selected separately with PlannerInfo costs. */
static Oid
pgq_vertex_index(Oid vertex_relid, AttrNumber key_attnum)
{
	Relation rel = table_open(vertex_relid, AccessShareLock);
	Oid index_oid = InvalidOid;
	List *indexes = RelationGetIndexList(rel);
	bool primary_only = !route_uses_btree() || enable_graph_age_adjacency;

	foreach_oid(candidate, indexes)
	{
		Relation idx = index_open(candidate, AccessShareLock);
		bool usable = pgq_vertex_index_eligible(idx, vertex_relid,
			key_attnum, primary_only);

		index_close(idx, AccessShareLock);
		if (usable)
		{
			index_oid = candidate;
			break;
		}
	}
	list_free(indexes);
	table_close(rel, AccessShareLock);
	return index_oid;
}

static GgcVariable *
pgq_rti_variable(GgcFixedProgram *program, Index rti)
{
	if (program)
		foreach_node(GgcVariable, variable, program->variables)
			if (variable->fallback_rti == rti)
				return variable;
	return NULL;
}

static AttrNumber
pgq_key_attnum(GgcFixedProgram *program, Index rti)
{
	GgcVariable *variable = pgq_rti_variable(program, rti);

	return variable ? variable->identity_attnum : 1;
}

/* Preserve the historical compact payload path only for its canonical
 * layout. Other attributes use typed, uniquely identified property reads;
 * a three-column edge needs no dummy payload column. */
static AttrNumber
pgq_payload_attnum(GgcVariable *edge)
{
	if (edge->identity_attnum == 1 && edge->source_attnum == 2 &&
		edge->destination_attnum == 3 &&
		OidIsValid(get_atttype(edge->relation_oid, 4)))
		return 4;
	return InvalidAttrNumber;
}

static bool
pgq_undirected_edge(GgcFixedProgram *program, int variable_id)
{
	foreach_node(GgcFixedHop, hop, program->hops)
		if (hop->edge_variable == variable_id)
			return hop->direction == GGC_UNDIRECTED;
	return false;
}

static bool
pgq_target_var_supported(GgcFixedProgram *program, Var *var)
{
	if (var->varattno <= 0 || var->varlevelsup || var->varnullingrels)
		return false;
	foreach_node(GgcVariable, variable, program->variables)
	{
		bool direct;

		if (variable->fallback_rti != var->varno)
			continue;
		direct = var->varattno == variable->identity_attnum ||
			(variable->entity_kind == GGC_EDGE &&
			 (var->varattno == variable->source_attnum ||
			  var->varattno == variable->destination_attnum ||
			  var->varattno == pgq_payload_attnum(variable)));
		if (variable->entity_kind == GGC_EDGE &&
			(var->varattno == variable->source_attnum ||
			 var->varattno == variable->destination_attnum) &&
			pgq_undirected_edge(program, variable->variable_id))
			direct = false;
		if (direct)
			return true;
		/* Additional properties need a unique, same-snapshot row identity.
		 * Do not turn the unique-key requirement into an implicit LIMIT 1. */
		if (route_uses_btree() &&
			GgcSelectColumnAllowed(variable->relation_oid, var->varattno, GetUserId()))
		{
			Oid indexoid = pgq_vertex_index(variable->relation_oid, variable->identity_attnum);
			Relation index;
			bool supported;

			if (!OidIsValid(indexoid))
				return false;
			index = index_open(indexoid, AccessShareLock);
			supported = index->rd_rel->relam == BTREE_AM_OID &&
				get_opfamily_proc(index->rd_opfamily[0], INT8OID, INT8OID,
					BTORDER_PROC) == F_BTINT8CMP;
			index_close(index, AccessShareLock);
			return supported;
		}
		return false;
	}
	return false;
}

static bool
pgq_binding_expression_unsafe(Node *node, GgcFixedProgram *program)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
		return !pgq_target_var_supported(program, castNode(Var, node));
	if (IsA(node, Param))
		return castNode(Param, node)->paramkind != PARAM_EXTERN;
	if (IsA(node, SubPlan) || IsA(node, SubLink) || IsA(node, AlternativeSubPlan) ||
		IsA(node, Aggref) || IsA(node, WindowFunc) || IsA(node, PlaceHolderVar) ||
		IsA(node, CurrentOfExpr) || IsA(node, CoerceToDomain))
		return true;
	return expression_tree_walker(node, pgq_binding_expression_unsafe, program);
}

/*
 * Recognize only a plain bigint key compared to a nonnull integer constant
 * with PostgreSQL's built-in integer equality.  In particular, an untyped
 * literal such as "id = 1" normally uses int84eq, not int8eq.  Normalize the
 * constant without evaluating expressions or calling user-defined code.
 */
static bool
pgq_key_equality_constant(RestrictInfo *rinfo, Index rti, AttrNumber attno,
						 int64 *value)
{
	OpExpr	   *op;
	Var		   *key;
	Const	   *constant;
	Node	   *left;
	Node	   *right;
	bool		key_on_left;
	Oid			function;
	Oid			expected;

	if (rinfo->security_level != 0 || rinfo->pseudoconstant ||
		!IsA(rinfo->clause, OpExpr))
		return false;
	op = castNode(OpExpr, rinfo->clause);
	if (list_length(op->args) != 2 || op->opresulttype != BOOLOID ||
		op->opretset || OidIsValid(op->inputcollid))
		return false;
	left = linitial(op->args);
	right = lsecond(op->args);
	if (IsA(left, Var) && IsA(right, Const))
	{
		key = castNode(Var, left);
		constant = castNode(Const, right);
		key_on_left = true;
	}
	else if (IsA(left, Const) && IsA(right, Var))
	{
		key = castNode(Var, right);
		constant = castNode(Const, left);
		key_on_left = false;
	}
	else
		return false;
	if (key->varno != rti || key->varattno != attno ||
		key->varlevelsup != 0 || !GgcIntegerKeyType(key->vartype) ||
		key->varnullingrels != NULL || constant->constisnull)
		return false;
	function = get_opcode(op->opno);
	switch (constant->consttype)
	{
		case INT8OID:
			*value = DatumGetInt64(constant->constvalue);
			break;
		case INT4OID:
			*value = DatumGetInt32(constant->constvalue);
			break;
		case INT2OID:
			*value = DatumGetInt16(constant->constvalue);
			break;
		default:
			return false;
	}
	expected = get_opcode(get_opfamily_member(INTEGER_BTREE_FAM_OID,
		key_on_left ? key->vartype : constant->consttype,
		key_on_left ? constant->consttype : key->vartype, BTEqualStrategyNumber));
	return function == expected;
}

/*
 * An OUT lookup already enforces edge.src = input.id.  Thus a
 * restriction edge.src = K is redundant if the bound input enforces
 * input.id = K.  Each removed restriction must pass this narrow proof; never
 * discard a payload/destination predicate or a security condition here.
 * find_pgq_quals must also accept the exact topology before a path is added.
 */
static List *
pgq_input_restrictions_remaining(PlannerInfo *root,
								   GgcVariable *anchor, GgcVariable *edge,
								   GgcDirection direction)
{
	RelOptInfo *anchor_rel = root->simple_rel_array[anchor->fallback_rti];
	RelOptInfo *edge_rel = root->simple_rel_array[edge->fallback_rti];
	List *remaining = NIL;

	if (direction == GGC_UNDIRECTED)
		return list_copy(edge_rel->baserestrictinfo);
	foreach_ptr(RestrictInfo, edge_qual, edge_rel->baserestrictinfo)
	{
		int64		edge_value;
		bool		implied = false;

		if (pgq_key_equality_constant(edge_qual, edge->fallback_rti,
				direction == GGC_OUT ? edge->source_attnum : edge->destination_attnum,
				&edge_value))
		foreach_ptr(RestrictInfo, anchor_qual, anchor_rel->baserestrictinfo)
		{
			int64		anchor_value;

			if (pgq_key_equality_constant(anchor_qual, anchor->fallback_rti,
					anchor->identity_attnum, &anchor_value) &&
				anchor_value == edge_value)
			{
				implied = true;
				break;
			}
		}
		if (!implied)
			remaining = lappend(remaining, edge_qual);
	}
	return remaining;
}

static bool
pgq_local_expression_unsafe(Node *node, Index *rti)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var *var = castNode(Var, node);

		return var->varno != *rti || var->varattno <= 0 ||
			var->varlevelsup != 0 || var->varnullingrels != NULL;
	}
	if (IsA(node, Param))
		return castNode(Param, node)->paramkind != PARAM_EXTERN;
	if (IsA(node, SubPlan) || IsA(node, SubLink) || IsA(node, AlternativeSubPlan) ||
		IsA(node, Aggref) || IsA(node, WindowFunc) || IsA(node, PlaceHolderVar) ||
		IsA(node, CurrentOfExpr) || IsA(node, CoerceToDomain))
		return true;
	return expression_tree_walker(node, pgq_local_expression_unsafe, rti);
}

static bool
pgq_local_quals_supported(List *quals, Index rti, Oid relation)
{
	foreach_node(RestrictInfo, rinfo, quals)
	{
		if (rinfo->security_level != 0 || rinfo->pseudoconstant ||
			!bms_equal(rinfo->clause_relids, bms_make_singleton(rti)) ||
			contain_volatile_functions((Node *) rinfo->clause) ||
			expression_returns_set((Node *) rinfo->clause) ||
			pgq_local_expression_unsafe((Node *) rinfo->clause, &rti))
			return false;
		foreach_node(Var, var, pull_var_clause((Node *) rinfo->clause, 0))
			if (!GgcSelectColumnAllowed(relation, var->varattno, GetUserId()))
				return false;
	}
	return true;
}

/*
 * Destination vertices are checked against the query snapshot by the runtime.
 * Local property predicates use the visible tuple. Additional properties use
 * a proven unique same-snapshot lookup; RLS remains relational-only.
 */
static bool
pgq_program_contract(PlannerInfo *root, RelOptInfo *joinrel,
				 GgcFixedProgram *program, int *stage_rtis,
				 int *vertex_rtis, int *input_positions, GgcDirection *directions,
				 List **local_quals, List **local_rinfos)
{
	Relids		program_relids = NULL;
	Relids		bound_relids = NULL;
	Bitmapset  *edge_variables = NULL;
	uint32		stage = 0;
	bool		flexible = route_uses_btree() && !enable_graph_age_adjacency;

	*local_quals = *local_rinfos = NIL;
	if (program->native_access || program->semantic_version != GGC_GRAPH_SEMANTIC_VERSION ||
		program->graph_oid != cbo_config.logical_scope.graph_oid ||
		list_length(program->hops) != cbo_config.stage_count ||
		(!flexible && list_length(program->variables) != 2 * cbo_config.stage_count + 1) ||
		list_length(program->path_constraints) != 1 ||
		linitial_node(GgcPathConstraint,
			program->path_constraints)->mode != GGC_WALK ||
		program->required_outer != NULL)
		return false;
	foreach_node(GgcVariable, variable, program->variables)
	{
		RangeTblEntry *rte;
		RelOptInfo *rel;
		Relation	relation;

		if (variable->fallback_rti <= 0 ||
			variable->fallback_rti >= root->simple_rel_array_size)
			return false;
		rte = planner_rt_fetch(variable->fallback_rti, root);
		rel = root->simple_rel_array[variable->fallback_rti];
		if (rte->rtekind != RTE_RELATION || rel == NULL ||
			rte->relid != variable->relation_oid || rte->securityQuals != NIL)
			return false;
		if (contain_volatile_functions((Node *) rel->baserestrictinfo))
			return false;
		if (variable->entity_kind == GGC_VERTEX)
		{
			if ((!flexible && (variable->relation_oid != cbo_config.request_relid ||
				 variable->identity_attnum != 1)) ||
				!(flexible ? pgq_key_type_supported(variable->key_type) : variable->key_type == INT8OID) ||
				!OidIsValid(pgq_vertex_index(variable->relation_oid, variable->identity_attnum)))
				return false;
		}
		else if (variable->entity_kind != GGC_EDGE ||
				 (!flexible && (variable->relation_oid != cbo_config.edge_relid ||
				 variable->identity_attnum != 1 ||
				 variable->source_attnum != 2 ||
				 variable->destination_attnum != 3)) ||
				 variable->source_attnum == variable->destination_attnum ||
				 !(flexible ? pgq_key_type_supported(variable->key_type) : variable->key_type == INT8OID) ||
				 (!flexible && pg_class_aclcheck(variable->relation_oid, GetUserId(), ACL_SELECT) != ACLCHECK_OK))
			return false;
		relation = table_open(variable->relation_oid, AccessShareLock);
		if (relation->rd_rel->relkind != RELKIND_RELATION ||
			relation->rd_rel->relrowsecurity ||
			relation->rd_rel->relhassubclass ||
			(!flexible && variable->entity_kind == GGC_EDGE &&
			 RelationGetDescr(relation)->natts < 4))
		{
			table_close(relation, AccessShareLock);
			return false;
		}
		/* Match the fixed runtime's key contract before offering this path.
		 * SQL/PGQ itself permits nullable edge endpoints; those relations must
		 * retain their ordinary relational plan, not fail in ExecutorStart. */
		{
			AttrNumber keys[3] = {variable->identity_attnum,
				variable->source_attnum, variable->destination_attnum};

			for (int i = 0; i < (variable->entity_kind == GGC_EDGE ? 3 : 1); i++)
			{
				Form_pg_attribute attr;

				if (keys[i] <= 0 || keys[i] > RelationGetDescr(relation)->natts)
				{
					table_close(relation, AccessShareLock);
					return false;
				}
				attr = TupleDescAttr(RelationGetDescr(relation), keys[i] - 1);

				if (attr->attisdropped ||
					!(flexible ? pgq_key_type_supported(attr->atttypid) : attr->atttypid == INT8OID) ||
					!attr->attnotnull || !GgcSelectColumnAllowed(variable->relation_oid,
						keys[i], GetUserId()))
				{
					table_close(relation, AccessShareLock);
					return false;
				}
			}
		}
		table_close(relation, AccessShareLock);
		program_relids = bms_add_member(program_relids,
			variable->fallback_rti);
	}
	if (!bms_equal(program_relids, joinrel->relids))
		return false;

	foreach_ptr(Node, expression, joinrel->reltarget->exprs)
	{
		if (contain_volatile_functions(expression) || expression_returns_set(expression) ||
			pgq_binding_expression_unsafe(expression, program))
			return false;
	}

	foreach_node(GgcFixedHop, hop, program->hops)
	{
		GgcVariable *input = pgq_program_variable(program,
			hop->input_variable);
		GgcVariable *edge = pgq_program_variable(program,
			hop->edge_variable);
		GgcVariable *output = pgq_program_variable(program,
			hop->output_variable);
		List	   *edge_quals;
		List	   *vertex_quals;

		if (!input || !edge || !output ||
			input->entity_kind != GGC_VERTEX ||
			edge->entity_kind != GGC_EDGE ||
			output->entity_kind != GGC_VERTEX ||
			(enable_graph_age_adjacency && hop->direction != GGC_OUT) ||
			(hop->direction != GGC_OUT &&
			 (!route_uses_btree() ||
			  (hop->direction != GGC_IN && hop->direction != GGC_UNDIRECTED))) ||
			edge->fallback_rti == input->fallback_rti ||
			edge->fallback_rti == output->fallback_rti)
			return false;
		/* A logical edge variable is expanded once. Repeated vertices instead
		 * close a binding and must not be mistaken for fresh graph variables. */
		if (bms_is_member(edge->variable_id, edge_variables))
			return false;
		edge_variables = bms_add_member(edge_variables, edge->variable_id);
		if (stage == 0)
		{
			vertex_rtis[0] = input->fallback_rti;
			bound_relids = bms_add_member(bound_relids, input->fallback_rti);
		}
		input_positions[stage] = -1;
		for (uint32 position = 0; position <= stage; position++)
		{
			if (vertex_rtis[position] == input->fallback_rti)
				input_positions[stage] = position;
			if (!flexible && vertex_rtis[position] == output->fallback_rti)
				return false;
		}
		if (input_positions[stage] < 0)
			return false;
		stage_rtis[stage] = edge->fallback_rti;
		vertex_rtis[stage + 1] = output->fallback_rti;
		bound_relids = bms_add_member(bound_relids, edge->fallback_rti);
		bound_relids = bms_add_member(bound_relids, output->fallback_rti);
		directions[stage] = hop->direction;
		edge_quals = root->simple_rel_array[edge->fallback_rti]->baserestrictinfo;
		vertex_quals = root->simple_rel_array[output->fallback_rti]->baserestrictinfo;
		/* The input is already bound and filtered, even when a reordered
		 * stage branches from an earlier position rather than its predecessor.
		 * Do not evaluate/estimate its implied endpoint equality twice. */
		edge_quals = pgq_input_restrictions_remaining(root, input, edge, hop->direction);
		if (edge_quals || vertex_quals)
		{
			/* Other providers and legacy manually configured test routes retain
			 * their old admission contract; opt-in B-tree shares the new contract. */
			if (!route_uses_btree() ||
				!pgq_local_quals_supported(edge_quals, edge->fallback_rti, edge->relation_oid) ||
				!pgq_local_quals_supported(vertex_quals, output->fallback_rti, output->relation_oid))
				return false;
		}
		*local_quals = lappend(*local_quals, extract_actual_clauses(edge_quals, false));
		*local_quals = lappend(*local_quals, extract_actual_clauses(vertex_quals, false));
		/* Admission records a set of owned clauses, even when a repeated
		 * vertex has a local recheck at several physical stages. */
		*local_rinfos = list_concat_unique_ptr(*local_rinfos, edge_quals);
		*local_rinfos = list_concat_unique_ptr(*local_rinfos, vertex_quals);
		stage++;
	}
	if (*local_rinfos == NIL)
		*local_quals = NIL;
	return stage == cbo_config.stage_count && bms_equal(bound_relids, program_relids);
}

static bool
pgq_directional_conjunction(Node *node, Index edge, AttrNumber first,
							AttrNumber second, GgcVariable *input, GgcVariable *output)
{
	RestrictInfo left = {0}, right = {0};
	BoolExpr *conjunction;

	if (!is_andclause(node))
		return false;
	conjunction = castNode(BoolExpr, node);
	if (list_length(conjunction->args) != 2)
		return false;
	left.clause = linitial(conjunction->args);
	right.clause = lsecond(conjunction->args);
	return (join_var_pair(&left, edge, first, input->fallback_rti, input->identity_attnum) &&
			join_var_pair(&right, edge, second, output->fallback_rti, output->identity_attnum)) ||
		(join_var_pair(&right, edge, first, input->fallback_rti, input->identity_attnum) &&
		 join_var_pair(&left, edge, second, output->fallback_rti, output->identity_attnum));
}

static bool
pgq_undirected_topology(RestrictInfo *rinfo, GgcVariable *edge,
						GgcVariable *input, GgcVariable *output)
{
	BoolExpr *disjunction;
	Node *left, *right;

	if (rinfo->security_level || !is_orclause(rinfo->clause))
		return false;
	disjunction = castNode(BoolExpr, rinfo->clause);
	if (list_length(disjunction->args) != 2)
		return false;
	left = linitial(disjunction->args);
	right = lsecond(disjunction->args);
	return (pgq_directional_conjunction(left, edge->fallback_rti,
			edge->source_attnum, edge->destination_attnum, input, output) &&
			pgq_directional_conjunction(right, edge->fallback_rti,
			edge->destination_attnum, edge->source_attnum, input, output)) ||
		(pgq_directional_conjunction(right, edge->fallback_rti,
			edge->source_attnum, edge->destination_attnum, input, output) &&
		 pgq_directional_conjunction(left, edge->fallback_rti,
			edge->destination_attnum, edge->source_attnum, input, output));
}

static bool
find_pgq_quals(GgcFixedProgram *program, List *available,
			   const int *stage_rtis, const int *vertex_rtis,
			   const GgcDirection *directions, List **consumed, List **residual)
{
	uint32		stage = 0;

	*consumed = NIL;
	*residual = NIL;
	foreach_node(GgcFixedHop, hop, program->hops)
	{
		GgcVariable *input = pgq_program_variable(program,
			hop->input_variable);
		GgcVariable *edge = pgq_program_variable(program,
			hop->edge_variable);
		GgcVariable *output = pgq_program_variable(program,
			hop->output_variable);
		RestrictInfo *input_qual = NULL;
		RestrictInfo *output_qual = NULL;
		AttrNumber input_edge_attnum = directions[stage] == GGC_OUT ?
			edge->source_attnum : edge->destination_attnum;
		AttrNumber output_edge_attnum = directions[stage] == GGC_OUT ?
			edge->destination_attnum : edge->source_attnum;

		if (directions[stage] == GGC_UNDIRECTED)
		{
			foreach_node(RestrictInfo, rinfo, available)
				if (pgq_undirected_topology(rinfo, edge, input, output))
				{
					if (input_qual)
						return false;
					input_qual = rinfo;
				}
			/* With the same bound vertex at both ends PostgreSQL simplifies
			 * (src=a AND dst=a) OR (dst=a AND src=a) to two equalities. */
			if (!input_qual && input->variable_id != output->variable_id)
				return false;
			if (input_qual)
			{
				*consumed = lappend(*consumed, input_qual);
				stage++;
				continue;
			}
		}
		foreach_ptr(RestrictInfo, rinfo, available)
		{
			if (join_var_pair(rinfo, stage_rtis[stage], input_edge_attnum,
					input->fallback_rti, input->identity_attnum))
			{
				if (input_qual)
					return false;
				input_qual = rinfo;
			}
			if (join_var_pair(rinfo, stage_rtis[stage], output_edge_attnum,
					vertex_rtis[stage + 1], output->identity_attnum))
			{
				if (output_qual)
					return false;
				output_qual = rinfo;
			}
		}
		if (!input_qual || !output_qual || input_qual == output_qual)
			return false;
		*consumed = lappend(*consumed, input_qual);
		*consumed = lappend(*consumed, output_qual);
		stage++;
	}
	foreach_node(RestrictInfo, rinfo, available)
	{
		if (list_member_ptr(*consumed, rinfo))
			continue;
		if (!route_uses_btree() || rinfo->security_level ||
			rinfo->pseudoconstant || contain_volatile_functions((Node *) rinfo->clause) ||
			expression_returns_set((Node *) rinfo->clause) ||
			pgq_binding_expression_unsafe((Node *) rinfo->clause, program))
			return false;
		*residual = lappend(*residual, rinfo);
		*consumed = lappend(*consumed, rinfo);
	}
	return true;
}

static bool
pgq_maybe_enable_endpoint_set(PlannerInfo *root, RelOptInfo *joinrel,
							 const int *vertex_rtis, CustomPath *path)
{
	Query *query = root->parse;
	TargetEntry *tle;
	SortGroupClause *distinct;
	Var *terminal;
	List *options;
	Size bytes;

	/* A deliberately narrow proof: this entire top-level SELECT requests
	 * only DISTINCT final-vertex bigint keys. No path output, aggregate,
	 * window, SRF, lateral parameter, or sibling relation can observe the
	 * discarded multiplicities. Leave the outer DISTINCT in place because
	 * separate anchor bindings can still reach the same terminal key. */
	if (root->parent_root || query->commandType != CMD_SELECT ||
		query->hasDistinctOn || list_length(query->distinctClause) != 1 ||
		query->hasAggs || query->hasWindowFuncs || query->hasTargetSRFs ||
		query->hasSubLinks || query->hasModifyingCTE || query->setOperations ||
		query->groupClause || query->groupingSets || query->havingQual ||
		query->windowClause || query->cteList || query->rowMarks ||
		query->limitCount || query->limitOffset ||
		!bms_equal(joinrel->relids, root->all_baserels) ||
		list_length(root->processed_tlist) != 1 ||
		list_length(joinrel->reltarget->exprs) != 1)
		return false;
	tle = linitial_node(TargetEntry, root->processed_tlist);
	distinct = linitial_node(SortGroupClause, query->distinctClause);
	if (tle->resjunk || !IsA(tle->expr, Var) ||
		tle->ressortgroupref != distinct->tleSortGroupRef)
		return false;
	terminal = castNode(Var, tle->expr);
	if (terminal->varno != vertex_rtis[cbo_config.stage_count] ||
		terminal->varattno != pgq_key_attnum(query->graph_program, terminal->varno) ||
		!GgcIntegerKeyType(terminal->vartype) ||
		distinct->eqop != get_opfamily_member(INTEGER_BTREE_FAM_OID,
			terminal->vartype, terminal->vartype, BTEqualStrategyNumber) ||
		terminal->varlevelsup != 0 || terminal->varnullingrels ||
		!equal(terminal, linitial(joinrel->reltarget->exprs)))
		return false;
	if (!path)
		return true;
	options = lsecond_node(List, path->custom_private);
	Assert(list_length(options) == 11);
	/* Unordered sets; upper PG nodes retain requested DISTINCT/ORDER BY. */
	lfirst(list_nth_cell(options, 10)) = makeInteger(false);
	bytes = (Size) work_mem * 1024;
	options = lappend(options, makeString(psprintf("%zu", bytes)));
	options = lappend(options, makeInteger(true));
	lsecond(path->custom_private) = options;
	return true;
}

/* A costing expression only: never becomes an executor Param or scan key.
 * A constant is usable only when this already-bound input enforces it.
 * Generic parameters and unknown neighbor keys retain ordinary PG estimates. */
static Expr *
pgq_estimated_lookup_value(PlannerInfo *root, Index input_rti)
{
	RelOptInfo *input = root->simple_rel_array[input_rti];
	int64 key;
	Param *unknown;

	foreach_node(RestrictInfo, rinfo, input->baserestrictinfo)
		if (pgq_key_equality_constant(rinfo, input_rti,
			pgq_key_attnum(root->parse->graph_program, input_rti), &key))
			return (Expr *) makeConst(INT8OID, -1, InvalidOid, sizeof(int64),
				Int64GetDatum(key), false, true);
	unknown = makeNode(Param);
	unknown->paramkind = PARAM_EXEC;
	unknown->paramid = -1;
	unknown->paramtype = INT8OID;
	unknown->paramtypmod = -1;
	unknown->paramcollid = InvalidOid;
	unknown->location = -1;
	return (Expr *) unknown;
}

static RestrictInfo *
pgq_lookup_restriction(PlannerInfo *root, Var *key, Expr *value, Oid equality)
{
	return make_simple_restrictinfo(root, make_opclause(equality, BOOLOID, false,
		(Expr *) key, value, InvalidOid, InvalidOid));
}

/* Ordinary column statistics are a read-only fallback, not a second store.
 * In particular, a middle-start candidate may probe the SAME known input in
 * two different stages. Its second branch must not use the global mean degree
 * and then multiply the input equality's selectivity into the output again. */
static void
pgq_fallback_estimates(PlannerInfo *root, const int *stage_rtis,
					  const int *vertex_rtis, const int *input_positions,
					  const GgcDirection *directions,
					  GgcGraphEstimateRequest *estimate)
{
	double *fanouts = palloc0(sizeof(double) * estimate->hops);
	double *ndistinct = palloc0(sizeof(double) * estimate->hops);

	for (uint32 i = 0; i < estimate->hops; i++)
	{
		RelOptInfo *edge = root->simple_rel_array[stage_rtis[i]];
		GgcVariable *mapped = pgq_rti_variable(root->parse->graph_program, stage_rtis[i]);
		VariableStatData data;
		bool isdefault;
		double n;
		Var *src = makeVar(stage_rtis[i], directions[i] == GGC_IN ?
			mapped->destination_attnum : mapped->source_attnum,
			INT8OID, -1, InvalidOid, 0);
		Var *dst = makeVar(stage_rtis[i], directions[i] == GGC_IN ?
			mapped->source_attnum : mapped->destination_attnum,
			INT8OID, -1, InvalidOid, 0);
		src->vartype = get_atttype(mapped->relation_oid, src->varattno);
		dst->vartype = get_atttype(mapped->relation_oid, dst->varattno);

		examine_variable(root, (Node *) src, 0, &data);
		n = get_variable_numdistinct(&data, &isdefault);
		if (!isdefault && n > 0)
			fanouts[i] = Max(edge->tuples / n, 1.0e-6);
		ReleaseVariableStats(data);
		if (directions[i] != GGC_UNDIRECTED)
		{
			Expr *value = pgq_estimated_lookup_value(root,
				vertex_rtis[input_positions[i]]);

			if (IsA(value, Const))
			{
				Oid equality = get_opfamily_member(INTEGER_BTREE_FAM_OID,
					src->vartype, exprType((Node *) value), BTEqualStrategyNumber);
				RestrictInfo *lookup = pgq_lookup_restriction(root, src, value,
					equality);

				fanouts[i] = Max(edge->tuples * clause_selectivity(root,
					(Node *) lookup, stage_rtis[i], JOIN_INNER, NULL), 1.0e-6);
			}
		}
		examine_variable(root, (Node *) dst, 0, &data);
		n = get_variable_numdistinct(&data, &isdefault);
		if (!isdefault && n > 0)
			ndistinct[i] = n;
		ReleaseVariableStats(data);
		if (directions[i] == GGC_UNDIRECTED)
		{
			/* Both endpoint probes contribute; overlap/self-loop selectivity is
			 * unknown, so overestimate rather than reuse directional profiles. */
			if (fanouts[i] > 0 && ndistinct[i] > 0)
				fanouts[i] += Max(edge->tuples / ndistinct[i], 1.0e-6);
			else
				fanouts[i] = 20.0;
			ndistinct[i] = Max(edge->tuples * 2, 1.0);
		}
	}
	estimate->fallback_fanouts = fanouts;
	estimate->endpoint_ndistinct = ndistinct;
}

/* An output key restriction propagated to the edge's opposite endpoint has
 * already reduced the edge candidates. The vertex still needs its actual
 * visibility/restriction check, but that SAME equality is not an independent
 * selectivity event. Keep every other vertex restriction, including payload
 * predicates, and require the same narrow integer-constant proof used for
 * an already-bound input key. */
static List *
pgq_output_selectivity_quals(GgcVariable *vertex, GgcVariable *edge,
							 GgcDirection direction, List *edge_quals,
							 List *vertex_quals)
{
	List *remaining = NIL;

	if (direction == GGC_UNDIRECTED)
		return list_copy(vertex_quals);
	foreach_ptr(RestrictInfo, vertex_qual, vertex_quals)
	{
		int64 vertex_value;
		bool implied = false;

		if (pgq_key_equality_constant(vertex_qual, vertex->fallback_rti,
				vertex->identity_attnum, &vertex_value))
		{
			foreach_ptr(RestrictInfo, edge_qual, edge_quals)
			{
				int64 edge_value;

				if (pgq_key_equality_constant(edge_qual, edge->fallback_rti,
						direction == GGC_OUT ? edge->destination_attnum : edge->source_attnum,
						&edge_value) && edge_value == vertex_value)
				{
					implied = true;
					break;
				}
			}
		}
		if (!implied)
			remaining = lappend(remaining, vertex_qual);
	}
	return remaining;
}

/* Match the executor's edge -> vertex -> completed-prefix filter order.
 * RestrictInfo/PG selectivity remains authoritative; only the time at which
 * a clause can affect later graph work is graph-specific. */
static void
pgq_stage_qual_estimates(PlannerInfo *root, GgcFixedProgram *program,
						 const int *stage_rtis, const int *vertex_rtis,
						 const int *input_positions, const GgcDirection *directions,
						 List *local_rinfos, List *residual,
						 GgcGraphEstimateRequest *estimate)
{
	GgcGraphStageQualEstimate *stages;
	Relids bound;
	List *scheduled = NIL;

	if (local_rinfos == NIL && residual == NIL)
		return;
	bound = bms_make_singleton(vertex_rtis[0]);
	stages = palloc0(sizeof(*stages) * estimate->hops);
	for (uint32 i = 0; i < estimate->hops; i++)
	{
		GgcGraphStageQualEstimate *stage = &stages[i];
		GgcVariable *input = pgq_rti_variable(program, vertex_rtis[input_positions[i]]);
		GgcVariable *edge = pgq_rti_variable(program, stage_rtis[i]);
		GgcVariable *vertex = pgq_rti_variable(program, vertex_rtis[i + 1]);
		List *edge_quals = pgq_input_restrictions_remaining(root, input, edge, directions[i]);
		List *vertex_quals = root->simple_rel_array[vertex_rtis[i + 1]]->baserestrictinfo;
		List *vertex_selectivity_quals;
		List *binding_quals = NIL;
		bool vertex_bound = bms_is_member(vertex_rtis[i + 1], bound);

		bound = bms_add_member(bound, stage_rtis[i]);
		bound = bms_add_member(bound, vertex_rtis[i + 1]);
		foreach_node(RestrictInfo, clause, residual)
			if (!list_member_ptr(scheduled, clause) &&
				bms_is_subset(clause->clause_relids, bound))
			{
				binding_quals = lappend(binding_quals, clause);
				scheduled = lappend(scheduled, clause);
			}
		stage->edge_selectivity = clauselist_selectivity(root, edge_quals,
			stage_rtis[i], JOIN_INNER, NULL);
		/* A previously bound variable already passed these restrictions,
		 * including when it is the filtered anchor of a closing cycle.
		 * Retain the actual recheck CPU, without a second independent s. */
		vertex_selectivity_quals = vertex_bound ? NIL :
			pgq_output_selectivity_quals(vertex, edge, directions[i], edge_quals, vertex_quals);
		stage->vertex_selectivity = clauselist_selectivity(root,
			vertex_selectivity_quals, vertex_rtis[i + 1], JOIN_INNER, NULL);
		list_free(vertex_selectivity_quals);
		stage->binding_selectivity = clauselist_selectivity(root, binding_quals,
			0, JOIN_INNER, NULL);
		cost_qual_eval(&stage->edge_cost, edge_quals, root);
		cost_qual_eval(&stage->vertex_cost, vertex_quals, root);
		cost_qual_eval(&stage->binding_cost, binding_quals, root);
		/* Production PGQ requests use predicate_clause_id=0: these are extra
		 * query predicates, not the collector's registered edge template.
		 * Never infer template identity from a matching selectivity. */
		stage->sampled_edge_clause_id = 0;
	}
	Assert(list_length(scheduled) == list_length(residual));
	estimate->stage_quals = stages;
	bms_free(bound);
	list_free(scheduled);
}

static Bitmapset *
pgq_cost_property_relids(GgcFixedProgram *program, List *expressions)
{
	Bitmapset *properties = NULL;

	foreach_node(Var, var, pull_var_clause((Node *) expressions, 0))
		foreach_node(GgcVariable, variable, program->variables)
			if (variable->fallback_rti == var->varno &&
				var->varattno != variable->identity_attnum &&
				(variable->entity_kind == GGC_VERTEX ||
				 (var->varattno != pgq_payload_attnum(variable) &&
				  ((var->varattno != variable->source_attnum &&
					var->varattno != variable->destination_attnum) ||
				   pgq_undirected_edge(program, variable->variable_id)))))
				properties = bms_add_member(properties, var->varno);
	return properties;
}

/* Prefix predicates can need a property before the final result exists.
 * Price each variable once at its stage, merging final output requirements
 * into the final predicate lookup. The executor's bounded last-row cache is
 * a possible saving, not an assumed global result cache or a fixed discount. */
static void
pgq_cost_stage_properties(GgcFixedProgram *program, const int *stage_rtis,
						  const int *vertex_rtis, List *residual,
						  List *outputs, GgcGraphPathResult *result)
{
	Relids bound = bms_make_singleton(vertex_rtis[0]);
	List *scheduled = NIL;

	for (uint32 i = 0; i < result->cost.stage_count; i++)
	{
		GgcGraphStageCost *stage = &result->cost.stages[i];
		List *clauses = NIL;
		Bitmapset *properties;
		Cost cost;

		bound = bms_add_member(bound, stage_rtis[i]);
		bound = bms_add_member(bound, vertex_rtis[i + 1]);
		foreach_node(RestrictInfo, clause, residual)
			if (!list_member_ptr(scheduled, clause) &&
				bms_is_subset(clause->clause_relids, bound))
			{
				clauses = lappend(clauses, clause->clause);
				scheduled = lappend(scheduled, clause);
			}
		properties = pgq_cost_property_relids(program, clauses);
		cost = bms_num_members(properties) * stage->binding_qual_rows *
			(2 * random_page_cost + cpu_index_tuple_cost + cpu_tuple_cost);
		if (i + 1 == result->cost.stage_count)
		{
			Bitmapset *output_properties = pgq_cost_property_relids(program, outputs);

			/* The same variable already fetched for this prefix predicate can
			 * serve its output. Output-only properties need surviving rows. */
			output_properties = bms_del_members(output_properties, properties);
			cost += bms_num_members(output_properties) * stage->next_bindings *
				(2 * random_page_cost + cpu_index_tuple_cost + cpu_tuple_cost);
			bms_free(output_properties);
		}
		stage->total_cost += cost;
		result->cost.total_cost += cost;
		result->path->path.total_cost += cost;
		bms_free(properties);
		list_free(clauses);
	}
	bms_free(bound);
	list_free(scheduled);
}

/* Fixed bag execution builds the earlier frontiers for the first 16-anchor
 * batch before the terminal stage starts yielding from its first 64-parent
 * batch (the exact budgets emitted by serialize_join_config). Endpoint sets
 * instead materialize a complete single-anchor result. Use the same priced
 * stage work, including early filters, for this startup estimate. Skew and an
 * empty first batch can still require more work before the first SQL row. */
static void
pgq_cost_fixed_startup(Path *anchor, bool endpoint_sets, GgcGraphPathResult *result)
{
	double anchors = Max(anchor->rows, 1.0);
	double fraction = Min(anchors, endpoint_sets ? 1.0 : 16.0) / anchors;
	Cost startup = anchor->startup_cost +
		(anchor->total_cost - anchor->startup_cost) * fraction;

	for (uint32 i = 0; i < result->cost.stage_count; i++)
	{
		GgcGraphStageCost *stage = &result->cost.stages[i];
		double stage_fraction = fraction;
		Cost fixed = stage->qual_startup_cost + result->cost.cost_profile.batch_startup;

		if (!endpoint_sets && i + 1 == result->cost.stage_count && stage->bindings > 0)
			stage_fraction = Min(stage->bindings * fraction, 64.0) / stage->bindings;
		stage->startup_cost = Min(stage->total_cost,
			fixed + Max(stage->total_cost - fixed, 0) * stage_fraction);
		startup += stage->startup_cost;
	}
	result->cost.startup_cost = startup - anchor->startup_cost;
	result->path->path.startup_cost = Min(startup, result->path->path.total_cost);
}

/* A candidate can revisit the same physical index through many RTE aliases.
 * Share identical scalar inputs and exact access-shape costs within that call.
 * No entry survives planning, and reaching the cap just stops memoization. */
typedef struct PgqIndexCostEntry
{
	IndexOptInfo *index;
	RangeTblEntry *rte;
	RTEPermissionInfo *permissions;
	Selectivity single;
	bool unique_proven;
	double loop_count;
	int keys;
	bool indexonly;
	bool has_statistics;
	ItemPointerData statistics_tid;
	TransactionId statistics_xmin;
	Cost cost;
} PgqIndexCostEntry;

typedef struct PgqIndexCostCache
{
	int count;
	PgqIndexCostEntry entries[32];
	List *array_parameters;
} PgqIndexCostCache;

/* The private cost expressions use the same unknown parameter at each array
 * position, with distinct synthetic IDs. Build their immutable leaves once
 * per candidate, growing only to the largest requested batch (at most 128).
 * Give each ArrayExpr its own List, so a later extension cannot change the
 * length of an already costed expression. No executable plan uses these. */
static List *
pgq_lookup_parameters(PgqIndexCostCache *cache, const Param *value, int keys)
{
	List *result;
	int present;

	Assert(keys > 1 && keys <= 128);
	if (cache->array_parameters != NIL)
	{
		Param *first = linitial_node(Param, cache->array_parameters);

		/* paramid is intentionally replaced below; every other field must
		 * match before sharing a leaf across separate cost expressions. */
		if (first->paramkind != value->paramkind ||
			first->paramtype != value->paramtype ||
			first->paramtypmod != value->paramtypmod ||
			first->paramcollid != value->paramcollid ||
			first->location != value->location)
			cache->array_parameters = NIL;
	}
	present = list_length(cache->array_parameters);
	if (present < keys)
	{
		Param *parameters = palloc(sizeof(*parameters) * (keys - present));

		for (int i = present; i < keys; i++)
		{
			Param *key = &parameters[i - present];

			*key = *value;
			key->paramid = i;
			cache->array_parameters = lappend(cache->array_parameters, key);
		}
	}
	result = list_copy_head(cache->array_parameters, keys);
#ifdef USE_ASSERT_CHECKING
	for (int i = 0; i < keys; i++)
	{
		Param expected = *value;

		expected.paramid = i;
		Assert(equal(&expected, list_nth(result, i)));
	}
#endif
	return result;
}

static bool
pgq_index_cost_inputs_match(const PgqIndexCostEntry *entry, IndexOptInfo *index,
					   RangeTblEntry *rte, RTEPermissionInfo *permissions,
					   bool unique_proven, bool has_statistics,
					   ItemPointer statistics_tid, TransactionId statistics_xmin)
{
	IndexOptInfo *old = entry->index;
	RelOptInfo *oldrel = old->rel;
	RelOptInfo *rel = index->rel;

	/* A locked physical index fixes key/opclass definitions. Still compare
	 * size estimates, which may have been collected separately for aliases.
	 * Unknown-key eqsel uses the column statistics, table tuple count and
	 * has_unique_index() proof. Check that native proof for EACH alias;
	 * partial-index predicates can make it differ between aliases. Retain
	 * the same permission context for all native costing. ANALYZE can
	 * change correlation without changing scalar selectivity, so also match
	 * the catalog tuple version used by examine_indexcol_variable(). */
	return old->indexoid == index->indexoid &&
		old->nkeycolumns == index->nkeycolumns && old->unique == index->unique &&
		old->indexkeys[0] == index->indexkeys[0] &&
		old->opfamily[0] == index->opfamily[0] &&
		old->opcintype[0] == index->opcintype[0] &&
		old->reverse_sort[0] == index->reverse_sort[0] &&
		old->pages == index->pages && old->tuples == index->tuples &&
		old->tree_height == index->tree_height &&
		old->reltablespace == index->reltablespace &&
		oldrel->pages == rel->pages && oldrel->tuples == rel->tuples &&
		oldrel->rows == rel->rows && oldrel->allvisfrac == rel->allvisfrac &&
		oldrel->reltablespace == rel->reltablespace &&
		entry->rte->relid == rte->relid && entry->rte->inh == rte->inh &&
		entry->unique_proven == unique_proven &&
		entry->has_statistics == has_statistics &&
		(!has_statistics ||
		 (ItemPointerEquals(&entry->statistics_tid, statistics_tid) &&
		  entry->statistics_xmin == statistics_xmin)) &&
		equal(entry->permissions, permissions);
}

/* Price an actual admitted index with the ordinary PG index cost machinery.
 * This temporary, non-executable path has only the lookup key: local graph
 * quals and projection have their own costs below. Copy metadata so neither
 * the native candidates nor their parameterization caches are changed.
 * cost_index accounts for correlation, effective_cache_size, tablespace costs
 * and the visibility map; it does not assume that all pages are resident. */
static Cost
pgq_btree_lookup_cost(PlannerInfo *root, Index rti, IndexOptInfo *original,
					 Expr *value, Index input_rti, double probes, bool indexonly,
					 int batch_size, PgqIndexCostCache *cache)
{
	RelOptInfo relation;
	IndexOptInfo index;
	IndexPath path;
	PathTarget target;
	IndexClause *clause;
	Var *keyvar;
	PgqIndexCostEntry *matched = NULL;
	RangeTblEntry *rte = planner_rt_fetch(rti, root);
	RTEPermissionInfo *permissions = rte->perminfoindex > 0 ?
		getRTEPermissionInfo(root->parse->rteperminfos, rte) : NULL;
	Selectivity single = -1;
	ItemPointerData statistics_tid;
	TransactionId statistics_xmin = InvalidTransactionId;
	bool has_statistics = false;
	bool unique_proven = false;
	bool builtin_eqsel;
	Oid keytype;
	Oid equality_op;
	double loop_count;
	bool memoize;
	double scans;
	int keys;

	if (probes <= 0)
		return 0;
	Assert(original && original->relam == BTREE_AM_OID);
	/* Cost the graph reader's admitted access shape, not the broader set of
	 * SAOP forms that ordinary PostgreSQL index scans can execute. INCLUDE
	 * columns are not keys; DESC and multi-key readers currently rescan for
	 * each endpoint and must not receive an array-scan cost. */
	if (!original->amsearcharray || !original->reverse_sort ||
		!GgcBtreeArrayIndexEligible(original->nkeycolumns,
			original->reverse_sort[0]))
		batch_size = 1;
	/* Do not charge a full last batch for a partial batch. At most two
	 * bounded cost_index calls are needed, even for a very large frontier. */
	if (!IsA(value, Const) && batch_size > 1 && probes > batch_size)
	{
		double full = floor(probes / batch_size);
		double tail = probes - full * batch_size;

		return full * pgq_btree_lookup_cost(root, rti, original, value,
			input_rti, batch_size, indexonly, batch_size, cache) +
			pgq_btree_lookup_cost(root, rti, original, value,
			input_rti, tail, indexonly, batch_size, cache);
	}
	/* Bound planner work by the executor's batch cap. Never manufacture
	 * endpoint values (or assume they form a contiguous range). */
	keys = IsA(value, Const) ? 1 : Min(Max(batch_size, 1), (int) Min(ceil(probes), 128.0));
	scans = ceil(probes / keys);
	loop_count = IsA(value, Const) ? 1.0 :
		Max(root->simple_rel_array[input_rti]->rows / keys, 1.0);

	/* build_index_tlist() already read the physical column's type. Reuse
	 * that native metadata, retaining the catalog lookup for a nonstandard
	 * hook-supplied target list. The opclass input type need not equal the
	 * column type and must not be substituted here. */
	keytype = InvalidOid;
	if (original->indextlist != NIL)
	{
		Expr *keyexpr = linitial_node(TargetEntry, original->indextlist)->expr;

		if (IsA(keyexpr, Var))
		{
			Var *key = (Var *) keyexpr;

			if (key->varno == rti && key->varlevelsup == 0 &&
				key->varattno == original->indexkeys[0])
				keytype = key->vartype;
		}
	}
	if (!OidIsValid(keytype))
		keytype = get_atttype(rte->relid, original->indexkeys[0]);
	Assert(keytype == get_atttype(rte->relid, original->indexkeys[0]));
	equality_op = get_opfamily_member(INTEGER_BTREE_FAM_OID,
		keytype, exprType((Node *) value), BTEqualStrategyNumber);
	builtin_eqsel = !IsA(value, Const) &&
		get_oprrest(equality_op) == F_EQSEL;
	/* Hook-supplied statistics can depend on RTE identity or call context.
	 * Do not reuse their estimates across aliases. Constants
	 * also retain normal native estimation, including MCV/histogram lookup. */
	memoize = IsA(value, Param) &&
		castNode(Param, value)->paramkind == PARAM_EXEC &&
		castNode(Param, value)->paramtype == INT8OID &&
		builtin_eqsel && rte->rtekind == RTE_RELATION && rte->securityQuals == NIL &&
		get_relation_stats_hook == NULL && get_index_stats_hook == NULL &&
		(amcostestimate_function) original->amcostestimate == btcostestimate;
	ItemPointerSetInvalid(&statistics_tid);
	if (memoize)
	{
		HeapTuple statistics;

		unique_proven = has_unique_index(original->rel, original->indexkeys[0]);
		/* Do not retain a syscache pin in the memo. Only plain columns and
		 * native statistics are admitted here; hooks disable reuse above. */
		statistics = SearchSysCache3(STATRELATTINH,
			ObjectIdGetDatum(rte->relid),
			Int16GetDatum(original->indexkeys[0]), BoolGetDatum(rte->inh));
		if (HeapTupleIsValid(statistics))
		{
			has_statistics = true;
			statistics_tid = statistics->t_self;
			statistics_xmin = HeapTupleHeaderGetRawXmin(statistics->t_data);
			ReleaseSysCache(statistics);
		}
		for (int i = 0; i < cache->count; i++)
		{
			PgqIndexCostEntry *entry = &cache->entries[i];
			bool same_shape = entry->loop_count == loop_count &&
				entry->keys == keys && entry->indexonly == indexonly;

			/* Once a scalar input matches, only an exact cost shape can
			 * supply anything else. Avoid repeating the metadata comparison
			 * for other batch lengths of the same physical index. */
			if (single >= 0 && !same_shape)
				continue;
			if (pgq_index_cost_inputs_match(entry, original, rte,
					permissions, unique_proven, has_statistics,
					&statistics_tid, statistics_xmin))
			{
				/* The scalar estimate is independent of batch length, scan
				 * repetitions and heap/index-only access. Reuse that input
				 * even when a different access shape requires native costing.
				 * Complete costs still require an exact shape match. */
				single = entry->single;
				if (!same_shape)
					continue;
				matched = entry;
#ifndef USE_ASSERT_CHECKING
				return scans * matched->cost;
#else
				/* Independently recompute every hit in assertion builds. */
				break;
#endif
			}
		}
	}
	/* A complete memo hit needs no expression tree. Operator/type lookup and
	 * every statistics, permission and uniqueness guard above still run. */
	clause = makeNode(IndexClause);
	keyvar = makeVar(rti, original->indexkeys[0], keytype, -1, InvalidOid, 0);
	/* A batch with an already known scalar estimate needs only its SAOP
	 * RestrictInfo. Do not build a scalar clause just to replace it below. */
	if (keys == 1 || single < 0)
		clause->rinfo = pgq_lookup_restriction(root, keyvar, value, equality_op);
	if (!IsA(value, Const) && builtin_eqsel)
	{
		if (single < 0)
			single = clause_selectivity(root, (Node *) clause->rinfo,
				rti, JOIN_INNER, NULL);
		else
			Assert(single == clause_selectivity(root,
				(Node *) (clause->rinfo != NULL ? clause->rinfo :
					pgq_lookup_restriction(root, keyvar, value, equality_op)),
				rti, JOIN_INNER, NULL));
		/* The one-key path uses this RestrictInfo directly, too. */
		if (clause->rinfo != NULL)
			clause->rinfo->norm_selec = single;
	}
	if (matched)
		Assert(matched->single == single);
	/* Recursive full/tail dispatch and memo hits need no private Path or
	 * relation copies. Materialize them only for a native cost calculation. */
	relation = *root->simple_rel_array[rti];
	index = *original;
	index.rel = &relation;
	index.indrestrictinfo = NIL;
	relation.baserestrictinfo = NIL;
	MemSet(&target, 0, sizeof(target));
	target.type = T_PathTarget;
	relation.reltarget = &target;
	MemSet(&path, 0, sizeof(path));
	if (keys > 1)
	{
		ScalarArrayOpExpr *array_qual = makeNode(ScalarArrayOpExpr);
		ArrayExpr *array = makeNode(ArrayExpr);

		/* Every element below is an unknown PARAM_EXEC of the same type.
		 * Built-in eqsel therefore returns the same selectivity for each,
		 * independently of paramid. Read its column statistics only once.
		 * Other estimators must retain scalararraysel's normal evaluation. */
		array->array_typeid = INT8ARRAYOID;
		array->element_typeid = INT8OID;
		array->location = -1;
		array->elements = pgq_lookup_parameters(cache, castNode(Param, value), keys);
		array_qual->opno = equality_op;
		array_qual->useOr = true;
		array_qual->args = list_make2(keyvar, array);
		array_qual->location = -1;
		clause->rinfo = make_simple_restrictinfo(root, (Expr *) array_qual);
		if (single >= 0)
		{
			Selectivity disjoint = 0;
			Selectivity independent = 0;

			/* Same recurrence and disjointness fallback as scalararraysel,
			 * including floating-point addition order. Cache on this private
			 * single-relation RestrictInfo, never on executable/native quals. */
			for (int i = 0; i < keys; i++)
			{
				independent = independent + single - independent * single;
				disjoint += single;
			}
			clause->rinfo->norm_selec = disjoint <= 1.0 ? disjoint : independent;
			CLAMP_PROBABILITY(clause->rinfo->norm_selec);
			Assert(clause->rinfo->norm_selec == scalararraysel(root,
				array_qual, false, rti, JOIN_INNER, NULL));
		}
	}
	clause->indexquals = list_make1(clause->rinfo);
	clause->indexcol = 0;
	path.path.type = T_IndexPath;
	path.path.pathtype = indexonly ? T_IndexOnlyScan : T_IndexScan;
	path.path.parent = &relation;
	path.path.pathtarget = &target;
	path.indexinfo = &index;
	path.indexclauses = list_make1(clause);
	path.indexscandir = ForwardScanDirection;
	/* Match indxpath.c:get_loop_count for a single inner-join parameter:
	 * its source baserel supplies the cache estimate, not the final join's
	 * cardinality. Constant index restrictions are unparameterized. Keep
	 * this distinct from the number of graph probes charged below, just as
	 * ordinary NestLoop costing multiplies its independently costed child. */
	cost_index(&path, root, loop_count, false);
	if (matched)
		Assert(matched->cost == path.path.total_cost);
	else if (memoize && cache->count < lengthof(cache->entries))
		cache->entries[cache->count++] = (PgqIndexCostEntry) {
			.index = original, .rte = rte, .permissions = permissions,
			.single = single, .unique_proven = unique_proven,
			.loop_count = loop_count, .keys = keys,
			.indexonly = indexonly, .has_statistics = has_statistics,
			.statistics_tid = statistics_tid, .statistics_xmin = statistics_xmin,
			.cost = path.path.total_cost
		};
	return scans * path.path.total_cost;
}

static bool
pgq_index_returns_attribute(const IndexOptInfo *index, AttrNumber attnum)
{
	for (int i = 0; i < index->ncolumns; i++)
		if (attnum > 0 && index->indexkeys[i] == attnum && index->canreturn[i])
			return true;
	return false;
}

/* Compare actual admitted indexes, not creation order. The alternatives are
 * independent per stage, so this is linear in the number of indexes and does
 * not introduce an index-choice Cartesian product into graph order search.
 * Covering eligibility is structural, but its benefit still comes from PG's
 * allvisfrac/correlation/tablespace model. A covering index need not win. */
static Oid
pgq_select_btree_index(PlannerInfo *root, Index rti, AttrNumber key,
					  AttrNumber opposite, AttrNumber identity, Expr *value,
					  Index input_rti, double probes, bool vertex,
					  bool allow_indexonly, int batch_size, Cost *lookup_cost,
					  PgqIndexCostCache *cache)
{
	RelOptInfo *rel = root->simple_rel_array[rti];
	Oid relationoid = planner_rt_fetch(rti, root)->relid;
	Oid chosen = InvalidOid;
	Cost best = DBL_MAX;

	foreach_node(IndexOptInfo, candidate, rel->indexlist)
	{
		Relation index;
		bool eligible;
		bool indexonly;
		Cost cost;

		CHECK_FOR_INTERRUPTS();
		if (candidate->hypothetical || candidate->relam != BTREE_AM_OID ||
			candidate->nkeycolumns < 1 || candidate->indexkeys[0] != key ||
			candidate->indpred != NIL || candidate->indexprs != NIL)
			continue;
		index = index_open(candidate->indexoid, AccessShareLock);
		eligible = vertex ?
			pgq_vertex_index_eligible(index, relationoid, key, false) :
			route_edge_index_eligible(index, key, opposite);
		/* Retain the ordinary planner's DDL lock. Only the selected OIDs are
		 * serialized; no Relation or PlannerInfo pointer escapes this call. */
		index_close(index, NoLock);
		if (!eligible)
			continue;
		indexonly = allow_indexonly && (vertex ||
			(pgq_index_returns_attribute(candidate, opposite) &&
			 pgq_index_returns_attribute(candidate, identity)));
		cost = pgq_btree_lookup_cost(root, rti, candidate, value,
			input_rti, probes, indexonly, batch_size, cache);
		if (!OidIsValid(chosen) || cost < best ||
			(cost == best && candidate->indexoid < chosen))
		{
			chosen = candidate->indexoid;
			best = cost;
		}
	}
	*lookup_cost = best;
	return chosen;
}

/* Price uncalibrated B-tree work with PG's actual index cost model. Measured
 * counts alone do not calibrate the generic page/CPU coefficients.
 * Calibrated provider profiles, graph_adj and AGE keep their existing contract.
 * Charge scalar index probes conservatively: batching is not a free discount. */
static bool
pgq_btree_fallback_cost(PlannerInfo *root, const int *stage_rtis,
						const int *vertex_rtis, const int *input_positions,
						const GgcDirection *directions, List *local_quals,
						bool endpoint_sets, GgcGraphPathResult *result,
						PgqStageIndexes **index_choices)
{
	Cost total = 0;
	PgqStageIndexes *choices;
	Cost *stage_costs;
	bool profile_matches = true;
	PgqIndexCostCache cache;

	*index_choices = NULL;
	if (!route_uses_btree() || enable_graph_age_adjacency)
		return false;
	cache.count = 0;
	cache.array_parameters = NIL;
	choices = palloc0(sizeof(*choices) * result->cost.stage_count);
	stage_costs = palloc0(sizeof(*stage_costs) * result->cost.stage_count);
	for (uint32 i = 0; i < result->cost.stage_count; i++)
	{
		GgcGraphStageCost *stage = &result->cost.stages[i];
		GgcVariable *edge = pgq_rti_variable(root->parse->graph_program, stage_rtis[i]);
		GgcVariable *vertex = pgq_rti_variable(root->parse->graph_program, vertex_rtis[i + 1]);
		Expr *value = pgq_estimated_lookup_value(root,
			vertex_rtis[input_positions[i]]);
		double outputs = endpoint_sets ? stage->next_endpoints : stage->next_bindings;
		double vertex_probes = endpoint_sets ? stage->next_endpoints : stage->vertex_probes;
		Cost access = 0;
		Cost lookup;
		bool stage_profile_matches = true;
		/* VM and the graph vertex array reader require heap semantics.
		 * Other TableAMs retain scalar, row-fetch costing. */
		bool vertex_indexonly = get_rel_relam(vertex->relation_oid) == HEAP_TABLE_AM_OID &&
			(local_quals == NIL || list_nth(local_quals, 2 * i + 1) == NIL);

		for (int side = 0; side < (directions[i] == GGC_UNDIRECTED ? 2 : 1); side++)
		{
			bool incoming = directions[i] == GGC_IN || side == 1;
			Oid indexoid = pgq_select_btree_index(root, stage_rtis[i],
				incoming ? edge->destination_attnum : edge->source_attnum,
				incoming ? edge->source_attnum : edge->destination_attnum,
				edge->identity_attnum, value, vertex_rtis[input_positions[i]],
				stage->requests, false,
				endpoint_sets && graph_edge_index_only,
				endpoint_sets && graph_edge_probe_batch ? graph_endpoint_batch_size : 1,
				&lookup, &cache);

			if (!OidIsValid(indexoid))
				return false;
			if (side == 0)
				choices[i].edge_index = indexoid;
			else
				choices[i].reverse_index = indexoid;
			stage_profile_matches &= indexoid == cbo_config.physical_scope.index_relid;
			access += lookup;
		}
		choices[i].vertex_index = pgq_select_btree_index(root, vertex_rtis[i + 1],
			vertex->identity_attnum, InvalidAttrNumber, InvalidAttrNumber,
			pgq_estimated_lookup_value(root, vertex_rtis[i + 1]), stage_rtis[i], vertex_probes,
			true, vertex_indexonly,
			endpoint_sets && graph_vertex_probe_batch && vertex_indexonly ?
			graph_vertex_batch_size : 1,
			&lookup, &cache);
		if (!OidIsValid(choices[i].vertex_index))
			return false;
		access += lookup;
		profile_matches &= stage_profile_matches;
		if (!stage_profile_matches)
		{
			/* Different index identities can have different dead entries and
			 * heap locality. Do not retain another index's physical counters
			 * even as the CPU component of the native access estimate. */
			stage->physical_fallback = true;
			stage->physical_entries = Max(stage->logical_edges, stage->requests);
			stage->edge_heap_fetches = stage->physical_entries;
			stage->heap_page_groups = stage->physical_entries;
			stage->peak_restore_bytes = 0;
		}
		stage_costs[i] = access + result->cost.cost_profile.batch_startup +
			stage->physical_entries * result->cost.cost_profile.adj_item +
			outputs * result->cost.cost_profile.binding_extend + stage->qual_cost;
		total += stage_costs[i];
	}
	*index_choices = choices;
	/* A provider profile is scoped to its physical index identity. Never
	 * transfer calibrated access costs to a newly selected different index.
	 * Logical cardinalities remain valid and are not discarded. */
	if (result->cost.cost_profile.calibrated && profile_matches)
	{
		pfree(stage_costs);
		return false;
	}
	if (!profile_matches)
	{
		result->cost.physical_state = GGC_STATS_ABSENT;
		result->cost.physical_generation = 0;
		result->cost.uncertainty_unknown = true;
	}
	result->path->path.total_cost =
		((Path *) linitial(result->path->custom_paths))->total_cost + total;
	result->cost.total_cost = total;
	for (uint32 i = 0; i < result->cost.stage_count; i++)
		result->cost.stages[i].total_cost = stage_costs[i];
	pfree(stage_costs);
	/* Startup is priced after all stage-local property/recheck work below. */
	return true;
}

/* Proof, NOT rows <= 1: an immediate unique bigint key constrained by a
 * supported non-null constant. Generic parameters and multiple anchors keep
 * DISTINCT. Register the proof index as a cached-plan dependency. */
static bool
pgq_single_anchor(PlannerInfo *root, RelOptInfo *anchor)
{
	bool equality = false;
	int64 value;
	AttrNumber keyatt = pgq_key_attnum(root->parse->graph_program, anchor->relid);

	if (anchor->reloptkind != RELOPT_BASEREL)
		return false;
	foreach_ptr(RestrictInfo, rinfo, anchor->baserestrictinfo)
		if (pgq_key_equality_constant(rinfo, anchor->relid, keyatt, &value))
			equality = true;
	if (!equality)
		return false;
	foreach_ptr(IndexOptInfo, index, anchor->indexlist)
	{
		if (index->unique && index->immediate && index->nkeycolumns == 1 &&
			index->indexkeys[0] == keyatt && index->indpred == NIL &&
			index->relam == BTREE_AM_OID &&
			get_opfamily_proc(index->opfamily[0], INT8OID, INT8OID,
				BTORDER_PROC) == F_BTINT8CMP)
		{
			root->glob->relationOids = list_append_unique_oid(
				root->glob->relationOids, index->indexoid);
			return true;
		}
	}
	return false;
}

static void
pgq_prepare_join_rows(PlannerInfo *root, List **prefixes, List **estimates)
{
	GgcFixedProgram *program = root->parse->graph_program;
	GgcFixedHop *first;
	GgcVariable *anchor, *edge0;
	GgcGraphEstimateRequest request = {0};
	GgcGraphLoadedPath loaded;
	Relids prefix = NULL;
	Relids required = NULL;
	List *consumed, *residual;
	int edges[4], vertices[5];
	GgcDirection directions[4];
	double refs[4] = {0};
	int64 key = 0;
	int hops = list_length(program->hops);
	int i = 0;
	bool bindings;
	bool anchor_implied = false;

	/* Only the collector's unfiltered, homogeneous, directed WALK domain.
	 * No graph access family is selected here; C-only builds use this too.
	 * These are estimates, never a uniqueness or join-elimination proof. */
	if (program->native_access || program->required_outer || hops < 2 || hops > 4 ||
		program->semantic_version != GGC_GRAPH_SEMANTIC_VERSION ||
		list_length(program->variables) != 2 * hops + 1 ||
		list_length(program->path_constraints) != 1 ||
		linitial_node(GgcPathConstraint, program->path_constraints)->mode != GGC_WALK ||
		root->join_info_list || root->placeholder_list)
		return;
	first = linitial_node(GgcFixedHop, program->hops);
	anchor = pgq_program_variable(program, first->input_variable);
	edge0 = pgq_program_variable(program, first->edge_variable);
	if (!anchor || !edge0 ||
		(first->direction != GGC_OUT && first->direction != GGC_IN))
		return;
	foreach_node(GgcVariable, variable, program->variables)
	{
		RelOptInfo *rel;
		RangeTblEntry *rte;
		Relation table;
		bool plain;

		if (variable->fallback_rti <= 0 || variable->fallback_rti >= root->simple_rel_array_size ||
			variable->key_type != INT8OID)
			return;
		rel = root->simple_rel_array[variable->fallback_rti];
		rte = planner_rt_fetch(variable->fallback_rti, root);
		if (!rel || rel->reloptkind != RELOPT_BASEREL || rel->lateral_relids ||
			rte->rtekind != RTE_RELATION || rte->securityQuals || rte->relid != variable->relation_oid)
			return;
		table = table_open(variable->relation_oid, AccessShareLock);
		plain = table->rd_rel->relkind == RELKIND_RELATION &&
			!table->rd_rel->relrowsecurity && !table->rd_rel->relhassubclass;
		table_close(table, NoLock);
		if (!plain)
			return;
	}
	if (!pgq_single_anchor(root, root->simple_rel_array[anchor->fallback_rti]))
		return;
	foreach_node(RestrictInfo, qual, root->simple_rel_array[anchor->fallback_rti]->baserestrictinfo)
		if (qual->security_level || !pgq_key_equality_constant(qual,
			anchor->fallback_rti, anchor->identity_attnum, &key))
			return;
	foreach_node(RestrictInfo, qual, root->simple_rel_array[edge0->fallback_rti]->baserestrictinfo)
	{
		int64 edge_key;

		if (pgq_key_equality_constant(qual, edge0->fallback_rti,
			first->direction == GGC_IN ? edge0->destination_attnum : edge0->source_attnum,
			&edge_key) && edge_key == key)
			anchor_implied = true;
	}
	vertices[0] = anchor->fallback_rti;
	prefix = bms_make_singleton(vertices[0]);
	foreach_node(GgcFixedHop, hop, program->hops)
	{
		GgcVariable *input = pgq_program_variable(program, hop->input_variable);
		GgcVariable *edge = pgq_program_variable(program, hop->edge_variable);
		GgcVariable *output = pgq_program_variable(program, hop->output_variable);

		if (!input || !edge || !output || input->fallback_rti != vertices[i] ||
			edge->element_oid != edge0->element_oid || output->element_oid != anchor->element_oid ||
			hop->direction != first->direction ||
			bms_is_member(edge->fallback_rti, prefix) || bms_is_member(output->fallback_rti, prefix) ||
			pgq_input_restrictions_remaining(root, input, edge, hop->direction) ||
			root->simple_rel_array[output->fallback_rti]->baserestrictinfo)
			return;
		edges[i] = edge->fallback_rti;
		vertices[i + 1] = output->fallback_rti;
		directions[i] = hop->direction;
		prefix = bms_add_member(prefix, edges[i]);
		prefix = bms_add_member(prefix, vertices[i + 1]);
		i++;
	}
	if (!bms_equal(prefix, root->all_baserels) ||
		!find_pgq_quals(program, internal_join_quals(root, prefix), edges, vertices,
			directions, &consumed, &residual) || residual)
		return;
	request.planner_root = root;
	request.logical_scope = (GgcStatsScope) {
		.graph_oid = program->graph_oid, .edge_element_oid = edge0->element_oid,
		.effective_role = GetUserId(), .endpoint_domain = edge0->relation_oid,
		.scope_kind = GGC_STATS_LOGICAL, .direction = first->direction,
		.path_mode = GGC_WALK, .result_mode = GGC_STATS_ALL_BINDINGS,
		.catalog_generation = Max(program->catalog_generation, 1), .security_generation = 1,
		.semantic_fingerprint = program->semantic_fingerprint & PG_INT64_MAX
	};
	request.physical_scope.scope_kind = GGC_STATS_PHYSICAL;
	request.cost_scope.scope_kind = GGC_STATS_COST;
	request.hops = hops;
	request.anchor_bindings = request.anchor_distinct = 1;
	request.ref_fractions = refs;
	GgcLoadGraphLogicalStatisticsForAnchor(&request, key, root->planner_cxt, &loaded);
	bindings = loaded.logical_state == GGC_STATS_READY;
	if (!bindings)
	{
		request.logical_scope.result_mode = GGC_STATS_ENDPOINT_SETS;
		GgcLoadGraphLogicalStatisticsForAnchor(&request, key, root->planner_cxt, &loaded);
		if (loaded.logical_state != GGC_STATS_READY)
			return;
	}
	prefix = bms_make_singleton(vertices[0]);
	required = anchor_implied ? NULL : bms_make_singleton(vertices[0]);
	for (i = 0; i < hops; i++)
	{
		GgcGraphLoadedStage *stage = &loaded.stages[i];
		double rows;

		/* A set sample cannot price bag multiplicity after any observed
		 * convergence. With no convergence at every preceding stage, the
		 * sampled bag and set counts agree. Still only a cardinality estimate. */
		if (!bindings && stage->accepted_edges.value != stage->next_endpoints.value)
			break;
		rows = bindings ? stage->next_bindings.value : stage->next_endpoints.value;
		required = bms_add_member(required, edges[i]);
		prefix = bms_add_member(prefix, edges[i]);
		prefix = bms_add_member(prefix, vertices[i + 1]);
		/* Native DP may delay unique vertex checks, including the anchor
		 * after its constant has propagated to the first edge. Match the
		 * same edge prefix with any subset of its unfiltered vertices. */
		*prefixes = lappend(*prefixes, list_make2(bms_copy(required), bms_copy(prefix)));
		*estimates = lappend(*estimates, makeFloat(psprintf("%.17g", rows)));
	}
}

void
GgcGraphJoinRows(PlannerInfo *root, RelOptInfo *rel, SpecialJoinInfo *sjinfo)
{
	typedef struct PrefixRows
	{
		PlannerInfo *root;
		List *prefixes;
		List *estimates;
	} PrefixRows;
	PrefixRows *cache = NULL;
	ListCell *p, *n;

	if (root->parse->graph_program_disabled || root->parse->hasRowSecurity ||
		root->parse->hasModifyingCTE || sjinfo->jointype != JOIN_INNER)
		return;
	foreach_ptr(PrefixRows, entry, route_cardinalities)
		if (entry->root == root)
			cache = entry;
	if (!cache)
	{
		MemoryContext old = MemoryContextSwitchTo(root->planner_cxt);

		cache = palloc0(sizeof(*cache));
		cache->root = root;
		route_cardinalities = lappend(route_cardinalities, cache);
		pgq_prepare_join_rows(root, &cache->prefixes, &cache->estimates);
		MemoryContextSwitchTo(old);
	}
	forboth(p, cache->prefixes, n, cache->estimates)
	{
		List *bounds = lfirst(p);

		if (bms_is_subset((Relids) linitial(bounds), rel->relids) &&
			bms_is_subset(rel->relids, (Relids) lsecond(bounds)))
		{
			rel->rows = clamp_row_est(floatVal(lfirst(n)));
			return;
		}
	}
}

static void
graph_upper_paths(PlannerInfo *root, UpperRelationKind stage,
				  RelOptInfo *input_rel, RelOptInfo *output_rel, void *extra)
{
	List *candidates;

	if (previous_graph_upper_paths_hook)
		previous_graph_upper_paths_hook(root, stage, input_rel, output_rel, extra);
	if (stage != UPPERREL_DISTINCT || root->parent_root)
		return;
	candidates = input_rel->pathlist;
	/* A slightly dearer join may still be cheaper than join + DISTINCT.
	 * Use the saved, owned Path even if ordinary join dominance discarded
	 * the original. Compete normally against complete upper paths below. */
	if (route_auto_active)
		foreach_ptr(RouteRegionConfig, region, route_regions)
			if (region->root == root && region->distinct_path)
				candidates = list_make1(region->distinct_path);
	foreach_ptr(Path, candidate, candidates)
	{
		CustomPath *path;
		List *options;
		Path *anchor;
		List *ordered = NIL;
		ListCell *lc;

		if (!IsA(candidate, CustomPath))
			continue;
		path = castNode(CustomPath, candidate);
		if (path->methods != &pgq_fixed_path_methods ||
			list_length(path->custom_private) < 3 ||
			list_length(path->custom_paths) != 1)
			continue;
		options = lsecond_node(List, path->custom_private);
		if (list_length(options) != 13 || !intVal(list_nth(options, 12)))
			continue;
		anchor = (Path *) linitial(path->custom_paths);
		if (!pgq_single_anchor(root, anchor->parent))
			continue;
		/*
		 * A sorted DISTINCT path can beat the unordered alternative under
		 * add_path's fuzzy cost comparison because it has useful pathkeys.
		 * Once uniqueness is proven, that ordering is useful here only for
		 * an explicit ORDER BY: this hook handles only the top-level query.
		 * Remove the redundant Unique paths before adding their replacements,
		 * retaining their sorts only when there is an ordering consumer.
		 * Otherwise fuzzy dominance can discard the cheaper unordered path
		 * in favor of a sort that was needed solely to implement DISTINCT.
		 * Do not extend the proof through projections or to another candidate
		 * (in particular, an AUTO relational alternative).  Keep the removed
		 * Paths allocated, since a replacement can share their descendants.
		 */
		foreach(lc, output_rel->pathlist)
		{
			Path *upper = lfirst(lc);
			Path *sorted;
			Path *source;

			if (!IsA(upper, UniquePath))
				continue;
			sorted = castNode(UniquePath, upper)->subpath;
			source = sorted;
			while (IsA(source, SortPath) || IsA(source, IncrementalSortPath))
			{
				if (IsA(source, SortPath))
					source = castNode(SortPath, source)->subpath;
				else
					source = castNode(IncrementalSortPath, source)->spath.subpath;
			}
			if (source != candidate)
				continue;
			if (root->sort_pathkeys != NIL)
				ordered = lappend(ordered, create_projection_path(root,
					output_rel, sorted, upper->pathtarget));
			output_rel->pathlist = foreach_delete_current(output_rel->pathlist, lc);
		}
		/* A no-op projection preserves the upper relation contract without
		 * changing Query.distinctClause or mutating a shared input Path. */
		add_path(output_rel, (Path *) create_projection_path(root, output_rel,
			candidate, candidate->pathtarget));
		foreach_ptr(Path, replacement, ordered)
			add_path(output_rel, replacement);
		list_free(ordered);
	}
}

static void
test_pgq_add_path(PlannerInfo *root, RelOptInfo *joinrel,
					   RelOptInfo *outerrel, RelOptInfo *innerrel,
					   JoinType jointype, JoinPathExtraData *extra,
					   GgcFixedProgram *program, int anchor_position)
{
	int		   *stage_rtis;
	int		   *vertex_rtis;
	int		   *input_positions;
	int		   *match_positions;
	GgcDirection *directions;
	List	   *available;
	List	   *consumed;
	List	   *local_quals;
	List	   *local_rinfos;
	List	   *residual;
	RelOptInfo *request_rel;
	Path	   *anchor_path;
	double	   *ref_fractions;
	GgcGraphPathRequest request = {0};
	GgcGraphPathResult result;
	PgqStageIndexes *index_choices = NULL;
	bool		undirected = false;
	bool		bound_keys = false;
	bool		canonical = program == root->parse->graph_program;
	bool		native_btree_cost;

	if (!program || root->parse->graph_program_disabled ||
		jointype != JOIN_INNER)
		return;
	if (bms_num_members(joinrel->relids) != list_length(program->variables))
		return;
	cbo_diagnostic.pgq_attempts++;
	if (root->parse->hasRowSecurity)
	{
		cbo_diagnostic.pgq_security_rejects++;
		return;
	}
	stage_rtis = palloc0(sizeof(*stage_rtis) * cbo_config.stage_count);
	vertex_rtis = palloc0(sizeof(*vertex_rtis) *
		(cbo_config.stage_count + 1));
	directions = palloc0(sizeof(*directions) * cbo_config.stage_count);
	input_positions = palloc0(sizeof(*input_positions) * cbo_config.stage_count);
	match_positions = palloc(sizeof(*match_positions) * cbo_config.stage_count);
	if (!pgq_program_contract(root, joinrel, program, stage_rtis,
			vertex_rtis, input_positions, directions, &local_quals, &local_rinfos))
	{
		cbo_diagnostic.pgq_contract_rejects++;
		return;
	}
	available = internal_join_quals(root, joinrel->relids);
	for (uint32 i = 0; i < cbo_config.stage_count; i++)
		undirected |= directions[i] == GGC_UNDIRECTED;
	if (!find_pgq_quals(program, available, stage_rtis, vertex_rtis,
			directions, &consumed, &residual))
	{
		cbo_diagnostic.pgq_contract_rejects++;
		return;
	}
	for (uint32 i = 0; i < cbo_config.stage_count; i++)
	{
		match_positions[i] = -1;
		for (uint32 previous = 0; previous <= i; previous++)
			if (vertex_rtis[i + 1] == vertex_rtis[previous])
			{
				match_positions[i] = previous;
				bound_keys = true;
				break;
			}
		/* Keep the original residual equality as a recheck. Only a proven
		 * already-bound bigint vertex key can become an early closing guard. */
		if (anchor_position >= 0)
			foreach_node(RestrictInfo, rinfo, residual)
				for (uint32 previous = 0; previous <= i; previous++)
					if (match_positions[i] < 0 &&
						join_var_pair(rinfo, vertex_rtis[i + 1],
							pgq_key_attnum(program, vertex_rtis[i + 1]),
							vertex_rtis[previous], pgq_key_attnum(program, vertex_rtis[previous])))
					{
						match_positions[i] = previous;
						bound_keys = true;
					}
	}
	/* Topology and local predicates have separate execution owners, but the
	 * common Path contract must account for both, not silently omit filters. */
	available = list_concat(available, list_copy(local_rinfos));
	consumed = list_concat(consumed, list_copy(local_rinfos));
	request_rel = root->simple_rel_array[vertex_rtis[0]];
	if (!request_rel || !request_rel->cheapest_total_path)
		return;
	anchor_path = request_rel->cheapest_total_path;
	ref_fractions = palloc0(sizeof(*ref_fractions) * cbo_config.stage_count);
	request.root = root;
	request.joinrel = joinrel;
	request.jointype = jointype;
	request.sjinfo = extra->sjinfo;
	request.covered_relids = joinrel->relids;
	request.available_quals = available;
	request.consumed_quals = consumed;
	request.required_outer = PATH_REQ_OUTER(anchor_path);
	request.anchor_path = anchor_path;
	request.ppi_outer_path = outerrel ? outerrel->cheapest_total_path : NULL;
	request.ppi_inner_path = innerrel ? innerrel->cheapest_total_path : NULL;
	request.estimate.planner_root = root;
	request.estimate.logical_scope = cbo_config.logical_scope;
	request.estimate.physical_scope = cbo_config.physical_scope;
	request.estimate.cost_scope = cbo_config.cost_scope;
	request.estimate.hops = cbo_config.stage_count;
	request.estimate.predicate_clause_id = cbo_config.predicate_clause_id;
	request.estimate.anchor_bindings = Max(anchor_path->rows, 1.0);
	request.estimate.anchor_distinct = test_anchor_distinct(root,
		vertex_rtis[0], request.estimate.anchor_bindings);
	request.estimate.ref_fractions = ref_fractions;
	request.estimate.max_profiles = GGC_GRAPH_STATS_DEFAULT_MAX_PROFILES;
	request.estimate.max_decoded_bytes = GGC_GRAPH_STATS_DEFAULT_MAX_BYTES;
	request.estimate.endpoint_sets = canonical && !undirected && !bound_keys && local_quals == NIL && residual == NIL && pgq_maybe_enable_endpoint_set(root,
		joinrel, vertex_rtis, NULL);
	if (request.estimate.endpoint_sets)
	{
		GgcVariable *first_edge = pgq_rti_variable(program, stage_rtis[0]);
		GgcVariable *first_vertex = pgq_rti_variable(program, vertex_rtis[0]);
		bool homogeneous = true;

		/* A set is a separate statistics population, not a discounted bag.
		 * The existing collector supports one homogeneous directional path.
		 * Only an actual key equality permits conditional profile lookup. */
		request.estimate.logical_scope.result_mode = GGC_STATS_ENDPOINT_SETS;
		request.estimate.physical_scope.result_mode = GGC_STATS_ENDPOINT_SETS;
		for (uint32 i = 0; i < cbo_config.stage_count; i++)
		{
			GgcVariable *edge = pgq_rti_variable(program, stage_rtis[i]);
			GgcVariable *vertex = pgq_rti_variable(program, vertex_rtis[i + 1]);

			if (edge->element_oid != first_edge->element_oid ||
				vertex->element_oid != first_vertex->element_oid ||
				directions[i] != directions[0])
				homogeneous = false;
		}
		if (homogeneous && pgq_single_anchor(root, request_rel))
			foreach_ptr(RestrictInfo, rinfo, request_rel->baserestrictinfo)
				if (pgq_key_equality_constant(rinfo, request_rel->relid,
					first_vertex->identity_attnum, &request.anchor_key))
				{
					request.has_anchor_key = true;
					request.estimate.anchor_distinct = 1.0;
					break;
				}
	}
	pgq_fallback_estimates(root, stage_rtis, vertex_rtis, input_positions,
		directions, &request.estimate);
	pgq_stage_qual_estimates(root, program, stage_rtis, vertex_rtis,
		input_positions, directions, local_rinfos, residual, &request.estimate);
	request.methods = &pgq_fixed_path_methods;
	if (!GgcCreateGraphPath(&request, root->planner_cxt, &result))
	{
		cbo_diagnostic.reject_reason = result.reject_reason;
		return;
	}
	native_btree_cost = pgq_btree_fallback_cost(root, stage_rtis, vertex_rtis,
		input_positions, directions, local_quals, request.estimate.endpoint_sets,
		&result, &index_choices);
	if (route_uses_btree() && !enable_graph_age_adjacency && !index_choices)
		return;
	result.path->custom_private = serialize_join_config(program, stage_rtis,
		vertex_rtis, directions, &result.cost,
		bms_num_members(joinrel->relids), list_length(consumed),
		bms_num_members(request.required_outer), vertex_rtis[0], index_choices);
	if (canonical && local_quals == NIL && residual == NIL && !undirected && !bound_keys)
		pgq_maybe_enable_endpoint_set(root, joinrel, vertex_rtis, result.path);
	else
	{
		/* Kept as expressions only until Plan construction moves them into
		 * custom_exprs: setrefs and normal expression dependencies must see them.
		 * Their stage cost/selectivity is already reflected in result.cost. */
		result.path->custom_private = lappend(result.path->custom_private, local_quals);
		result.path->custom_private = lappend(result.path->custom_private,
			extract_actual_clauses(residual, false));
	}
	if (anchor_position >= 0)
	{
		List *metadata = lthird_node(List, result.path->custom_private);
		List *inputs = NIL, *matches = NIL;

		for (uint32 i = 0; i < cbo_config.stage_count; i++)
		{
			inputs = lappend(inputs, makeInteger(input_positions[i]));
			matches = lappend(matches, makeInteger(match_positions[i]));
		}
		metadata = lappend(metadata, makeInteger(0)); /* Plan fills anchor resno */
		metadata = lappend(metadata, inputs);
		metadata = lappend(metadata, matches);
		metadata = lappend(metadata, makeInteger(anchor_position));
		metadata = lappend(metadata, makeInteger(1)); /* tried candidates */
		metadata = lappend(metadata, makeInteger(1)); /* candidate budget */
		metadata = lappend(metadata, makeInteger(false)); /* soft budget reached */
		lthird(result.path->custom_private) = metadata;
	}
	pgq_cost_stage_properties(program, stage_rtis, vertex_rtis, residual,
		joinrel->reltarget->exprs, &result);
	/* Prototype vertex rechecks are per binding, not free adjacency access. */
	for (uint32 i = 0; !native_btree_cost && i < result.cost.stage_count; i++)
	{
		GgcGraphStageCost *stage = &result.cost.stages[i];
		Cost cost = (request.estimate.endpoint_sets ? stage->next_endpoints : stage->vertex_probes) *
			(2 * random_page_cost + cpu_index_tuple_cost + cpu_tuple_cost);

		stage->total_cost += cost;
		result.cost.total_cost += cost;
		result.path->path.total_cost += cost;
	}
	pgq_cost_fixed_startup(anchor_path, request.estimate.endpoint_sets, &result);
	if (route_auto_active && request.estimate.endpoint_sets &&
		pgq_single_anchor(root, anchor_path->parent))
	{
		RouteRegionConfig *region = route_region(root);

		/* Only the canonical endpoint-set candidate can pass this proof.
		 * add_path frees rejected Path structs, but not their substructure.
		 * The anchor and expression lists already live in this planner's
		 * context; a separate struct is sufficient and survives pruning. */
		Assert(region->distinct_path == NULL);
		region->distinct_path = palloc(sizeof(CustomPath));
		memcpy(region->distinct_path, result.path, sizeof(CustomPath));
	}
	if (!route_candidate_mode && (route_active == ROUTE_BTREE || route_active == ROUTE_ADJ))
	{
		/* Strong selection at the exact admitted graph boundary. Costs remain
		 * truthful; never leave a cheaper fallback that could ignore a hint. */
		joinrel->pathlist = NIL;
		joinrel->partial_pathlist = NIL;
	}
	add_path(joinrel, &result.path->path);
	foreach_ptr(Path, retained, joinrel->pathlist)
		if (IsA(retained, CustomPath) &&
			castNode(CustomPath, retained)->methods == &pgq_fixed_path_methods)
		{
			cbo_diagnostic.pgq_paths_retained++;
			break;
		}
}

#include "graph_btree_search.inc"

#include "graph_pgq_native.inc"

/* The planner can interleave multiple subqueries (or recursively plan inside
 * a cost/selectivity function).  Never leave one region's configuration active
 * for a different region, nor cache any runtime state in this planner record. */
static void
test_pgq_set_join_pathlist(PlannerInfo *root, RelOptInfo *joinrel,
						 RelOptInfo *outerrel, RelOptInfo *innerrel,
						 JoinType jointype, JoinPathExtraData *extra)
{
	if (route_auto_active || route_active == ROUTE_BTREE || route_active == ROUTE_ADJ)
	{
		RouteRegionConfig *region;
		TestCboConfig saved_cbo = cbo_config;
		bool saved_candidate = route_candidate_mode;

		if (!root->parse->graph_program || root->parse->graph_program_disabled ||
			jointype != JOIN_INNER || bms_num_members(joinrel->relids) !=
			list_length(root->parse->graph_program->variables))
			return;
		region = route_region(root);
		if (!region->eligible || region->considered)
			return;
		region->considered = true;
		PG_TRY();
		{
			if (route_uses_btree())
			{
				route_candidate_mode = true;
				pgq_search_orders(root, joinrel, outerrel, innerrel, jointype, extra);
			}
			else
			{
				cbo_config = region->config;
				test_pgq_add_path(root, joinrel, outerrel, innerrel, jointype, extra,
					root->parse->graph_program, -1);
			}
		}
		PG_FINALLY();
		{
			cbo_config = saved_cbo;
			route_candidate_mode = saved_candidate;
		}
		PG_END_TRY();
	}
	else
		test_pgq_add_path(root, joinrel, outerrel, innerrel, jointype, extra,
			root->parse->graph_program, -1);
}

/*
 * Statement-scoped forced graph search.  Common base-relation setup still
 * checks permissions and chooses the anchor access path; there is no DP/GEQO
 * enumeration or executable relational join Plan for this region.  The typed
 * route uses temporary NestPaths only for native clause ownership and costing.
 * Build only a linear metadata spine using the core relation builder so that
 * targetlists, ECs and qual ownership retain the normal planner contracts.
 * These prefix RelOptInfos are metadata, not executable relational plans.
 */
static RelOptInfo *
route_graph_join_search(PlannerInfo *root, int levels_needed, List *initial_rels)
{
	GgcFixedProgram *program = root->parse->graph_program;
	Relids		all_relids = NULL;
	RelOptInfo *graphrel = NULL;
	SpecialJoinInfo *sjinfo = NULL;
	JoinPathExtraData extra = {0};

	/* Recursive planner calls without the hint retain standard behavior.
	 * The installed hook is restored even on ERROR by route_planner. */
	if ((route_active != ROUTE_BTREE && route_active != ROUTE_ADJ) ||
		(route_active == ROUTE_BTREE &&
		 !program && !root->parse->graph_program_disabled))
	{
		if (enable_geqo && levels_needed >= geqo_threshold)
			return geqo(root, levels_needed, initial_rels);
		return standard_join_search(root, levels_needed, initial_rels);
	}
	if (!program || root->parse->graph_program_disabled ||
		(route_active != ROUTE_BTREE && root->parse->hasRowSecurity) || root->join_info_list != NIL ||
		root->placeholder_list != NIL ||
		levels_needed != list_length(program->variables) ||
		list_length(initial_rels) != levels_needed || levels_needed < 2)
		route_error("The independent graph route requires one complete inner graph region.");

	foreach_ptr(RelOptInfo, rel, initial_rels)
	{
		if (rel->reloptkind != RELOPT_BASEREL ||
			bms_num_members(rel->relids) != 1 ||
			!bms_is_empty(rel->lateral_relids) ||
			bms_overlap(all_relids, rel->relids))
			route_error("Parameterized or prejoined graph inputs are not supported.");
		all_relids = bms_add_members(all_relids, rel->relids);
	}
	if (!bms_equal(all_relids, root->all_baserels))
		route_error("The independent graph route cannot split a graph across join searches.");
	if (route_active == ROUTE_BTREE &&
		(program->native_access || root->parse->hasRowSecurity))
		return pgq_native_join_search(root, program, false);

	foreach_ptr(RelOptInfo, rel, initial_rels)
	{
		Relids combined;

		if (!graphrel)
		{
			graphrel = rel;
			continue;
		}
		sjinfo = makeNode(SpecialJoinInfo);
		sjinfo->jointype = JOIN_INNER;
		sjinfo->min_lefthand = graphrel->relids;
		sjinfo->min_righthand = rel->relids;
		sjinfo->syn_lefthand = graphrel->relids;
		sjinfo->syn_righthand = rel->relids;
		combined = bms_union(graphrel->relids, rel->relids);
		graphrel = build_join_rel(root, combined, graphrel, rel, sjinfo,
								 NIL, NULL);
	}
	Assert(graphrel != NULL && sjinfo != NULL);
	extra.sjinfo = sjinfo;
	/* Reuse the exact same security/key/qual/output admission and costs.
	 * NULL physical children are safe only for these nonparameterized inputs;
	 * GgcCreateGraphPath obtains the sole executable child from the anchor. */
	test_pgq_set_join_pathlist(root, graphrel, NULL, NULL, JOIN_INNER, &extra);
	if (graphrel->pathlist == NIL && route_active == ROUTE_BTREE)
		return pgq_native_join_search(root, program, false);
	if (graphrel->pathlist == NIL)
		route_error("The graph region did not pass independent graph admission.");
	/* A partial retrieval can benefit from the existing depth-first native
	 * executor without materializing Fixed's prefix frontiers.  Keep the Fixed
	 * candidates in the ordinary add_path tournament: startup versus total cost
	 * and the final fractional-path choice remain PG's decisions.  Only use this
	 * root's fraction: core subquery propagation and upper operators still
	 * decide how much input is needed.  A pulled-up graph can share a root with
	 * a blocking operator, so a positive fraction does not promise early output.
	 * Full retrieval and AGE/graph_adj keep their existing search. */
	if (route_active == ROUTE_BTREE && !enable_graph_age_adjacency &&
		root->tuple_fraction > 0)
	{
		RelOptInfo *native_rel = pgq_native_join_search(root, program, true);

		Assert(native_rel == graphrel);
		graphrel = native_rel;
	}
	foreach_ptr(Path, path, graphrel->pathlist)
		if (!IsA(path, CustomPath) ||
			(castNode(CustomPath, path)->methods != &pgq_fixed_path_methods &&
			 castNode(CustomPath, path)->methods != &pgq_native_path_methods))
			route_error("A relational path escaped independent graph search.");
	graphrel->consider_parallel = false;
	set_cheapest(graphrel);
	return graphrel;
}

static void
test_fixed_set_join_pathlist(PlannerInfo *root, RelOptInfo *joinrel,
							 RelOptInfo *outerrel, RelOptInfo *innerrel,
							 JoinType jointype, JoinPathExtraData *extra)
{
	Index		request_rti;
	int		   *stage_rtis;
	List	   *available;
	List	   *consumed;
	RelOptInfo *request_rel;
	Path	   *anchor_path;
	double	   *ref_fractions;
	GgcGraphPathRequest request = {0};
	GgcGraphPathResult result;

	if (previous_set_join_pathlist_hook)
		previous_set_join_pathlist_hook(root, joinrel, outerrel, innerrel,
									   jointype, extra);
	if ((!cbo_config.enabled && !route_auto_active &&
		 route_active != ROUTE_BTREE && route_active != ROUTE_ADJ) ||
		jointype != JOIN_INNER)
		return;
	if (root->parse->graph_program != NULL)
	{
		test_pgq_set_join_pathlist(root, joinrel, outerrel, innerrel,
			jointype, extra);
		return;
	}
	/* A statement hint authorizes only the PGQ region, never the test-only
	 * relational-chain recognizer in a surrounding business query. */
	if (route_auto_active || route_active == ROUTE_BTREE || route_active == ROUTE_ADJ)
		return;
	if (
		bms_num_members(joinrel->relids) != cbo_config.stage_count + 1)
		return;
	cbo_diagnostic.exact_relid_attempts++;
	foreach_ptr(Path, candidate, joinrel->pathlist)
	{
		if (IsA(candidate, CustomPath) &&
			castNode(CustomPath, candidate)->methods == &pgq_fixed_path_methods)
			return;
	}

	stage_rtis = palloc0(sizeof(*stage_rtis) * cbo_config.stage_count);
	available = internal_join_quals(root, joinrel->relids);
	cbo_diagnostic.available_quals = list_length(available);
	cbo_diagnostic.qual_shape[0] = '\0';
	foreach_ptr(RestrictInfo, debug_rinfo, available)
	{
		if (IsA(debug_rinfo->clause, OpExpr))
		{
			OpExpr	   *op = castNode(OpExpr, debug_rinfo->clause);
			Node	   *left = strip_implicit_coercions(linitial(op->args));
			Node	   *right = strip_implicit_coercions(lsecond(op->args));

			if (IsA(left, Var) && IsA(right, Var))
			{
				Var		   *lv = castNode(Var, left);
				Var		   *rv = castNode(Var, right);
				int			used = strlen(cbo_diagnostic.qual_shape);

				snprintf(cbo_diagnostic.qual_shape + used,
						 sizeof(cbo_diagnostic.qual_shape) - used,
						 "%u.%d=%u.%d;", lv->varno, lv->varattno,
						 rv->varno, rv->varattno);
			}
		}
	}
	if (!find_graph_chain(root, joinrel->relids, available, &request_rti,
						  stage_rtis, &consumed) ||
		list_length(available) != cbo_config.stage_count)
		return;
	cbo_diagnostic.chain_matches++;
	cbo_diagnostic.consumed_quals = list_length(consumed);
	request_rel = root->simple_rel_array[request_rti];
	if (request_rel == NULL || request_rel->cheapest_total_path == NULL)
		return;
	if (bms_is_empty(joinrel->lateral_relids))
		anchor_path = request_rel->cheapest_total_path;
	else if (planner_rt_fetch(request_rti, root)->rtekind == RTE_SUBQUERY)
	{
		anchor_path = NULL;
		foreach_ptr(Path, candidate, request_rel->pathlist)
		{
			if (bms_equal(PATH_REQ_OUTER(candidate),
						  joinrel->lateral_relids) &&
				(anchor_path == NULL ||
				 compare_path_costs(candidate, anchor_path, TOTAL_COST) < 0))
				anchor_path = candidate;
		}
		if (anchor_path == NULL)
			return;
	}
	else
		anchor_path = create_seqscan_path(root, request_rel,
			bms_copy(joinrel->lateral_relids), 0);
	ref_fractions = palloc0(sizeof(*ref_fractions) * cbo_config.stage_count);

	request.root = root;
	request.joinrel = joinrel;
	request.jointype = jointype;
	request.sjinfo = extra->sjinfo;
	request.covered_relids = joinrel->relids;
	request.available_quals = available;
	request.consumed_quals = consumed;
	request.required_outer = PATH_REQ_OUTER(anchor_path);
	request.anchor_path = anchor_path;
	request.ppi_outer_path = outerrel->cheapest_total_path;
	request.ppi_inner_path = innerrel->cheapest_total_path;
	request.estimate.planner_root = root;
	request.estimate.logical_scope = cbo_config.logical_scope;
	request.estimate.physical_scope = cbo_config.physical_scope;
	request.estimate.cost_scope = cbo_config.cost_scope;
	request.estimate.hops = cbo_config.stage_count;
	request.estimate.predicate_clause_id = cbo_config.predicate_clause_id;
	request.estimate.anchor_bindings = Max(anchor_path->rows, 1.0);
	request.estimate.anchor_distinct = test_anchor_distinct(root,
		request_rti, request.estimate.anchor_bindings);
	request.estimate.ref_fractions = ref_fractions;
	request.estimate.max_profiles = GGC_GRAPH_STATS_DEFAULT_MAX_PROFILES;
	request.estimate.max_decoded_bytes = GGC_GRAPH_STATS_DEFAULT_MAX_BYTES;
	request.methods = &pgq_fixed_path_methods;

	if (!GgcCreateGraphPath(&request, root->planner_cxt, &result))
	{
		cbo_diagnostic.reject_reason = result.reject_reason;
		return;
	}
	cbo_diagnostic.contract_accepts++;
	cbo_diagnostic.startup_cost = result.path->path.startup_cost;
	cbo_diagnostic.total_cost = result.path->path.total_cost;
	result.path->custom_private = serialize_join_config(NULL, stage_rtis, NULL, NULL,
		&result.cost,
		bms_num_members(joinrel->relids), list_length(consumed),
		bms_num_members(request.required_outer), request_rti, NULL);
	add_path(joinrel, &result.path->path);
	foreach_ptr(Path, retained, joinrel->pathlist)
	{
		if (IsA(retained, CustomPath) &&
			castNode(CustomPath, retained)->methods == &pgq_fixed_path_methods)
		{
			cbo_diagnostic.paths_retained++;
			break;
		}
	}
}

static void
test_fixed_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel,
							Index rti, RangeTblEntry *rte)
{
	Path *subpath = NULL;
	CustomPath *path;

	if (previous_set_rel_pathlist_hook)
		previous_set_rel_pathlist_hook(root, rel, rti, rte);
	if (!test_config.enabled || rte->rtekind != RTE_RELATION ||
		rte->relid != test_config.request_relid ||
		rel->reloptkind != RELOPT_BASEREL || !target_has_request_columns(rel, rti))
		return;
	foreach_ptr(Path, candidate, rel->pathlist)
		if (!subpath || compare_path_costs(candidate, subpath, TOTAL_COST) < 0)
			subpath = candidate;
	if (!subpath)
		return;
	path = makeNode(CustomPath);
	path->path.pathtype = T_CustomScan;
	path->path.parent = rel;
	path->path.pathtarget = rel->reltarget;
	path->path.param_info = subpath->param_info;
	path->path.parallel_aware = false;
	path->path.parallel_safe = false;
	path->path.parallel_workers = 0;
	path->path.rows = Max(subpath->rows, 1.0);
	path->path.startup_cost = subpath->startup_cost;
	path->path.total_cost = subpath->startup_cost + 0.0001;
	path->path.pathkeys = NIL;
	path->flags = 0;
	path->custom_paths = list_make1(subpath);
	path->custom_private = serialize_config();
	path->methods = &pgq_fixed_path_methods;
	/* Explicit runtime-test mode only, activated by test_graph_fixed_config.
	 * It synthesizes paths from a request table for lifecycle/fault tests.
	 * Natural CBO tests use test_graph_cbo_config and the separate join hook.
	 * Preserve the anchor child: add_path must not free it as dominated. */
	rel->pathlist = NIL;
	add_path(rel, &path->path);
}


void
GgcInitializeGraphPlanner(void)
{
	static bool initialized;

	if (initialized)
		return;
	RegisterCustomScanMethods(&pgq_fixed_scan_methods);
	RegisterCustomScanMethods(&pgq_native_scan_methods);
	previous_set_rel_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = test_fixed_set_rel_pathlist;
	previous_set_join_pathlist_hook = set_join_pathlist_hook;
	set_join_pathlist_hook = test_fixed_set_join_pathlist;
	previous_route_planner = planner_hook;
	planner_hook = route_planner;
	previous_route_index_hook = ggc_graph_index_allowed_hook;
	ggc_graph_index_allowed_hook = route_index_allowed;
	previous_route_explain = explain_per_plan_hook;
	explain_per_plan_hook = route_explain;
	previous_graph_upper_paths_hook = create_upper_paths_hook;
	create_upper_paths_hook = graph_upper_paths;
	initialized = true;
}


/* The physical order may start in the middle of the written path.  Schedule
 * against the chosen binding positions, never the parser's variable order. */
static int
pgq_binding_qual_stage(Node *clause, List *metadata)
{
	List *edges = list_nth_node(List, metadata, 14);
	List *vertices = list_nth_node(List, metadata, 15);
	int stage = 0;

	foreach_node(Var, var, pull_var_clause(clause, 0))
	{
		int position = 0;
		bool found = false;

		foreach_node(Integer, rti, vertices)
		{
			if (var->varno == intVal(rti))
			{
				stage = Max(stage, position - 1);
				found = true;
				break;
			}
			position++;
		}
		if (found)
			continue;
		position = 0;
		foreach_node(Integer, rti, edges)
		{
			if (var->varno == intVal(rti))
			{
				stage = Max(stage, position);
				found = true;
				break;
			}
			position++;
		}
		if (!found || var->varlevelsup || var->varattno <= 0)
			return -1;
	}
	return stage;
}

static Plan *
pgq_plan_fixed_path(PlannerInfo *root, RelOptInfo *rel,
					 CustomPath *best_path, List *tlist,
					 List *clauses, List *custom_plans)
{
	CustomScan *scan = makeNode(CustomScan);
	Plan	   *subplan;
	bool		join_mode = list_length(best_path->custom_private) >= 3;
	List	   *serialized_stages = linitial_node(List,
		best_path->custom_private);
	List	   *local_quals = list_length(best_path->custom_private) >= 4 ?
		list_nth(best_path->custom_private, 3) : NIL;
	List	   *residual = list_length(best_path->custom_private) == 5 ?
		list_nth(best_path->custom_private, 4) : NIL;

	/* OIDs hidden in custom_private still participate in DDL invalidation. */
	foreach_node(List, stage, serialized_stages)
	{
		root->glob->relationOids = list_append_unique_oid(
			root->glob->relationOids, (Oid) intVal(list_nth(stage, 0)));
		root->glob->relationOids = list_append_unique_oid(
			root->glob->relationOids, (Oid) intVal(list_nth(stage, 3)));
		if (intVal(list_nth(stage, 5)))
			root->glob->relationOids = list_append_unique_oid(
				root->glob->relationOids, (Oid) intVal(list_nth(stage, 7)));
		if (list_length(stage) >= 15)
		{
			root->glob->relationOids = list_append_unique_oid(
				root->glob->relationOids, (Oid) intVal(list_nth(stage, 13)));
			root->glob->relationOids = list_append_unique_oid(
				root->glob->relationOids, (Oid) intVal(list_nth(stage, 14)));
		}
		if (pgq_stage_undirected(stage))
			root->glob->relationOids = list_append_unique_oid(
				root->glob->relationOids, (Oid) intVal(list_nth(stage, 15)));
	}
	if (join_mode)
		cbo_diagnostic.plan_builds++;

	if (list_length(custom_plans) != 1)
		elog(ERROR, "GraphExpandFixed needs exactly one anchor child");
	subplan = (Plan *) linitial(custom_plans);
	scan->scan.plan.targetlist = tlist;
	scan->scan.plan.qual = residual;
	scan->scan.scanrelid = 0;
	scan->flags = best_path->flags;
	scan->custom_plans = custom_plans;
	scan->custom_exprs = local_quals;
	scan->custom_private = list_copy(best_path->custom_private);
	if (join_mode)
		scan->custom_private = list_truncate(scan->custom_private, 3);
	/* Compile consecutive equal static stages into backward references.
	 * This is only plan metadata compression: live provider/ACL/snapshot
	 * validation remains in GgcFixedBegin and provider begin/rescan. Integers
	 * and Lists survive copyObject/equal/outfuncs; no runtime pointer is saved. */
	{
		List *encoded = NIL;
		List *previous = NIL;
		int position = 0;

		foreach_node(List, stage, linitial_node(List, best_path->custom_private))
		{
			if (previous && equal(stage, previous))
				encoded = lappend(encoded, makeInteger(position - 1));
			else
				encoded = lappend(encoded, stage);
			previous = stage;
			position++;
		}
		linitial(scan->custom_private) = encoded;
	}
	if (join_mode)
	{
		List	   *metadata = list_nth_node(List, best_path->custom_private, 2);
		Index		request_rti = intVal(list_nth(metadata, 13));
		List	   *stage_rtis = list_nth_node(List, metadata, 14);
		List	   *vertex_rtis = list_length(metadata) > 15 ?
			list_nth_node(List, metadata, 15) : NIL;
		List	   *mappings = NIL;
		List	   *scan_tlist;
		List	   *local_mappings = NIL;
		List	   *property_specs = NIL;
		List	   *needed = list_concat(pull_var_clause((Node *) tlist, 0),
			pull_var_clause((Node *) residual, 0));
		int			output_columns;
		AttrNumber anchor_column = InvalidAttrNumber;
		AttrNumber anchor_key = pgq_key_attnum(root->parse->graph_program, request_rti);

		/* Keep expressions in the normal projection; scan Datums have their
		 * original Var type, including when outputs contain implicit casts. */
		scan_tlist = add_to_flat_tlist(NIL, needed);
		output_columns = list_length(scan_tlist);
		foreach_node(TargetEntry, tle, subplan->targetlist)
			if (IsA(tle->expr, Var) && castNode(Var, tle->expr)->varno == request_rti &&
				castNode(Var, tle->expr)->varattno == anchor_key)
				anchor_column = tle->resno;
		if (anchor_column == InvalidAttrNumber)
			elog(ERROR, "GraphExpandFixed anchor child lost its key");
		metadata = list_copy(metadata);
		if (list_length(metadata) > 16)
			lfirst(list_nth_cell(metadata, 16)) = makeInteger(anchor_column);
		else
			metadata = lappend(metadata, makeInteger(anchor_column));
		lthird(scan->custom_private) = metadata;

		if (local_quals)
		{
			scan_tlist = add_to_flat_tlist(scan_tlist,
				pull_var_clause((Node *) local_quals, 0));
			foreach_node(List, quals, local_quals)
			{
				List *inputs = NIL;

				foreach_node(Var, var, pull_var_clause((Node *) quals, 0))
				{
					TargetEntry *tle = tlist_member((Expr *) var, scan_tlist);

					if (!tle)
						elog(ERROR, "GraphExpandFixed lost a local filter attribute");
					inputs = list_append_unique(inputs,
						list_make2_int(tle->resno, var->varattno));
				}
				local_mappings = lappend(local_mappings, inputs);
			}
		}
		foreach_ptr(TargetEntry, tle, scan_tlist)
		{
			Node	   *expression = (Node *) tle->expr;
			Var		   *var;
			int16		typlen;
			bool		typbyval;
			int			kind = 0;
			int			position = -1;
			int			stage = 0;
			AttrNumber keyatt;

			if (!IsA(expression, Var))
				elog(ERROR, "GraphExpandFixed join target is not a base Var");
			var = castNode(Var, expression);
			keyatt = pgq_key_attnum(root->parse->graph_program, var->varno);
			if (var->varno == request_rti && var->varattno == anchor_key)
			{
				kind = GGC_PGQ_OUTPUT_VERTEX;
				position = 0;
			}
			if (vertex_rtis != NIL && var->varattno == keyatt)
			{
				int			vertex = 0;

				foreach_ptr(Integer, rti_node, vertex_rtis)
				{
					if (var->varno == intVal(rti_node))
					{
						kind = GGC_PGQ_OUTPUT_VERTEX;
						position = vertex;
						break;
					}
					vertex++;
				}
			}
			foreach_ptr(Integer, rti_node, stage_rtis)
			{
				if (var->varno == intVal(rti_node))
				{
					List *stage_spec = list_nth_node(List, serialized_stages, stage);
					bool incoming = intVal(lsecond(stage_spec)) == GGC_IN;
					bool undirected = pgq_stage_undirected(stage_spec);
					bool mapped = list_length(stage_spec) == PGQ_STAGE_FIELDS;
					AttrNumber identity = mapped ? intVal(list_nth(stage_spec, PGQ_STAGE_IDENTITY)) : 1;
					AttrNumber source = mapped ? intVal(list_nth(stage_spec, PGQ_STAGE_SOURCE)) : 2;
					AttrNumber destination = mapped ? intVal(list_nth(stage_spec, PGQ_STAGE_DESTINATION)) : 3;
					AttrNumber payload = mapped ? intVal(list_nth(stage_spec, PGQ_STAGE_PAYLOAD)) : 4;
					int input = list_length(metadata) > 17 ?
						intVal(list_nth(list_nth_node(List, metadata, 17), stage)) : stage;

					if (var->varattno == identity)
					{
						kind = GGC_PGQ_OUTPUT_EDGE;
						position = stage;
					}
					else if (var->varattno == source || var->varattno == destination)
					{
						kind = undirected ? 0 : GGC_PGQ_OUTPUT_VERTEX;
						position = (var->varattno == source) == incoming ? stage + 1 : input;
					}
					else if (var->varattno == payload)
					{
						kind = GGC_PGQ_OUTPUT_PAYLOAD;
						position = stage;
					}
				}
				stage++;
			}
			/* Local-only columns are read from the local callback's visible
			 * row.  This also applies to the conventional payload column: do
			 * not copy it into every prefix merely to evaluate one predicate. */
			if (tle->resno > output_columns)
			{
				kind = GGC_PGQ_OUTPUT_FILTER_ONLY;
				position = -1;	/* supplied only by the local row callback */
			}
			if (kind == 0)
			{
				int property = 0;
				int entity = 0;
				int binding = 0;
				Oid relationoid = planner_rt_fetch(var->varno, root)->relid;
				Oid indexoid;

				foreach_node(List, spec, property_specs)
				{
					if (intVal(linitial(spec)) == var->varno)
						break;
					property++;
				}
				if (property == list_length(property_specs))
				{
					foreach_ptr(Integer, rti_node, vertex_rtis)
					{
						if (intVal(rti_node) == var->varno)
						{
							entity = GGC_PGQ_OUTPUT_VERTEX;
							break;
						}
						binding++;
					}
					if (!entity)
					{
						binding = 0;
						foreach_ptr(Integer, rti_node, stage_rtis)
						{
							if (intVal(rti_node) == var->varno)
							{
								entity = GGC_PGQ_OUTPUT_EDGE;
								break;
							}
							binding++;
						}
					}
					indexoid = pgq_vertex_index(relationoid, keyatt);
					if (!entity || !OidIsValid(indexoid))
						elog(ERROR, "GraphExpandFixed cannot map property Var %u.%d",
							 var->varno, var->varattno);
					property_specs = lappend(property_specs, lappend(list_make5(
						makeInteger(var->varno), makeInteger(relationoid),
						makeInteger(indexoid), makeInteger(entity), makeInteger(binding)), makeInteger(keyatt)));
					root->glob->relationOids = list_append_unique_oid(
						root->glob->relationOids, indexoid);
				}
				kind = GGC_PGQ_OUTPUT_PROPERTY;
				position = property;
			}
			get_typlenbyval(var->vartype, &typlen, &typbyval);
			mappings = lappend(mappings,
				list_make5(makeInteger(kind), makeInteger(position),
					makeInteger(typbyval), makeInteger(typlen), makeInteger(var->varattno)));
		}
		scan->custom_private = lappend(scan->custom_private, mappings);
		if (local_quals || property_specs || residual)
		{
			scan->custom_private = lappend(scan->custom_private, local_mappings);
			scan->custom_private = lappend(scan->custom_private, property_specs);
		}
		scan->custom_scan_tlist = scan_tlist;
		if (residual)
		{
			int count = list_length(serialized_stages);
			List *stage_quals = NIL;
			List *remaining = NIL;

			/* custom_exprs must stay visible to setrefs/copy/read and parameter
			 * dependency processing.  First 2*N lists are local row predicates;
			 * the optional next N lists are complete-prefix predicates. */
			if (scan->custom_exprs == NIL)
				for (int i = 0; i < 2 * count; i++)
					scan->custom_exprs = lappend(scan->custom_exprs, NIL);
			for (int i = 0; i < count; i++)
				stage_quals = lappend(stage_quals, NIL);
			foreach_ptr(Node, clause, residual)
			{
				int stage = pgq_binding_qual_stage(clause, metadata);

				if (stage < 0 || stage >= count || contain_volatile_functions(clause))
					remaining = lappend(remaining, clause);
				else
					lfirst(list_nth_cell(stage_quals, stage)) =
						lappend(list_nth(stage_quals, stage), clause);
			}
			scan->custom_exprs = list_concat(list_copy(scan->custom_exprs), stage_quals);
			scan->scan.plan.qual = remaining;
		}
	}
	else
		scan->custom_scan_tlist = subplan->targetlist;
	scan->custom_relids = rel->relids;
	scan->methods = &pgq_fixed_scan_methods;
	scan->custom_private = GgcPgqEncodeFixedPlan(scan->custom_private);
	return &scan->scan.plan;
}

#include "graph_pgq_diagnostic.inc"
#include "graph_pgq_exec.inc"
