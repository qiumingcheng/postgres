/* Statement-scoped GraphRoute hints. Included by graph_pgq.c so this
 * entry uses the very same admission, costing, plan and runtime as PGQ CBO.
 * No request-table substitution and no modification of cardinalities/costs.
 */
#include <ctype.h>
#include "catalog/pg_am_d.h"
#include "catalog/pg_opfamily_d.h"
#include "catalog/pg_propgraph_element.h"
#include "commands/defrem.h"
#include "optimizer/planner.h"
#include "parser/scansup.h"
#include "utils/guc.h"

enum { ROUTE_AUTO, ROUTE_RELATIONAL, ROUTE_BTREE, ROUTE_ADJ };
static int route_active;
static Oid route_index;
static Oid route_adj_am;
static bool route_preserve_regions;
static bool route_auto_active;
static bool route_candidate_mode;

/* Each PlannerInfo owns its admission/cost scope.  The legacy global CBO
 * configuration is used only while constructing a candidate, then restored. */
typedef struct RouteRegionConfig
{
	PlannerInfo *root;
	TestCboConfig config;
	bool eligible;
	bool considered;
	/* A globally unique alternative has a property add_path cannot compare
	 * at the bag-valued join boundary. Keep at most one until DISTINCT. */
	CustomPath *distinct_path;
} RouteRegionConfig;
static List *route_regions;
static List *route_cardinalities;
static planner_hook_type previous_route_planner;
static GgcGraphIndexAllowedHook previous_route_index_hook;
static explain_per_plan_hook_type previous_route_explain;
static RelOptInfo *route_graph_join_search(PlannerInfo *root,
										  int levels_needed, List *initial_rels);

static const char *
route_name(int mode)
{
	return mode == ROUTE_RELATIONAL ? "relational" :
		mode == ROUTE_BTREE ? "btree" : mode == ROUTE_ADJ ? "adj" : "auto";
}

static bool
route_uses_btree(void)
{
	return route_active == ROUTE_BTREE || route_auto_active;
}

static void
route_error(const char *detail)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("cannot satisfy GraphRoute(%s)", route_name(route_active)),
			 errdetail_internal("%s", detail),
			 errhint("Remove the hint to allow the ordinary planner; a forced route never silently falls back.")));
}

static int
route_comment(const char *body, int length)
{
	char *text = pnstrdup(body, length);
	char *p = text;
	char *word;
	int mode;

	while (isspace((unsigned char) *p)) p++;
	if (pg_strncasecmp(p, "GraphRoute", 10) != 0)
		return ROUTE_AUTO;
	p += 10;
	while (isspace((unsigned char) *p)) p++;
	if (*p++ != '(') goto syntax_error;
	while (isspace((unsigned char) *p)) p++;
	word = p;
	while (isalpha((unsigned char) *p)) p++;
	if (p - word == 10 && pg_strncasecmp(word, "relational", 10) == 0) mode = ROUTE_RELATIONAL;
	else if (p - word == 8 && pg_strncasecmp(word, "nestloop", 8) == 0)
		ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
						errmsg("GraphRoute(nestloop) has been replaced by GraphRoute(relational)"),
						errhint("Use /*+ GraphRoute(relational) */ for ordinary relational planning; join methods follow the session settings.")));
	else if (p - word == 5 && pg_strncasecmp(word, "btree", 5) == 0) mode = ROUTE_BTREE;
	else if (p - word == 3 && pg_strncasecmp(word, "adj", 3) == 0) mode = ROUTE_ADJ;
	else goto syntax_error;
	while (isspace((unsigned char) *p)) p++;
	if (*p++ != ')') goto syntax_error;
	while (isspace((unsigned char) *p)) p++;
	if (*p != '\0') goto syntax_error;
	return mode;
syntax_error:
	ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
					errmsg("invalid GraphRoute hint"),
					errhint("Use exactly /*+ GraphRoute(relational|btree|adj) */ with one mode.")));
	return ROUTE_AUTO;
}

/* Inspect real block comments only. String/dollar literals, quoted names,
 * nested ordinary comments and other statements cannot activate this hint. */
