/* Bounded start/order search, included by the core PGQ integration.
 * The logical Query is never mutated. These copies are physical candidates,
 * not additional graph results or alternatives in the query's semantics. */

/* Every non-leftmost start of a chain must traverse its first logical hop
 * backwards. Reject only a necessary index shape that is already absent from
 * the native planner's index list: pgq_select_btree_index() requires that same
 * list later, even when physical cost profiles are available. This is not a
 * catalog/negative-result cache. Native index locks and validity decisions
 * remain authoritative, and all other admission checks still run normally. */
static bool
pgq_reverse_start_possible(PlannerInfo *root, GgcFixedProgram *program)
{
	GgcFixedHop *first = linitial_node(GgcFixedHop, program->hops);
	GgcVariable *edge = pgq_program_variable(program, first->edge_variable);
	RelOptInfo *rel;
	AttrNumber key;

	/* Leave unknown or undirected forms to the existing complete checks. */
	if (!edge || edge->fallback_rti <= 0 ||
		edge->fallback_rti >= root->simple_rel_array_size ||
		(first->direction != GGC_OUT && first->direction != GGC_IN))
		return true;
	rel = root->simple_rel_array[edge->fallback_rti];
	if (!rel)
		return true;
	key = first->direction == GGC_OUT ? edge->destination_attnum :
		edge->source_attnum;
	foreach_node(IndexOptInfo, index, rel->indexlist)
		if (!index->hypothetical && index->relam == BTREE_AM_OID &&
			index->nkeycolumns >= 1 && index->indexkeys[0] == key &&
			index->indpred == NIL && index->indexprs == NIL)
			return true;
	return false;
}

static GgcFixedHop *
pgq_ordered_hop(GgcFixedHop *original, bool reverse)
{
	GgcFixedHop *hop;

	/* Forward hops, predicates and constraint IDs remain read-only. A reverse
	 * hop owns only its changed scalar fields and required-variable bitmap. */
	if (!reverse)
		return original;
	hop = palloc(sizeof(*hop));
	*hop = *original;
	hop->input_variable = original->output_variable;
	hop->output_variable = original->input_variable;
	if (hop->direction != GGC_UNDIRECTED)
		hop->direction = hop->direction == GGC_OUT ? GGC_IN : GGC_OUT;
	hop->required_variables = bms_make_singleton(hop->input_variable);
	return hop;
}

static GgcFixedProgram *
pgq_ordered_program(GgcFixedProgram *original, GgcFixedHop **hops,
					int count, int start, bool left_first)
{
	GgcFixedProgram *program;

	if (start == 0)
		return original;
	/* Variables, expressions and output mappings are immutable across physical
	 * orders. Only the hop list, stage order and fingerprint belong to this
	 * candidate; deep-copying the logical metadata also copied discarded hops. */
	program = palloc(sizeof(*program));
	*program = *original;
	program->hops = NIL;
	program->stage_order = NIL;
	for (int side = 0; side < 2; side++)
	{
		bool left = side == 0 ? left_first : !left_first;

		if (left)
			for (int i = start - 1; i >= 0; i--)
				program->hops = lappend(program->hops, pgq_ordered_hop(hops[i], true));
		else
			for (int i = start; i < count; i++)
				program->hops = lappend(program->hops, pgq_ordered_hop(hops[i], false));
	}
	foreach_node(GgcFixedHop, hop, program->hops)
	{
		program->stage_order = lappend_int(program->stage_order, hop->stage_id);
		program->semantic_fingerprint ^= ((uint64) hop->input_variable << 32) |
			((uint32) hop->edge_variable << 2) | hop->direction;
		program->semantic_fingerprint *= UINT64CONST(1099511628211);
	}
	return program;
}

