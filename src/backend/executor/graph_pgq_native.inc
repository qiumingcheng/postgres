/*
 * Typed graph bindings over native PG access plans.
 *
 * The integer provider remains the optimized route.  This route deliberately
 * keeps native RLS, view, partition, FDW, index proof and expression semantics.
 * It is a streaming depth-first graph binding machine, not a wrapper around a
 * complete relational join Plan.  No graph key is truncated or hashed to int64.
 */
typedef struct PgqNativePath
{
	CustomPath path;
	List *access_paths;
	List *stage_quals;
	List *gating_quals;
	int hops;
} PgqNativePath;

typedef struct PgqNativeStage
{
	PlanState *child;
	TupleTableSlot *binding;
	ExprState *qual;
	const GgcPgqNativeStage *description;
	bool restart;
} PgqNativeStage;

typedef struct PgqNativeState
{
	CustomScanState css;
	GgcPgqNativePlan *description;
	PgqNativeStage *stages;
	int count;
	int depth;
	uint64 rescans;
	bool gated;
} PgqNativeState;

static Plan *pgq_native_plan(PlannerInfo *, RelOptInfo *, CustomPath *, List *, List *, List *);
static Node *pgq_native_state(CustomScan *);
static void pgq_native_begin(CustomScanState *, EState *, int);
static TupleTableSlot *pgq_native_exec(CustomScanState *);
static void pgq_native_end(CustomScanState *);
static void pgq_native_rescan(CustomScanState *);
static void pgq_native_explain(CustomScanState *, List *, ExplainState *);

static const CustomPathMethods pgq_native_path_methods = {
	.CustomName = "GraphExpandNative",
	.PlanCustomPath = pgq_native_plan
};
static const CustomScanMethods pgq_native_scan_methods = {
	.CustomName = "GraphExpandNative",
	.CreateCustomScanState = pgq_native_state
};
static const CustomExecMethods pgq_native_exec_methods = {
	.CustomName = "GraphExpandNative",
	.BeginCustomScan = pgq_native_begin,
	.ExecCustomScan = pgq_native_exec,
	.EndCustomScan = pgq_native_end,
	.ReScanCustomScan = pgq_native_rescan,
	.ExplainCustomScan = pgq_native_explain
};

/* Cost the actual child that will be installed under GraphExpandNative.
 * Memoize/Materialize must win the same rescan-cost comparison as plain
 * access; they are never injected after path selection.  The temporary
 * NestPath owns the native parameter-clause division, but is not emitted
 * as a NestLoop Plan. */
static NestPath *
pgq_native_consider_inner(PlannerInfo *root, RelOptInfo *joined,
						 Path *outer, Path *inner, List *restrictlist,
						 JoinPathExtraData *extra, uint64 subtype,
						 NestPath *best)
{
	JoinCostWorkspace costs;
	NestPath   *candidate;

	initial_cost_nestloop(root, &costs, JOIN_INNER,
						 subtype | PGS_CONSIDER_NONPARTIAL,
						 outer, inner, extra);
	candidate = create_nestloop_path(root, joined, JOIN_INNER, &costs,
									extra, outer, inner, restrictlist, NIL, NULL);
	if (best == NULL ||
		compare_path_costs((Path *) candidate, (Path *) best, TOTAL_COST) < 0)
		return candidate;
	return best;
}