static int
route_parse_hint(Query *query, const char *source)
{
	int mode = ROUTE_AUTO;
	int start;
	int length;
	char *sql;
	List *comments = NIL;

	/* A hint must contain this exact opener.  Reject the common no-hint
	 * case before copying/lexing ordinary SQL, including large DML arrays.
	 * A match is only a possibility: retain the lexer below for quoting,
	 * nested comments and statement boundaries (including prefix hints). */
	if (!source || strstr(source, "/*+") == NULL) return mode;
	start = Max(query->stmt_location, 0);
	if (start > strlen(source)) return mode;
	/* RawStmt starts at the first SQL token, not at a preceding hint.
	 * Lex the prefix as well, retaining comments only from this statement.
	 * Defer interpretation until the boundary is known: a malformed hint in
	 * an earlier statement must not reject the current statement. */
	length = strlen(source);
	if (query->stmt_len > 0) length = Min(length, start + query->stmt_len);
	sql = pnstrdup(source, length);
	for (int i = 0; i < length;)
	{
		if (sql[i] == ';')
		{
			if (i >= start) break;
			comments = NIL;
			i++;
			continue;
		}
		if (sql[i] == '-' && sql[i + 1] == '-')
		{
			while (i < length && sql[i] != '\n') i++;
			continue;
		}
		if (sql[i] == '\'' || sql[i] == '"')
		{
			char quote = sql[i++];
			/* PG19 rejects standard_conforming_strings=off. */
			bool escapes = quote == '\'' &&
				(i >= 2 && (sql[i - 2] == 'e' || sql[i - 2] == 'E'));
			while (i < length)
			{
				if (escapes && sql[i] == '\\') { i += Min(2, length - i); continue; }
				if (sql[i++] == quote)
				{
					if (sql[i] == quote) i++;
					else break;
				}
			}
			continue;
		}
		/* Match PG identifier bytes, including embedded '$' and UTF-8. */
		if (isalpha((unsigned char) sql[i]) || sql[i] == '_' ||
			(unsigned char) sql[i] >= 128)
		{
			do { i++; }
			while (isalnum((unsigned char) sql[i]) || sql[i] == '_' ||
				   sql[i] == '$' || (unsigned char) sql[i] >= 128);
			continue;
		}
		if (sql[i] == '$')
		{
			int end = i + 1;
			if (sql[end] == '$' || isalpha((unsigned char) sql[end]) ||
				sql[end] == '_' || (unsigned char) sql[end] >= 128)
			{
				while (isalnum((unsigned char) sql[end]) || sql[end] == '_' ||
					   (unsigned char) sql[end] >= 128) end++;
				if (sql[end] == '$')
				{
					char *tag = pnstrdup(sql + i, end - i + 1);
					char *close = strstr(sql + end + 1, tag);
					i = close ? close - sql + strlen(tag) : length;
					continue;
				}
			}
		}
		if (sql[i] == '/' && sql[i + 1] == '*')
		{
			int begin = i;
			int depth = 1;
			i += 2;
			while (i < length && depth)
			{
				if (sql[i] == '/' && sql[i + 1] == '*') { depth++; i += 2; }
				else if (sql[i] == '*' && sql[i + 1] == '/') { depth--; i += 2; }
				else i++;
			}
			if (depth == 0 && sql[begin + 2] == '+')
				comments = lappend(comments,
					makeString(pnstrdup(sql + begin + 3, i - begin - 5)));
			continue;
		}
		i++;
	}
	foreach_node(String, comment, comments)
	{
		int found = route_comment(strVal(comment), strlen(strVal(comment)));
		if (found && mode)
			ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR), errmsg("duplicate GraphRoute hints")));
		if (found) mode = found;
	}
	return mode;
}

typedef struct RouteProgramSearch
{
	GgcFixedProgram *program;
	int count;
	int unsupported;
	bool has_outer_join_region;
} RouteProgramSearch;

static bool
route_find_program(Node *node, RouteProgramSearch *search)
{
	if (!node) return false;
	if (IsA(node, Query))
	{
		Query *query = castNode(Query, node);
		if (query->graph_program_disabled)
			search->unsupported++;
		if (query->graph_program)
		{
			search->program = query->graph_program;
			search->count++;
			if (query->graph_program->native_access)
				search->has_outer_join_region = true;
		}
		else if (query->jointree && query->jointree->fromlist != NIL &&
				 (list_length(query->jointree->fromlist) != 1 ||
				  !IsA(linitial(query->jointree->fromlist), RangeTblRef)))
			search->has_outer_join_region = true;
		return query_tree_walker(query, route_find_program, search, 0);
	}
	return expression_tree_walker(node, route_find_program, search);
}