static void
pgq_search_orders(PlannerInfo *root, RelOptInfo *joinrel,
				  RelOptInfo *outerrel, RelOptInfo *innerrel,
				  JoinType jointype, JoinPathExtraData *extra)
{
	GgcFixedProgram *original = root->parse->graph_program;
	int count = list_length(original->hops);
	GgcFixedHop **hops = palloc(sizeof(*hops) * count);
	int *vertex_ids = palloc(sizeof(*vertex_ids) * (count + 1));
	int starts[8];
	bool left_first[8];
	int middle[3] = {-1, -1, -1};
	double middle_rows[3] = {DBL_MAX, DBL_MAX, DBL_MAX};
	int orders = 0, tried = 0, position = 0;
	int budget = Min(graph_btree_max_candidates, 1 + 8192 / Max(count, 1));
	bool limited = false;
	bool reverse_possible = true;
	List *candidates;
	instr_time started;
#ifdef USE_ASSERT_CHECKING
	GgcFixedProgram *logical_copy = copyObject(original);
#endif

	/* AGE remains on its existing qualified endpoint-set route. */
	if (enable_graph_age_adjacency)
		budget = 1;
	foreach_node(GgcFixedHop, hop, original->hops)
	{
		if (position && hop->input_variable != vertex_ids[position])
			return; /* This search does not generalize arbitrary logical graphs. */
		hops[position] = hop;
		if (position == 0)
			vertex_ids[0] = hop->input_variable;
		vertex_ids[++position] = hop->output_variable;
	}
	if (count == 0)
		return;
	if (budget > 1 && route_uses_btree() && !enable_graph_age_adjacency)
		reverse_possible = pgq_reverse_start_possible(root, original);
	starts[orders] = 0; left_first[orders++] = false;
	starts[orders] = count; left_first[orders++] = true;
	/* Only three internal starts, selected using existing base-relation rows.
	 * For each, compare both branch orders with the same complete path cost. */
	for (int i = 1; i < count; i++)
	{
		GgcVariable *v = pgq_program_variable(original, vertex_ids[i]);
		RelOptInfo *rel;
		double rows;

		if (!v || v->fallback_rti <= 0 || v->fallback_rti >= root->simple_rel_array_size)
			continue;
		rel = root->simple_rel_array[v->fallback_rti];
		if (!rel || !rel->cheapest_total_path || !bms_is_empty(rel->lateral_relids))
			continue;
		rows = rel->rows;
		for (int slot = 0; slot < 3; slot++)
			if (rows < middle_rows[slot])
			{
				for (int move = 2; move > slot; move--)
				{
					middle[move] = middle[move - 1];
					middle_rows[move] = middle_rows[move - 1];
				}
				middle[slot] = i;
				middle_rows[slot] = rows;
				break;
			}
	}
	for (int i = 0; i < 3 && middle[i] >= 0; i++)
	{
		starts[orders] = middle[i]; left_first[orders++] = true;
		starts[orders] = middle[i]; left_first[orders++] = false;
	}
	budget = Min(budget, orders);
	if (route_active == ROUTE_BTREE)
	{
		joinrel->pathlist = NIL;
		joinrel->partial_pathlist = NIL;
	}
	INSTR_TIME_SET_CURRENT(started);
	for (int i = 0; i < budget; i++)
	{
		GgcFixedProgram *program;
		instr_time elapsed;
#ifdef USE_ASSERT_CHECKING
		List *saved_paths = list_copy(joinrel->pathlist);
		CustomPath *saved_distinct = route_region(root)->distinct_path;
#endif

		CHECK_FOR_INTERRUPTS();
		INSTR_TIME_SET_CURRENT(elapsed);
		INSTR_TIME_SUBTRACT(elapsed, started);
		if (i > 0 && INSTR_TIME_GET_MILLISEC(elapsed) >= 20.0)
		{
			limited = true;
			break;
		}
		tried++;
#ifndef USE_ASSERT_CHECKING
		if (starts[i] > 0 && !reverse_possible)
			continue;
#endif
		program = pgq_ordered_program(original, hops, count, starts[i], left_first[i]);
		if (route_configure(program, &cbo_config))
			test_pgq_add_path(root, joinrel, outerrel, innerrel, jointype, extra,
				program, starts[i]);
		/* Assertion builds run the original full pipeline for rejected
		 * shapes and verify that it cannot retain a graph path either. */
#ifdef USE_ASSERT_CHECKING
		if (starts[i] > 0 && !reverse_possible)
		{
			ListCell *old_path;
			ListCell *new_path;

			/* add_path() can change a List in place. Compare the saved
			 * member pointers, not only the List allocation address. */
			Assert(list_length(joinrel->pathlist) == list_length(saved_paths));
			forboth(old_path, saved_paths, new_path, joinrel->pathlist)
				Assert(lfirst(old_path) == lfirst(new_path));
			Assert(route_region(root)->distinct_path == saved_distinct);
		}
		list_free(saved_paths);
#endif
	}
	candidates = list_copy(joinrel->pathlist);
	if (route_auto_active && route_region(root)->distinct_path)
		candidates = lappend(candidates, route_region(root)->distinct_path);
	foreach_ptr(Path, candidate, candidates)
		if (IsA(candidate, CustomPath) &&
			castNode(CustomPath, candidate)->methods == &pgq_fixed_path_methods)
		{
			List *metadata = lthird_node(List, castNode(CustomPath, candidate)->custom_private);

			lfirst(list_nth_cell(metadata, 20)) = makeInteger(tried);
			lfirst(list_nth_cell(metadata, 21)) = makeInteger(budget);
			lfirst(list_nth_cell(metadata, 22)) = makeInteger(limited || budget < orders);
		}
	Assert(equal(original, logical_copy));
}