static RelOptInfo *
pgq_native_join_search(PlannerInfo *root, GgcFixedProgram *program,
					   bool retain_fixed)
{
	List *order = NIL;
	List *access = NIL;
	List *quals = NIL;
	List *gating = NIL;
	RelOptInfo *prefix = NULL;
	Path *prefix_path = NULL;
	PgqNativePath *result;
	const IndexAmRoutine *btree = GetIndexAmRoutineByAmId(BTREE_AM_OID, false);
	if (btree->amgetgraphprovider == NULL || btree->amgetgraphprovider() == NULL)
		route_error("The B-tree graph provider is not present in this build.");
	if (enable_graph_age_adjacency)
		route_error("Typed native access does not expand the AGE adjacency contract.");

	/* A bounded graph order, never an unrestricted JOIN subset search. */
	foreach_node(GgcFixedHop, hop, program->hops)
	{
		int ids[3] = {hop->input_variable, hop->edge_variable, hop->output_variable};
		for (int j = 0; j < 3; j++)
			if (!list_member_int(order, ids[j]))
				order = lappend_int(order, ids[j]);
	}
	if (list_length(order) != list_length(program->variables))
		route_error("Typed graph stages must cover every binding.");
	foreach_int(id, order)
	{
		GgcVariable *variable = pgq_program_variable(program, id);
		RelOptInfo *rel = root->simple_rel_array[variable->fallback_rti];
		NestPath *best = NULL;
		RelOptInfo *joined;
		List *restrictlist;
		SpecialJoinInfo *sjinfo;
		JoinPathExtraData extra = {0};
		Path *material;

		if (!rel || rel->reloptkind != RELOPT_BASEREL)
			route_error("Typed graph binding lost its native relation boundary.");
		if (!prefix)
		{
			prefix_path = get_cheapest_path_for_pathkeys(rel->pathlist, NIL,
				NULL, TOTAL_COST, false);
			if (!prefix_path)
				route_error("No native anchor access path is available.");
			prefix = rel;
			access = lappend(access, prefix_path);
			quals = lappend(quals, NIL);
			continue;
		}
		sjinfo = makeNode(SpecialJoinInfo);
		sjinfo->jointype = JOIN_INNER;
		sjinfo->min_lefthand = sjinfo->syn_lefthand = prefix->relids;
		sjinfo->min_righthand = sjinfo->syn_righthand = rel->relids;
		joined = build_join_rel(root, bms_union(prefix->relids, rel->relids),
			prefix, rel, sjinfo, NIL, &restrictlist);
		/* A constant-false graph WHERE may replace ALL ordinary join quals.
		 * It must gate the complete graph before any child access, not be
		 * discarded when extracting per-binding (nonconstant) predicates. */
		gating = list_concat_unique(gating, extract_actual_clauses(restrictlist, true));
		extra.sjinfo = sjinfo;
		extra.restrictlist = restrictlist;
		/* This is graph costing, not an executable disabled NestLoop. Native
		 * child scan disabled-node costs remain intact.  Honor the independent
		 * cache switches even when enable_nestloop is off.  Native stages read
		 * each binding to completion, so inner_unique deliberately stays false;
		 * no single-row cache completion or early-stop cost is assumed. */
		extra.pgs_mask = PGS_NESTLOOP_PLAIN | PGS_CONSIDER_NONPARTIAL;
		if (enable_memoize)
			extra.pgs_mask |= PGS_NESTLOOP_MEMOIZE;
		if (enable_material)
			extra.pgs_mask |= PGS_NESTLOOP_MATERIALIZE;
		foreach_ptr(Path, inner, rel->pathlist)
		{
			Path *memoized;

			if (!bms_is_subset(PATH_REQ_OUTER(inner), prefix->relids) ||
				inner->parallel_aware)
				continue;
			best = pgq_native_consider_inner(root, joined, prefix_path, inner,
				restrictlist, &extra, PGS_NESTLOOP_PLAIN, best);
			/* Shared PG eligibility includes every parameter key, hash equality,
			 * binary comparisons and volatile expressions.  Executor invalidation
			 * and cache memory limits remain those of the actual Memoize node. */
			memoized = get_memoize_path(root, rel, prefix, inner, prefix_path,
				JOIN_INNER, &extra);
			if (memoized != NULL)
				best = pgq_native_consider_inner(root, joined, prefix_path, memoized,
					restrictlist, &extra, PGS_NESTLOOP_MEMOIZE, best);
		}
		/* Like ordinary nested loops, materialize only the cheapest inner
		 * access that does not depend on this prefix.  Reject any remaining
		 * outside dependency using the same stage-dependency rule as above. */
		material = rel->cheapest_total_path;
		if (material != NULL &&
			(!bms_is_empty(PATH_REQ_OUTER(material)) || material->parallel_aware))
			material = NULL;
		material = get_materialize_path(rel, material, extra.pgs_mask);
		if (material != NULL)
			best = pgq_native_consider_inner(root, joined, prefix_path, material,
				restrictlist, &extra, PGS_NESTLOOP_MATERIALIZE, best);
		if (!best)
			route_error("No native access path satisfies graph stage dependencies.");
		access = lappend(access, best->jpath.innerjoinpath);
		quals = lappend(quals, extract_actual_clauses(best->jpath.joinrestrictinfo, false));
		prefix = joined;
		prefix_path = (Path *) best;
	}
	result = palloc0(sizeof(*result));
	NodeSetTag(&result->path, T_CustomPath);
	result->path.path = *prefix_path;
	NodeSetTag(&result->path.path, T_CustomPath);
	result->path.path.pathtype = T_CustomScan;
	result->path.path.pathkeys = NIL;
	result->path.path.parallel_safe = false;
	result->path.path.parallel_aware = false;
	result->path.path.parallel_workers = 0;
	result->path.methods = &pgq_native_path_methods;
	result->access_paths = access;
	result->stage_quals = quals;
	result->gating_quals = gating;
	result->hops = list_length(program->hops);
	/* The standalone native route replaces all ordinary join candidates.
	 * When partial retrieval also admits Fixed, preserve those graph paths for
	 * normal PG startup/total-cost pruning and fractional-path selection. */
	if (!retain_fixed)
		prefix->pathlist = NIL;
	prefix->partial_pathlist = NIL;
	prefix->consider_parallel = false;
	add_path(prefix, &result->path.path);
	set_cheapest(prefix);
	return prefix;
}