/* Scope the boundary to explicit B-tree composition.  Pure graph queries
 * still pull up normally, including their existing endpoint-set proofs. */
static bool
route_keep_subquery_boundary(PlannerInfo *root, Query *subquery)
{
	return route_uses_btree() &&
		(route_preserve_regions || root->parse->graph_program != NULL ||
		 (subquery->graph_program && subquery->graph_program->native_access));
}

static bool
route_keep_variable_bindings(PlannerInfo *root)
{
	return route_active == ROUTE_BTREE;
}

static bool
route_index_allowed(Oid relation, Oid index, Oid am)
{
	if (previous_route_index_hook && !previous_route_index_hook(relation, index, am))
		return false;
	return (route_active == ROUTE_AUTO && !route_auto_active) || am != route_adj_am;
}

static bool
route_edge_index_eligible(Relation index, AttrNumber src, AttrNumber dst)
{
	Form_pg_index form = index->rd_index;
	bool eligible = form->indisvalid && form->indisready && form->indislive &&
		!form->indcheckxmin && RelationGetIndexPredicate(index) == NIL &&
		RelationGetIndexExpressions(index) == NIL && form->indnkeyatts >= 1 &&
		form->indkey.values[0] == src;

	if (route_uses_btree())
		eligible = eligible && index->rd_rel->relam == BTREE_AM_OID &&
			get_rel_relam(form->indrelid) == HEAP_TABLE_AM_OID &&
			index->rd_opfamily[0] == INTEGER_BTREE_FAM_OID &&
			pgq_key_type_supported(index->rd_opcintype[0]);
	else
		eligible = eligible && OidIsValid(route_adj_am) &&
			index->rd_rel->relam == route_adj_am && form->indnkeyatts == 2 &&
			form->indkey.values[1] == dst;
	/* Common-only and C+A builds must not offer an unavailable B provider. */
	return eligible && GgcGetIndexAdjProvider(index) != NULL;
}

/* Admission and physical-statistics identity only.  A B-tree execution plan
 * chooses among admitted indexes with cost_index once PlannerInfo and the
 * actual stage population are available (pgq_select_btree_index). */
static Oid
route_choose_index(Oid edge, AttrNumber src, AttrNumber dst)
{
	Relation table = table_open(edge, AccessShareLock);
	List *indexes = RelationGetIndexList(table);
	Oid chosen = InvalidOid;

	foreach_oid(oid, indexes)
	{
		Relation index = index_open(oid, AccessShareLock);
		if (route_edge_index_eligible(index, src, dst) &&
			(!OidIsValid(chosen) || oid < chosen)) chosen = oid;
		index_close(index, NoLock);
	}
	list_free(indexes);
	table_close(table, NoLock);
	if (!OidIsValid(chosen) && !route_auto_active && !route_candidate_mode)
		route_error("No valid, non-partial index of the requested kind matches the edge endpoints.");
	return chosen;
}

static bool
route_unsupported(const char *detail)
{
	if (!route_auto_active && !route_candidate_mode)
		route_error(detail);
	return false;
}

