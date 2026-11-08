/* Test-only inspection of PGQ rewrite provenance before and after pull-up. */
#include "postgres.h"

#include "fmgr.h"
#include "lib/stringinfo.h"
#include "nodes/graphnodes.h"
#include "nodes/parsenodes.h"
#include "optimizer/planner.h"
#include "parser/parsetree.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "miscadmin.h"

PG_FUNCTION_INFO_V1(test_graph_pgq_provenance);
PG_FUNCTION_INFO_V1(test_graph_pgq_invalid_mapping);
PG_FUNCTION_INFO_V1(test_graph_pgq_statistics_scope);

typedef struct ProgramInspection
{
	int			programs;
	int			graph_patterns;
	int			disabled_queries;
} ProgramInspection;

static void
inspect_query(Query *query, ProgramInspection *inspection)
{
	if (query->graph_program)
		inspection->programs++;
	if (query->graph_program_disabled)
		inspection->disabled_queries++;
	foreach_node(RangeTblEntry, rte, query->rtable)
	{
		if (rte->rtekind == RTE_GRAPH_TABLE && rte->graph_pattern)
			inspection->graph_patterns++;
		else if (rte->rtekind == RTE_SUBQUERY && rte->subquery)
			inspect_query(rte->subquery, inspection);
	}
}

static GgcFixedProgram *
find_first_program(Query *query)
{
	if (query->graph_program)
		return query->graph_program;
	foreach_node(RangeTblEntry, rte, query->rtable)
	{
		GgcFixedProgram *program;

		if (rte->rtekind != RTE_SUBQUERY || !rte->subquery)
			continue;
		program = find_first_program(rte->subquery);
		if (program)
			return program;
	}
	return NULL;
}

static void
append_program(StringInfo result, Query *query)
{
	GgcFixedProgram *program = query->graph_program;
	bool		mapping_valid = program != NULL;

	appendStringInfo(result, "\"top_program\":%s,\"top_disabled\":%s,",
		program ? "true" : "false",
		query->graph_program_disabled ? "true" : "false");
	appendStringInfoString(result, "\"variables\":[");
	if (program)
	{
		int			position = 0;

		foreach_node(GgcVariable, variable, program->variables)
		{
			RangeTblEntry *rte = NULL;
			bool		mapped = variable->fallback_rti > 0 &&
				variable->fallback_rti <= list_length(query->rtable);

			if (mapped)
				rte = rt_fetch(variable->fallback_rti, query->rtable);
			mapped = mapped && rte->rtekind == RTE_RELATION &&
				rte->relid == variable->relation_oid;
			mapping_valid &= mapped;
			appendStringInfo(result,
				"%s{\"id\":%d,\"rti\":%u,\"relation\":%u,"
				"\"mapped\":%s}",
				position++ ? "," : "", variable->variable_id,
				variable->fallback_rti, variable->relation_oid,
				mapped ? "true" : "false");
		}
	}
	appendStringInfo(result, "],\"mapping_valid\":%s",
		mapping_valid ? "true" : "false");
}

static Datum
inspect_provenance(FunctionCallInfo fcinfo, bool corrupt_mapping)
{
	char	   *source = text_to_cstring(PG_GETARG_TEXT_PP(0));
	List	   *raw_list = pg_parse_query(source);
	RawStmt   *raw;
	List	   *queries;
	Query	   *query;
	ProgramInspection before = {0};
	ProgramInspection after = {0};
	StringInfoData result;

	if (list_length(raw_list) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("test query must contain exactly one statement")));
	raw = linitial_node(RawStmt, raw_list);
	queries = pg_analyze_and_rewrite_fixedparams(raw, source, NULL, 0, NULL);
	if (list_length(queries) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("test query must rewrite to exactly one statement")));
	query = linitial_node(Query, queries);
	if (query->commandType != CMD_SELECT)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("test query must be SELECT")));
	inspect_query(query, &before);
	if (corrupt_mapping)
	{
		GgcFixedProgram *program = find_first_program(query);
		GgcVariable *variable;

		if (!program || program->variables == NIL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("test query did not produce graph provenance")));
		variable = linitial_node(GgcVariable, program->variables);
		variable->fallback_rti = list_length(query->rtable) + 100;
	}
	(void) pg_plan_query(query, source, CURSOR_OPT_PARALLEL_OK, NULL, NULL);
	inspect_query(query, &after);

	initStringInfo(&result);
	appendStringInfo(&result,
		"{\"before_programs\":%d,\"before_graph_patterns\":%d,"
		"\"before_disabled_queries\":%d,\"after_programs\":%d,"
		"\"after_graph_patterns\":%d,\"after_disabled_queries\":%d,",
		before.programs, before.graph_patterns, before.disabled_queries,
		after.programs, after.graph_patterns, after.disabled_queries);
	append_program(&result, query);
	appendStringInfoChar(&result, '}');
	return PointerGetDatum(cstring_to_text(result.data));
}

Datum
test_graph_pgq_provenance(PG_FUNCTION_ARGS)
{
	return inspect_provenance(fcinfo, false);
}

Datum
test_graph_pgq_invalid_mapping(PG_FUNCTION_ARGS)
{
	return inspect_provenance(fcinfo, true);
}

/* Test-only scope inspection. Actual acceptance queries run in a new backend
 * without loading this module; this does not create or select a graph Path. */
Datum
test_graph_pgq_statistics_scope(PG_FUNCTION_ARGS)
{
	char *source = text_to_cstring(PG_GETARG_TEXT_PP(0));
	List *raw_list = pg_parse_query(source);
	List *queries;
	GgcFixedProgram *program;
	GgcFixedHop *hop;
	GgcVariable *edge = NULL;
	StringInfoData result;

	if (list_length(raw_list) != 1)
		elog(ERROR, "scope inspection requires one SELECT");
	queries = pg_analyze_and_rewrite_fixedparams(linitial_node(RawStmt, raw_list),
		source, NULL, 0, NULL);
	if (list_length(queries) != 1 || linitial_node(Query, queries)->commandType != CMD_SELECT)
		elog(ERROR, "scope inspection requires one rewritten SELECT");
	program = find_first_program(linitial_node(Query, queries));
	if (!program || program->native_access || program->hops == NIL)
		elog(ERROR, "scope inspection requires a fixed integer graph program");
	hop = linitial_node(GgcFixedHop, program->hops);
	foreach_node(GgcVariable, variable, program->variables)
		if (variable->variable_id == hop->edge_variable)
			edge = variable;
	if (!edge)
		elog(ERROR, "scope inspection found no edge variable");
	initStringInfo(&result);
	appendStringInfo(&result,
		"{\"catalog_provider_oid\":0,\"graph_oid\":%u,\"edge_element_oid\":%u,"
		"\"index_relid\":0,\"effective_role\":%u,\"endpoint_domain\":%u,"
		"\"scope_kind\":1,\"direction\":%u,\"path_mode\":1,\"result_mode\":2,"
		"\"catalog_generation\":" UINT64_FORMAT ",\"security_generation\":1,"
		"\"build_generation\":0,\"semantic_fingerprint\":" UINT64_FORMAT "}",
		program->graph_oid, edge->element_oid, GetUserId(), edge->relation_oid,
		hop->direction, Max(program->catalog_generation, UINT64CONST(1)),
		program->semantic_fingerprint & PG_INT64_MAX);
	PG_RETURN_TEXT_P(cstring_to_text(result.data));
}