static Plan *
pgq_native_plan(PlannerInfo *root, RelOptInfo *rel, CustomPath *path,
				List *tlist, List *clauses, List *unused_plans)
{
	PgqNativePath *native = (PgqNativePath *) path;
	CustomScan *scan = makeNode(CustomScan);
	List *params;
	List *maps = NIL;
	List *parameter_maps = NIL;
	List *flat = NIL;

	scan->methods = &pgq_native_scan_methods;
	scan->scan.plan.targetlist = tlist;
	scan->scan.plan.qual = native->gating_quals;
	scan->custom_plans = create_graph_stage_plans(root, native->access_paths, &params);
	/* Each child projects only the columns needed by output, joins and quals. */
	foreach_ptr(Plan, plan, scan->custom_plans)
	{
		List *map = NIL;
		foreach_node(TargetEntry, te, plan->targetlist)
		{
			TargetEntry *existing = tlist_member(te->expr, flat);
			if (!existing)
			{
				existing = makeTargetEntry(copyObject(te->expr), list_length(flat)+1,
					NULL, false);
				flat = lappend(flat, existing);
			}
			map = lappend_int(map, existing->resno);
		}
		maps = lappend(maps, map);
	}
	foreach_ptr(List, stage, params)
	{
		List *map = NIL;
		foreach_node(NestLoopParam, param, stage)
		{
			TargetEntry *entry = tlist_member((Expr *) param->paramval, flat);
			ListCell *idcell;
			ListCell *exprcell;
			bool existing = false;

			if (!entry)
				elog(ERROR, "graph parameter is missing from native binding target");
			map = lappend(map, list_make2_int(param->paramno, entry->resno));
			/* Keep the source visible to core setrefs/dependency/deparse code.
			 * Private slot numbers alone are insufficient for a PARAM_EXEC that
			 * appears inside an ordinary child IndexScan or Memoize node. */
			forboth(idcell, scan->custom_param_ids, exprcell, scan->custom_param_exprs)
				if (lfirst_int(idcell) == param->paramno)
				{
					if (!equal(lfirst(exprcell), param->paramval))
						elog(ERROR, "graph parameter has inconsistent binding sources");
					existing = true;
					break;
				}
			if (!existing)
			{
				scan->custom_param_ids = lappend_int(scan->custom_param_ids, param->paramno);
				scan->custom_param_exprs = lappend(scan->custom_param_exprs,
					copyObject(param->paramval));
			}
			scan->custom_params = bms_add_member(scan->custom_params, param->paramno);
		}
		parameter_maps = lappend(parameter_maps, map);
	}
	scan->custom_scan_tlist = flat;
	scan->custom_exprs = native->stage_quals;
	scan->custom_private = GgcPgqEncodeNativePlan(maps, parameter_maps, native->hops);
	return &scan->scan.plan;
}

static Node *
pgq_native_state(CustomScan *scan)
{
	PgqNativeState *state = palloc0(sizeof(*state));

	/* Before the framework builds our TupleDesc/projection or initializes
	 * child plans, validate and normalize every private binding map once. */
	state->description = GgcPgqDecodeNativePlan(scan);
	NodeSetTag(&state->css, T_CustomScanState);
	state->css.methods = &pgq_native_exec_methods;
	return (Node *) state;
}

static void
pgq_native_begin(CustomScanState *css, EState *estate, int eflags)
{
	PgqNativeState *state = (PgqNativeState *) css;
	CustomScan *plan = (CustomScan *) css->ss.ps.plan;
	int i = 0;

	GgcPgqValidateNativeParams(state->description, estate->es_plannedstmt->paramExecTypes);
	if (state->description->parameter_count && estate->es_param_exec_vals == NULL)
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
						errmsg("native graph plan has no executor parameter slots")));
	state->count = state->description->stage_count;
	state->stages = palloc0(mul_size(sizeof(*state->stages), state->count));
	foreach_ptr(Plan, child, plan->custom_plans)
	{
		PgqNativeStage *stage = &state->stages[i];
		TupleDesc descriptor;
		TupleDesc flat = css->ss.ss_ScanTupleSlot->tts_tupleDescriptor;

		stage->description = &state->description->stages[i];
		stage->child = ExecInitNode(child, estate, eflags & ~EXEC_FLAG_BACKWARD);
		css->custom_ps = lappend(css->custom_ps, stage->child);
		descriptor = ExecGetResultType(stage->child);
		if (descriptor->natts != stage->description->column_count)
			ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
							errmsg("native graph child slot width does not match its plan")));
		for (int column = 0; column < descriptor->natts; column++)
		{
			Form_pg_attribute source = TupleDescAttr(descriptor, column);
			Form_pg_attribute target = TupleDescAttr(flat, stage->description->columns[column]);

			if (source->atttypid != target->atttypid ||
				source->atttypmod != target->atttypmod ||
				source->attcollation != target->attcollation)
				ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
								errmsg("native graph child slot type does not match its plan")));
		}
		stage->binding = ExecInitExtraTupleSlot(estate,
			descriptor, &TTSOpsMinimalTuple);
		stage->qual = ExecInitQual(stage->description->quals, &css->ss.ps);
		stage->restart = true;
		i++;
	}
	/* Child initialization may register initplans.  Locally assigned slots
	 * must not also be owned by an initplan.  Check before writing any value. */
	for (i = 0; i < state->description->parameter_count; i++)
		if (estate->es_param_exec_vals[state->description->parameters[i].id].execPlan != NULL)
			ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
							errmsg("native graph parameter conflicts with an initplan")));
	ExecStoreAllNullTuple(css->ss.ss_ScanTupleSlot);
	state->depth = 0;
}