static bool
route_configure(GgcFixedProgram *program, TestCboConfig *config)
{
	Oid vertex = InvalidOid;
	Oid edge = InvalidOid;
	Oid element = InvalidOid;
	AttrNumber src = 0, dst = 0;
	uint64 generation = 1;
	HeapTuple tuple;
	Oid indexoid;
	bool multiple_elements = false;
	bool per_stage = route_uses_btree() && !enable_graph_age_adjacency;
	GgcFixedHop *first;
	GgcVariable *first_edge, *first_vertex;

	foreach_node(GgcVariable, variable, program->variables)
	{
		if (variable->entity_kind == GGC_VERTEX)
		{
			if (!per_stage && OidIsValid(vertex) && vertex != variable->relation_oid)
				return route_unsupported("The current graph executor requires one homogeneous vertex table.");
			vertex = variable->relation_oid;
		}
		else
		{
			if (!per_stage && OidIsValid(edge) && (edge != variable->relation_oid ||
				src != variable->source_attnum || dst != variable->destination_attnum))
				return route_unsupported("The current graph executor requires one homogeneous edge mapping.");
			edge = variable->relation_oid;
			src = variable->source_attnum;
			dst = variable->destination_attnum;
		}
	}
	if (!OidIsValid(edge) || !OidIsValid(vertex) || !program->hops)
		return route_unsupported("No fixed graph path is available.");
	first = linitial_node(GgcFixedHop, program->hops);
	first_edge = pgq_program_variable(program, first->edge_variable);
	first_vertex = pgq_program_variable(program, first->input_variable);
	if (!first_edge || !first_vertex)
		return false;
	edge = first_edge->relation_oid;
	vertex = first_vertex->relation_oid;
	src = first_edge->source_attnum;
	dst = first_edge->destination_attnum;
	if (route_uses_btree() &&
		linitial_node(GgcFixedHop, program->hops)->direction == GGC_IN)
		indexoid = route_choose_index(edge, dst, src);
	else
		indexoid = route_choose_index(edge, src, dst);
	if (!OidIsValid(indexoid))
		return false;
	/* Natural planning must reject absent later/reverse indexes before Plan
	 * serialization.  It must not ERROR after retaining a candidate. */
	if (route_uses_btree())
	{
		Oid checked_edge = edge;
		AttrNumber checked_src = src, checked_dst = dst;
		bool checked_out = first->direction != GGC_IN;
		bool checked_in = first->direction == GGC_IN;

		foreach_node(GgcFixedHop, hop, program->hops)
		{
			GgcVariable *mapped = pgq_program_variable(program, hop->edge_variable);

			if (!mapped)
				return false;
			/* The discovery above retains table/index DDL locks. Adjacent
			 * aliases of this physical mapping need only one check per
			 * direction; each stage still gets its own native cost and quals. */
			if (mapped->relation_oid != checked_edge ||
				mapped->source_attnum != checked_src ||
				mapped->destination_attnum != checked_dst)
			{
				checked_edge = mapped->relation_oid;
				checked_src = mapped->source_attnum;
				checked_dst = mapped->destination_attnum;
				checked_out = checked_in = false;
			}
			if (hop->direction != GGC_IN && !checked_out)
			{
				if (!OidIsValid(route_choose_index(checked_edge, checked_src, checked_dst)))
					return false;
				checked_out = true;
			}
			if (hop->direction != GGC_OUT && !checked_in)
			{
				if (!OidIsValid(route_choose_index(checked_edge, checked_dst, checked_src)))
					return false;
				checked_in = true;
			}
		}
	}
	if (route_active == ROUTE_ADJ)
	{
		Relation index = index_open(indexoid, AccessShareLock);
		bool available = GgcGetIndexBuildGeneration(index, &generation);

		index_close(index, AccessShareLock);
		if (!available)
			return route_unsupported("The index provider does not expose its build generation.");
	}
	/* Fixed B-tree programs retain the exact graph-element identity from
	 * rewrite. Use the native OID cache instead of scanning every element.
	 * Homogeneous legacy/AGE admission still checks alias ambiguity below. */
	if (per_stage)
	{
		tuple = SearchSysCache1(PROPGRAPHELOID,
			ObjectIdGetDatum(first_edge->element_oid));
		if (HeapTupleIsValid(tuple))
		{
			Form_pg_propgraph_element form = (Form_pg_propgraph_element) GETSTRUCT(tuple);

			if (form->pgepgid == program->graph_oid &&
				form->pgekind == PGEKIND_EDGE && form->pgerelid == edge)
				element = form->oid;
			ReleaseSysCache(tuple);
		}
	}
	else
	{
		Relation catalog = table_open(PropgraphElementRelationId, AccessShareLock);
		SysScanDesc scan;
		ScanKeyData key;

		ScanKeyInit(&key, Anum_pg_propgraph_element_pgepgid, BTEqualStrategyNumber,
					F_OIDEQ, ObjectIdGetDatum(program->graph_oid));
		scan = systable_beginscan(catalog, InvalidOid, false, NULL, 1, &key);
		while (HeapTupleIsValid(tuple = systable_getnext(scan)))
		{
			Form_pg_propgraph_element form = (Form_pg_propgraph_element) GETSTRUCT(tuple);

			if (form->pgekind == PGEKIND_EDGE && form->pgerelid == edge)
			{
				multiple_elements |= OidIsValid(element);
				element = form->oid;
			}
		}
		systable_endscan(scan);
		table_close(catalog, AccessShareLock);
	}
	if (multiple_elements)
		return route_unsupported("Multiple edge aliases for this table are not yet supported by GraphRoute.");
	if (!OidIsValid(element))
		return route_unsupported("Could not resolve the graph edge element.");
	MemSet(config, 0, sizeof(*config));
	config->enabled = true;
	config->request_relid = vertex;
	config->edge_relid = edge;
	config->index_relid = indexoid;
	config->stage_count = list_length(program->hops);
	config->logical_scope = (GgcStatsScope) {
		.graph_oid = program->graph_oid, .edge_element_oid = element,
		.effective_role = GetUserId(), .endpoint_domain = edge,
		.scope_kind = GGC_STATS_LOGICAL,
		.direction = linitial_node(GgcFixedHop, program->hops)->direction == GGC_IN ? GGC_IN : GGC_OUT,
		.path_mode = GGC_WALK, .result_mode = 1,
		.catalog_generation = Max(program->catalog_generation, 1),
		.security_generation = 1,
		/* Statistics serialize hashes as nonnegative SQL bigint. */
		.semantic_fingerprint = program->semantic_fingerprint & PG_INT64_MAX
	};
	config->physical_scope = config->logical_scope;
	config->physical_scope.scope_kind = GGC_STATS_PHYSICAL;
	config->physical_scope.index_relid = indexoid;
	config->physical_scope.build_generation = generation;
	config->cost_scope = (GgcStatsScope) {
		.effective_role = GetUserId(), .scope_kind = GGC_STATS_COST,
		.semantic_fingerprint = GGC_GRAPH_COST_PROFILE_VERSION
	};
	return true;
}

static RouteRegionConfig *
route_region(PlannerInfo *root)
{
	RouteRegionConfig *region;

	foreach_ptr(RouteRegionConfig, entry, route_regions)
		if (entry->root == root)
			return entry;
	region = palloc0(sizeof(*region));
	region->root = root;
	route_regions = lappend(route_regions, region);
	if (root->parse->graph_program && !root->parse->graph_program_disabled)
		region->eligible = route_uses_btree() ||
			route_configure(root->parse->graph_program, &region->config);
	return region;
}

/* Inspect every child, including containers used by ordinary outer plans.
 * PlannedStmt.subplans are checked separately by route_planner. */
static int
route_check_plan(Plan *plan)
{
	int graphs = 0;
	Oid indexid = InvalidOid;

	if (!plan) return 0;
	if (IsA(plan, IndexScan)) indexid = castNode(IndexScan, plan)->indexid;
	if (IsA(plan, IndexOnlyScan)) indexid = castNode(IndexOnlyScan, plan)->indexid;
	if (IsA(plan, BitmapIndexScan)) indexid = castNode(BitmapIndexScan, plan)->indexid;
	if (OidIsValid(indexid))
	{
		Relation index = index_open(indexid, AccessShareLock);
		if (index->rd_rel->relam == route_adj_am) route_error("An ordinary graph_adj index scan escaped the route filter.");
		index_close(index, NoLock);
	}
	if (IsA(plan, CustomScan))
	{
		CustomScan *custom = castNode(CustomScan, plan);
		if (custom->methods == &pgq_native_scan_methods)
		{
			if (route_active != ROUTE_BTREE)
				route_error("A typed graph node escaped the B-tree route.");
			graphs++;
		}
		if (custom->methods == &pgq_fixed_scan_methods)
		{
			MemoryContext context;
			MemoryContext previous;
			GgcPgqPlan *decoded;

			if (route_active == ROUTE_RELATIONAL)
				route_error("A graph node escaped the relational route.");
			/* This is a post-Plan invariant check, not a candidate Path reader.
			 * Use the same versioned boundary as ExecutorStart.  Its temporary
			 * descriptor must not remain in a saved plan's memory context. */
			context = AllocSetContextCreate(CurrentMemoryContext,
				"graph route plan validation", ALLOCSET_SMALL_SIZES);
			previous = MemoryContextSwitchTo(context);
			decoded = GgcPgqDecodeFixedPlan(custom);
			MemoryContextSwitchTo(previous);
			for (uint32 stage = 0; stage < decoded->stage_count; stage++)
			{
				Oid indexes[2] = {decoded->stages[stage].primary.index_relid,
					decoded->reverse_access[stage].index_relid};

				for (int i = 0; i < 2; i++)
				{
					Relation index;
					Oid wanted = route_active == ROUTE_BTREE ? BTREE_AM_OID : route_adj_am;

					if (!OidIsValid(indexes[i])) continue;
					index = index_open(indexes[i], AccessShareLock);
					if (index->rd_rel->relam != wanted)
						route_error("A graph stage selected a different access family.");
					index_close(index, NoLock);
				}
			}
			route_index = decoded->stages[0].primary.index_relid;
			MemoryContextDelete(context);
			graphs++;
		}
		foreach_ptr(Plan, child, custom->custom_plans) graphs += route_check_plan(child);
	}
	if (IsA(plan, SubqueryScan)) graphs += route_check_plan(castNode(SubqueryScan, plan)->subplan);
	if (IsA(plan, BitmapAnd))
		foreach_ptr(Plan, child, castNode(BitmapAnd, plan)->bitmapplans) graphs += route_check_plan(child);
	if (IsA(plan, BitmapOr))
		foreach_ptr(Plan, child, castNode(BitmapOr, plan)->bitmapplans) graphs += route_check_plan(child);
	if (IsA(plan, Append))
		foreach_ptr(Plan, child, castNode(Append, plan)->appendplans) graphs += route_check_plan(child);
	if (IsA(plan, MergeAppend))
		foreach_ptr(Plan, child, castNode(MergeAppend, plan)->mergeplans) graphs += route_check_plan(child);
	return graphs + route_check_plan(plan->lefttree) + route_check_plan(plan->righttree);
}