static TupleTableSlot *
pgq_native_exec(CustomScanState *css)
{
	PgqNativeState *state = (PgqNativeState *) css;
	TupleTableSlot *flat = css->ss.ss_ScanTupleSlot;
	ExprContext *context = css->ss.ps.ps_ExprContext;
	if (!state->gated)
	{
		state->gated = true;
		if (!ExecQual(css->ss.ps.qual, context))
			state->depth = -1;
	}

	while (state->depth >= 0)
	{
		PgqNativeStage *stage = &state->stages[state->depth];
		const GgcPgqNativeStage *description = stage->description;
		TupleTableSlot *tuple;

		CHECK_FOR_INTERRUPTS();
		ResetExprContext(context);
		if (stage->restart)
		{
			for (int i = 0; i < description->parameter_count; i++)
			{
				int id = description->parameters[i].id;
				int att = description->parameters[i].column;
				ParamExecData *param = &css->ss.ps.state->es_param_exec_vals[id];
				Assert(param->execPlan == NULL);
				param->value = flat->tts_values[att];
				param->isnull = flat->tts_isnull[att];
				stage->child->chgParam = bms_add_member(stage->child->chgParam, id);
			}
			ExecReScan(stage->child);
			stage->restart = false;
		}
		tuple = ExecProcNode(stage->child);
		if (TupIsNull(tuple))
		{
			ExecClearTuple(stage->binding);
			state->depth--;
			continue;
		}
		/* Native scan slots can own transient/detoasted Datums. Materialize
		 * one tuple per depth so descendants and cursor pauses stay valid. */
		ExecCopySlot(stage->binding, tuple);
		slot_getallattrs(stage->binding);
		for (int column = 0; column < description->column_count; column++)
		{
			int att = description->columns[column];

			flat->tts_values[att] = stage->binding->tts_values[column];
			flat->tts_isnull[att] = stage->binding->tts_isnull[column];
		}
		context->ecxt_scantuple = flat;
		if (!ExecQual(stage->qual, context))
			continue;
		if (state->depth == state->count-1)
			return css->ss.ps.ps_ProjInfo ? ExecProject(css->ss.ps.ps_ProjInfo) : flat;
		state->stages[++state->depth].restart = true;
	}
	return ExecClearTuple(css->ss.ps.ps_ResultTupleSlot);
}

static void
pgq_native_rescan(CustomScanState *css)
{
	PgqNativeState *state = (PgqNativeState *) css;
	for (int i = 0; i < state->count; i++)
	{
		PgqNativeStage *stage = &state->stages[i];
		UpdateChangedParamSet(stage->child, css->ss.ps.chgParam);
		ExecClearTuple(stage->binding);
		stage->restart = true;
	}
	ExecStoreAllNullTuple(css->ss.ss_ScanTupleSlot);
	state->depth = 0;
	state->gated = false;
	state->rescans++;
}

static void
pgq_native_end(CustomScanState *css)
{
	PgqNativeState *state = (PgqNativeState *) css;
	for (int i = 0; i < state->count; i++)
	{
		ExecEndNode(state->stages[i].child);
		ExecClearTuple(state->stages[i].binding);
	}
}

static void
pgq_native_explain(CustomScanState *css, List *ancestors, ExplainState *es)
{
	PgqNativeState *state = (PgqNativeState *) css;
	ExplainPropertyInteger("Graph Stages", NULL, state->description->hops, es);
	ExplainPropertyText("Graph Binding Model", "typed native access; full bag", es);
	ExplainPropertyText("Graph Access", "native child plans (index/append/view/FDW as shown)", es);
	if (es->analyze)
		ExplainPropertyUInteger("Graph Rescans", NULL, state->rescans, es);
}