static PlannedStmt *
route_planner(Query *parse, const char *source, int options,
			  ParamListInfo params, ExplainState *es)
{
	int mode = route_parse_hint(parse, source);
	int saved_mode = route_active;
	Oid saved_index = route_index, saved_am = route_adj_am;
	TestCboConfig saved_cbo = cbo_config;
	bool saved_fixed = test_config.enabled;
	bool saved_preserve_regions = route_preserve_regions;
	bool saved_auto = route_auto_active;
	bool saved_candidate_mode = route_candidate_mode;
	List *saved_regions = route_regions;
	List *saved_cardinalities = route_cardinalities;
	join_search_hook_type saved_join_search = join_search_hook;
	GgcGraphSubqueryBoundaryHook saved_boundary = ggc_graph_subquery_boundary_hook;
	GgcGraphKeepBindingsHook saved_bindings = ggc_graph_keep_bindings_hook;
	PlannedStmt *result = NULL;
	RouteProgramSearch search = {0};

	PG_TRY();
	{
		route_active = mode;
		route_index = InvalidOid;
		route_preserve_regions = false;
		route_regions = NIL;
		route_cardinalities = NIL;
		route_auto_active = false;
		route_candidate_mode = false;
		if (mode != ROUTE_RELATIONAL && (mode ||
			(enable_graph_btree && parse->commandType == CMD_SELECT &&
			 !parse->hasModifyingCTE)))
			route_find_program((Node *) parse, &search);
		/* Do not take over an unrelated optimizer or a modifying statement.
		 * Explicit graph hints reject these, automatic mode simply opts out. */
		if (mode == ROUTE_AUTO && enable_graph_btree && search.count > 0 &&
			!search.has_outer_join_region &&
			parse->commandType == CMD_SELECT && !parse->hasModifyingCTE &&
			(!saved_join_search || saved_join_search == route_graph_join_search) &&
			(!saved_boundary || saved_boundary == route_keep_subquery_boundary))
			route_auto_active = true;
		/* Preserving a graph boundary can prevent a highly selective outer
		 * join from pushing restrictions into the graph. Until full native
		 * and boundary-preserving plans can be compared, automatic mode must
		 * leave these joins entirely native. Explicit B-tree composition is
		 * still available; a constrained relational candidate is not an
		 * adequate substitute for the original unrestricted native plan. */
		if (mode || route_auto_active)
		{
			if (parse->commandType != CMD_SELECT || parse->hasModifyingCTE)
				route_error("Only read-only SELECT graph queries are supported.");
			route_adj_am = get_am_oid("graph_adj", true);
			test_config.enabled = false;
			cbo_config.enabled = false;
			/* A route selects an execution family, not a join algorithm.
			 * The relational route excludes graph paths/indexes, but leaves
			 * join methods, GEQO, Memoize and all other caller settings alone. */
			if (mode == ROUTE_BTREE || mode == ROUTE_ADJ)
			{
				if (search.count == 0 || search.unsupported ||
					(mode == ROUTE_ADJ && search.count != 1))
					route_error("Every forced graph region must be supported; adj still requires exactly one region.");
				/* A forced graph route must not pay for ordinary JOIN search.
				 * Do not silently bypass another optimizer's search contract. */
				if (saved_join_search && saved_join_search != route_graph_join_search)
					route_error("A different join search hook is already installed.");
				join_search_hook = route_graph_join_search;
				if (mode == ROUTE_BTREE)
				{
					if (saved_bindings && saved_bindings != route_keep_variable_bindings)
						route_error("A different graph variable binding hook is already installed.");
					ggc_graph_keep_bindings_hook = route_keep_variable_bindings;
				}
			}
			if (route_uses_btree() && (mode == ROUTE_BTREE || search.count > 1 || search.has_outer_join_region))
			{
				if (saved_boundary && saved_boundary != route_keep_subquery_boundary)
					route_error("A different graph subquery boundary hook is already installed.");
				route_preserve_regions = search.count > 1 || search.has_outer_join_region;
				ggc_graph_subquery_boundary_hook = route_keep_subquery_boundary;
			}
		}
		else if (saved_mode || saved_auto)
			cbo_config.enabled = false;
		result = previous_route_planner ? previous_route_planner(parse, source, options, params, es) :
			standard_planner(parse, source, options, params, es);
		if (mode)
		{
			int graphs = route_check_plan(result->planTree);
			foreach_ptr(Plan, subplan, result->subplans) graphs += route_check_plan(subplan);
			if ((mode == ROUTE_RELATIONAL && graphs != 0) ||
				(mode != ROUTE_RELATIONAL && graphs != search.count))
				route_error("The query did not pass graph admission or the required graph plan was not selected.");
			result->graph_route_hint = mode;
			result->graph_route_index = graphs == 1 ? route_index : InvalidOid;
		}
	}
	PG_FINALLY();
	{
		route_active = saved_mode;
		route_index = saved_index;
		route_adj_am = saved_am;
		cbo_config = saved_cbo;
		test_config.enabled = saved_fixed;
		route_preserve_regions = saved_preserve_regions;
		route_auto_active = saved_auto;
		route_candidate_mode = saved_candidate_mode;
		route_regions = saved_regions;
		route_cardinalities = saved_cardinalities;
		join_search_hook = saved_join_search;
		ggc_graph_subquery_boundary_hook = saved_boundary;
		ggc_graph_keep_bindings_hook = saved_bindings;
	}
	PG_END_TRY();
	return result;
}

static void
route_explain(PlannedStmt *stmt, IntoClause *into, ExplainState *es,
			  const char *source, ParamListInfo params, QueryEnvironment *env)
{
	if (previous_route_explain) previous_route_explain(stmt, into, es, source, params, env);
	if (!stmt->graph_route_hint) return;
	ExplainPropertyText("Graph Route", route_name(stmt->graph_route_hint), es);
	ExplainPropertyBool("Graph Route Forced", true, es);
	ExplainPropertyText("Graph Provider", stmt->graph_route_hint == ROUTE_RELATIONAL ?
		"relational" : stmt->graph_route_hint == ROUTE_BTREE ? "btree" : "graph_adj", es);
	if (OidIsValid(stmt->graph_route_index))
		ExplainPropertyText("Graph Index", get_rel_name(stmt->graph_route_index), es);
}
