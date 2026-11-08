/* Versioned metadata tests need no installed graph access provider. */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "catalog/pg_collation_d.h"
#include "executor/graph_pgq_plan.h"
#include "fmgr.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/readfuncs.h"
#include "optimizer/planmain.h"
#include "utils/memutils.h"
#include "utils/ruleutils.h"

PG_FUNCTION_INFO_V1(test_graph_plan_metadata);
PG_FUNCTION_INFO_V1(test_graph_custom_parameters);
PG_FUNCTION_INFO_V1(test_graph_native_metadata);

/* Synthetic OIDs are intentional: decoding must not open a relation, resolve
 * a provider, or turn a provider ABI rejection into a plan-format rejection. */
static CustomScan *
plan_fixture(bool join, int stage_fields, int option_fields)
{
	CustomScan *scan = makeNode(CustomScan);
	List *provider = list_make5(makeInteger(sizeof(GgcProviderRef)),
		makeInteger(GGC_ADJ_ABI_VERSION), makeInteger(0), makeInteger(0),
		makeString(pstrdup("test-only")));
	List *stage = list_make5(makeInteger(1), makeInteger(GGC_OUT),
		provider, makeInteger(2), makeString(pstrdup("0")));
	List *options = list_make5(makeInteger(0), makeInteger(8),
		makeInteger(8), makeInteger(100000), makeInteger(0));
	List *payload;

	stage = lappend(stage, makeInteger(false));
	stage = lappend(stage, copyObject(provider));
	stage = lappend(stage, makeInteger(0));
	stage = lappend(stage, makeString(pstrdup("0")));
	stage = lappend(stage, makeInteger(3));
	stage = lappend(stage, makeString(pstrdup("0")));
	stage = lappend(stage, makeString(pstrdup("0")));
	stage = lappend(stage, makeInteger(4));
	if (stage_fields >= 15)
		stage = list_concat(stage, list_make2(makeInteger(5), makeInteger(6)));
	if (stage_fields >= 16)
		stage = lappend(stage, makeInteger(0));
	if (stage_fields == 21)
		stage = list_concat(stage, list_make5(makeInteger(1), makeInteger(2),
			makeInteger(3), makeInteger(4), makeInteger(1)));
	options = list_concat(options, list_make3(makeInteger(8), makeInteger(16), makeInteger(4)));
	options = list_concat(options, list_make3(makeString(pstrdup("1024")),
		makeInteger(true), makeInteger(true)));
	if (option_fields >= 12)
		options = lappend(options, makeString(pstrdup("65536")));
	if (option_fields == 13)
		options = lappend(options, makeInteger(false));
	payload = list_make2(list_make2(stage, makeInteger(0)), options);
	if (join)
	{
		List *meta = list_make5(makeInteger(1), makeInteger(0), makeInteger(0),
			makeInteger(0), makeInteger(true));
		List *mapping = list_make5(makeInteger(GGC_PGQ_OUTPUT_VERTEX),
			makeInteger(2), makeInteger(FLOAT8PASSBYVAL), makeInteger(8), makeInteger(1));

		meta = lappend(meta, makeInteger(false));
		for (int i = 0; i < 3; i++)
			meta = lappend(meta, makeString(pstrdup("0")));
		meta = list_concat(meta, list_make5(makeInteger(1), makeInteger(5),
			makeInteger(4), makeInteger(0), makeInteger(1)));
		meta = lappend(meta, list_make2(makeInteger(2), makeInteger(3)));
		meta = lappend(meta, list_make3(makeInteger(1), makeInteger(4), makeInteger(5)));
		meta = lappend(meta, makeInteger(1));
		meta = lappend(meta, list_make2(makeInteger(0), makeInteger(1)));
		meta = lappend(meta, list_make2(makeInteger(-1), makeInteger(-1)));
		meta = list_concat(meta, list_make4(makeInteger(0), makeInteger(1),
			makeInteger(64), makeInteger(false)));
		payload = lappend(payload, meta);
		payload = lappend(payload, list_make1(mapping));
	}
	scan->custom_private = GgcPgqEncodeFixedPlan(payload);
	scan->custom_plans = list_make1(makeNode(Result));
	scan->custom_scan_tlist = list_make1(makeTargetEntry((Expr *)
		makeVar(1, 1, INT8OID, -1, InvalidOid, 0), 1, NULL, false));
	if (!join)
	{
		const Oid arrays[] = {INT8ARRAYOID, INT8ARRAYOID, TEXTARRAYOID};

		for (int i = 0; i < (int) lengthof(arrays); i++)
			scan->custom_scan_tlist = lappend(scan->custom_scan_tlist,
				makeTargetEntry((Expr *) makeVar(1, i + 2, arrays[i], -1,
					InvalidOid, 0), i + 2, NULL, false));
	}
	return scan;
}

static List *
fixture_payload(CustomScan *scan)
{
	return lthird_node(List, scan->custom_private);
}

/* Deliberately not 2*hops+1 bindings; repeated variables can reuse stages.
 * The large PARAM_EXEC number must survive the complete Node codec. */
static CustomScan *
native_fixture(void)
{
	CustomScan *scan = makeNode(CustomScan);
	Result *anchor = makeNode(Result);
	Result *child = makeNode(Result);
	Var *key = makeVar(1, 1, INT4OID, -1, InvalidOid, 0);
	Var *value = makeVar(2, 1, TEXTOID, -1, DEFAULT_COLLATION_OID, 0);

	scan->methods = GetCustomScanMethods("GraphExpandNative", false);
	anchor->plan.targetlist = list_make1(makeTargetEntry((Expr *) copyObject(key), 1, NULL, false));
	child->plan.targetlist = list_make1(makeTargetEntry((Expr *) copyObject(value), 1, NULL, false));
	scan->custom_plans = list_make2(anchor, child);
	scan->custom_scan_tlist = list_make2(makeTargetEntry((Expr *) key, 1, NULL, false),
		makeTargetEntry((Expr *) value, 2, NULL, false));
	scan->scan.plan.targetlist = list_make1(makeTargetEntry((Expr *)
		makeVar(INDEX_VAR, 2, TEXTOID, -1, DEFAULT_COLLATION_OID, 0), 1, NULL, false));
	scan->custom_params = bms_make_singleton(70000);
	scan->custom_param_ids = list_make1_int(70000);
	scan->custom_param_exprs = list_make1(makeVar(INDEX_VAR, 1, INT4OID, -1, InvalidOid, 0));
	scan->custom_exprs = list_make2(NIL, NIL);
	scan->custom_private = GgcPgqEncodeNativePlan(
		list_make2(list_make1_int(1), list_make1_int(2)),
		list_make2(NIL, list_make1(list_make2_int(70000, 1))), 7);
	return scan;
}

Datum
test_graph_native_metadata(PG_FUNCTION_ARGS)
{
	List *types = NIL;
	CustomScan *scan = native_fixture();
	GgcPgqNativePlan *decoded;

	for (int i = 0; i <= 70000; i++)
		types = lappend_oid(types, i == 70000 ? INT4OID : InvalidOid);
	/* Exercise the whole Plan, not just the private List.  This covers the
	 * public source fields and ordinary child Plans as well as our envelope. */
	scan = stringToNode(nodeToString(copyObject(scan)));
	decoded = GgcPgqDecodeNativePlan(scan);
	if (decoded->hops != 7 || decoded->stage_count != 2 ||
		decoded->column_count != 2 || decoded->parameters[0].id != 70000 ||
		decoded->stages[1].parameters[0].column != 0)
		elog(ERROR, "native graph metadata failed Node round-trip");
	GgcPgqValidateNativeParams(decoded, types);
	(void) scan->methods->CreateCustomScanState(scan);
	/* The same source can legitimately parameterize multiple later children. */
	scan = native_fixture();
	scan->custom_plans = lappend(scan->custom_plans, copyObject(lsecond(scan->custom_plans)));
	scan->custom_exprs = lappend(scan->custom_exprs, NIL);
	linitial(fixture_payload(scan)) = lappend(linitial_node(List, fixture_payload(scan)), list_make1_int(2));
	lsecond(fixture_payload(scan)) = lappend(lsecond_node(List, fixture_payload(scan)),
		list_make1(list_make2_int(70000, 1)));
	(void) GgcPgqDecodeNativePlan(scan);

	/* Every malformed private map must be rejected before PG constructs the
	 * scan tuple descriptor.  Only statement slot limits wait until Begin. */
	for (int malformed = 0; malformed < 30; malformed++)
	{
		MemoryContext old = CurrentMemoryContext;
		MemoryContext test = AllocSetContextCreate(old, "invalid native metadata",
			ALLOCSET_SMALL_SIZES);
		volatile bool rejected = false;
		List *payload;
		List *maps;
		List *parameters;

		MemoryContextSwitchTo(test);
		scan = native_fixture();
		payload = fixture_payload(scan);
		maps = linitial_node(List, payload);
		parameters = lsecond_node(List, payload);
		switch (malformed)
		{
			case 0: lsecond(scan->custom_private) = makeInteger(99); break;
			case 1: linitial(scan->custom_private) = makeInteger(1); break;
			case 2: scan->custom_private = payload; break;
			case 3: lthird(scan->custom_private) = list_truncate(payload, 2); break;
			case 4: lthird(payload) = makeInteger(0); break;
			case 5: scan->custom_exprs = NIL; break;
			case 6: linitial(maps) = list_make1(makeInteger(1)); break;
			case 7: linitial(maps) = NIL; break;
			case 8: linitial_int((List *) linitial(maps)) = 0; break;
			case 9: linitial_int((List *) lsecond(maps)) = 3; break;
			case 10: linitial(scan->custom_scan_tlist) = makeInteger(1); break;
			case 11: lsecond(scan->custom_plans) = makeInteger(1); break;
			case 12:
				castNode(Var, linitial_node(TargetEntry,
					((Plan *) lsecond(scan->custom_plans))->targetlist)->expr)->vartype = INT8OID;
				break;
			case 13:
				castNode(Var, lsecond_node(TargetEntry, scan->custom_scan_tlist)->expr)->varcollid = InvalidOid;
				break;
			case 14: lsecond(parameters) = list_make1(list_make1_int(70000)); break;
			case 15: lsecond(parameters) = list_make1(list_make2_int(-1, 1)); break;
			case 16:
				/* Stage zero cannot read the anchor before it has run. */
				linitial(parameters) = lsecond(parameters);
				break;
			case 17: lsecond(parameters) = list_make1(list_make2_int(70000, 2)); break;
			case 18:
				lsecond(parameters) = list_make2(list_make2_int(70000, 1), list_make2_int(70000, 1));
				break;
			case 19: scan->custom_params = NULL; break;
			case 20: scan->custom_param_exprs = list_make1(NULL); break;
			case 21: lsecond(parameters) = NIL; break;
			case 22:
				{
					NullTest *nulltest = makeNode(NullTest);

					nulltest->arg = (Expr *) makeVar(INDEX_VAR, 2, TEXTOID, -1, DEFAULT_COLLATION_OID, 0);
					nulltest->nulltesttype = IS_NOT_NULL;
					linitial(scan->custom_exprs) = list_make1(nulltest);
					break;
				}
			case 23:
				lsecond(scan->custom_exprs) = list_make1(makeConst(INT4OID, -1,
					InvalidOid, sizeof(int32), Int32GetDatum(1), false, true));
				break;
			case 24:
				castNode(Var, linitial_node(TargetEntry, scan->scan.plan.targetlist)->expr)->varattno = 3;
				break;
			case 25: case 26: break; /* runtime parameter bounds/type */
			case 27: scan->scan.plan.qual = list_make1(NULL); break;
			case 28:
				scan->scan.plan.qual = list_make1(makeConst(INT4OID, -1,
					InvalidOid, sizeof(int32), Int32GetDatum(42), false, true));
				break;
			case 29:
				castNode(Var, lsecond_node(TargetEntry, scan->custom_scan_tlist)->expr)->vartypmod = 42;
				break;
		}
		PG_TRY();
		{
			if (malformed != 25 && malformed != 26)
				(void) scan->methods->CreateCustomScanState(scan);
			else
			{
				decoded = GgcPgqDecodeNativePlan(scan);
				if (malformed == 25)
					GgcPgqValidateNativeParams(decoded, NIL);
				else
				{
					List *badtypes = list_copy(types);

					lfirst_oid(list_nth_cell(badtypes, 70000)) = TEXTOID;
					GgcPgqValidateNativeParams(decoded, badtypes);
				}
			}
		}
		PG_CATCH();
		{
			ErrorData *error;

			MemoryContextSwitchTo(old);
			error = CopyErrorData();
			FlushErrorState();
			if (error->sqlerrcode != ERRCODE_DATA_CORRUPTED)
				ReThrowError(error);
			FreeErrorData(error);
			rejected = true;
		}
		PG_END_TRY();
		MemoryContextSwitchTo(old);
		MemoryContextDelete(test);
		if (!rejected)
			elog(ERROR, "native metadata accepted invalid case %d", malformed);
	}
	PG_RETURN_BOOL(true);
}

static void
reject_plan(CustomScan *scan, const char *label)
{
	MemoryContext old = CurrentMemoryContext;
	MemoryContext context = AllocSetContextCreate(old, "invalid graph plan test",
		ALLOCSET_SMALL_SIZES);
	volatile bool rejected = false;

	MemoryContextSwitchTo(context);
	PG_TRY();
	{
		(void) GgcPgqDecodeFixedPlan(scan);
	}
	PG_CATCH();
	{
		ErrorData *error;

		MemoryContextSwitchTo(old);
		error = CopyErrorData();
		FlushErrorState();
		if (error->sqlerrcode != ERRCODE_DATA_CORRUPTED)
			ReThrowError(error);
		FreeErrorData(error);
		rejected = true;
	}
	PG_END_TRY();
	MemoryContextSwitchTo(old);
	MemoryContextDelete(context);
	if (!rejected)
		elog(ERROR, "graph metadata accepted invalid %s", label);
}

Datum
test_graph_plan_metadata(PG_FUNCTION_ARGS)
{
	const int lengths[] = {13, 15, 16, 21};
	CustomScan *scan;
	GgcPgqPlan *decoded;
	List *payload;
	List *fields;
	List *copy;
	List *roundtrip;

	/* All old diagnostic stage/option shapes normalize to the same descriptor.
	 * The second stage is a real backward reference, not a duplicated List. */
	for (int join = 0; join <= 1; join++)
		for (int s = 0; s < (int) lengthof(lengths); s++)
			for (int options = 11; options <= 13; options++)
			{
				scan = plan_fixture(join, lengths[s], options);
				copy = copyObject(scan->custom_private);
				roundtrip = stringToNode(nodeToString(copy));
				if (!equal(scan->custom_private, roundtrip))
					elog(ERROR, "graph plan metadata did not round-trip as Nodes");
				scan->custom_private = roundtrip;
				decoded = GgcPgqDecodeFixedPlan(scan);
				if (decoded->stage_count != 2 || decoded->stage_specs != 1 ||
					decoded->join_mode != join || decoded->stages[1].primary.index_relid != 2)
					elog(ERROR, "graph metadata normalization changed a stage");
			}
	scan = plan_fixture(true, 21, 13);
	payload = fixture_payload(scan);
	fields = lsecond_node(List, payload);
	lfirst(list_nth_cell(fields, 3)) = makeString(pstrdup("18446744073709551615"));
	lfirst(list_nth_cell(fields, 12)) = makeInteger(true);
	decoded = GgcPgqDecodeFixedPlan(scan);
	if (decoded->options.max_path_bindings != PG_UINT64_MAX || !decoded->endpoint_set)
		elog(ERROR, "graph work budget was truncated");
	/* Legacy join diagnostics omit candidate metadata and vertex RTIs. */
	scan = plan_fixture(true, 13, 11);
	payload = fixture_payload(scan);
	fields = lthird_node(List, payload);
	lfirst(list_nth_cell(fields, 15)) = NIL;
	lthird(payload) = list_truncate(fields, 17);
	(void) GgcPgqDecodeFixedPlan(scan);

	scan = plan_fixture(false, 13, 11);
	lsecond(scan->custom_private) = makeInteger(GGC_PGQ_PLAN_VERSION + 1);
	reject_plan(scan, "version");
	scan = plan_fixture(false, 13, 11);
	linitial(scan->custom_private) = makeInteger(1);
	reject_plan(scan, "tag kind");
	scan = plan_fixture(false, 13, 11);
	scan->custom_private = fixture_payload(scan);
	reject_plan(scan, "unversioned payload");
	scan = plan_fixture(false, 13, 11);
	fields = linitial_node(List, fixture_payload(scan));
	lsecond(fields) = makeInteger(1);
	reject_plan(scan, "forward reference");
	scan = plan_fixture(false, 13, 11);
	fields = linitial_node(List, fixture_payload(scan));
	linitial(fields) = list_truncate(linitial_node(List, fields), 12);
	reject_plan(scan, "truncated stage");
	scan = plan_fixture(false, 13, 11);
	fields = lsecond_node(List, fixture_payload(scan));
	lfirst(list_nth_cell(fields, 3)) = makeString(pstrdup("18446744073709551616"));
	reject_plan(scan, "uint64 overflow");
	scan = plan_fixture(false, 13, 11);
	fields = lsecond_node(List, fixture_payload(scan));
	lfirst(list_nth_cell(fields, 3)) = makeInteger(-1);
	reject_plan(scan, "negative budget");
	scan = plan_fixture(false, 13, 11);
	fields = lsecond_node(List, fixture_payload(scan));
	lfirst(list_nth_cell(fields, 9)) = makeInteger(2);
	reject_plan(scan, "boolean");
	scan = plan_fixture(true, 21, 11);
	fields = linitial_node(List, list_nth_node(List, fixture_payload(scan), 3));
	lsecond(fields) = makeInteger(3);
	reject_plan(scan, "output binding");
	scan = plan_fixture(true, 21, 11);
	fields = linitial_node(List, list_nth_node(List, fixture_payload(scan), 3));
	lthird(fields) = makeInteger(false);
	lfirst(list_nth_cell(fields, 3)) = makeInteger(-1);
	reject_plan(scan, "output representation");
	scan = plan_fixture(true, 21, 11);
	fields = lthird_node(List, fixture_payload(scan));
	lfirst(list_nth_cell(fields, 17)) = list_make2(makeInteger(0), makeInteger(2));
	reject_plan(scan, "unbound input");
	scan = plan_fixture(true, 21, 11);
	scan->custom_exprs = list_make1(makeInteger(1));
	reject_plan(scan, "predicate shape");
	scan = plan_fixture(true, 21, 11);
	payload = fixture_payload(scan);
	payload = lappend(payload, list_make4(list_make1(list_make2_int(2, 1)), NIL, NIL, NIL));
	lthird(scan->custom_private) = payload;
	reject_plan(scan, "local scan column");
	scan = plan_fixture(false, 13, 11);
	scan->custom_scan_tlist = list_truncate(scan->custom_scan_tlist, 1);
	reject_plan(scan, "diagnostic slot width");
	scan = plan_fixture(false, 13, 11);
	castNode(Var, lsecond_node(TargetEntry, scan->custom_scan_tlist)->expr)->vartype = TEXTOID;
	reject_plan(scan, "diagnostic slot type");
	scan = plan_fixture(true, 21, 11);
	linitial(scan->custom_scan_tlist) = makeInteger(1);
	reject_plan(scan, "scan target node tag");
	PG_RETURN_BOOL(true);
}

/* Exercise the generic contract, without a concrete graph provider.  An ID
 * above AttrNumber's range proves this is not a disguised target resno. */
Datum
test_graph_custom_parameters(PG_FUNCTION_ARGS)
{
	const int paramid = 70000;

	for (int variant = 0; variant < 10; variant++)
	{
		int wrapper = variant % 5;
		CustomScan *custom = makeNode(CustomScan);
		Result *leaf = makeNode(Result);
		PlannedStmt *stmt = makeNode(PlannedStmt);
		Param *param = makeNode(Param);
		Plan *child = &leaf->plan;
		List *ancestors;
		List *context;
		char *rendered;

		custom->custom_params = bms_make_singleton(paramid);
		custom->custom_param_ids = list_make1_int(paramid);
		custom->custom_param_exprs = list_make1(makeVar(INDEX_VAR, 1,
			INT4OID, -1, InvalidOid, 0));
		custom->custom_scan_tlist = list_make1(makeTargetEntry((Expr *)
			makeConst(INT4OID, -1, InvalidOid, sizeof(int32), Int32GetDatum(42),
				false, true), 1, NULL, false));
		ancestors = list_make1(custom);
		if (wrapper)
		{
			if (wrapper == 3)
			{
				Append *append = makeNode(Append);

				append->appendplans = list_make1(leaf);
				child = &append->plan;
			}
			else if (wrapper == 4)
			{
				SubqueryScan *subquery = makeNode(SubqueryScan);

				subquery->subplan = &leaf->plan;
				child = &subquery->scan.plan;
			}
			else
			{
				child = wrapper == 1 ? (Plan *) makeNode(Material) : (Plan *) makeNode(Memoize);
				outerPlan(child) = &leaf->plan;
			}
			ancestors = lcons(child, ancestors);
		}
		custom->custom_plans = list_make1(child);
		stmt->planTree = &custom->scan.plan;
		if (variant >= 5)
		{
			NestLoop *outer = makeNode(NestLoop);
			Result *source = makeNode(Result);
			NestLoopParam *binding = makeNode(NestLoopParam);
			Param *input = makeNode(Param);

			/* A local custom parameter may be sourced by a genuinely outer
			 * parameter.  Resolve both scopes without capturing the latter. */
			input->paramkind = PARAM_EXEC;
			input->paramid = 9;
			input->paramtype = INT4OID;
			input->paramtypmod = -1;
			custom->custom_param_exprs = list_make1(input);
			binding->paramno = 9;
			binding->paramval = makeVar(OUTER_VAR, 1, INT4OID, -1, InvalidOid, 0);
			outer->nestParams = list_make1(binding);
			source->plan.targetlist = copyObject(custom->custom_scan_tlist);
			outerPlan(outer) = &source->plan;
			innerPlan(outer) = &custom->scan.plan;
			ancestors = lappend(ancestors, outer);
			stmt->planTree = &outer->join.plan;
		}
		param->paramkind = PARAM_EXEC;
		param->paramid = paramid;
		param->paramtype = INT4OID;
		param->paramtypmod = -1;
		context = deparse_context_for_plan_tree(stmt, NIL);
		context = set_deparse_context_plan(context, &leaf->plan, ancestors);
		rendered = deparse_expression((Node *) param, context, false, false);
		if (strcmp(rendered, "42") != 0 && strcmp(rendered, "(42)") != 0)
			elog(ERROR, "custom parameter source was not deparsed: %s", rendered);
	}

	/* Validate the new paired fields using the actual setrefs entry point.
	 * Case zero is valid; the others must fail before an executor sees them. */
	for (int malformed = 0; malformed <= 5; malformed++)
	{
		MemoryContext old = CurrentMemoryContext;
		MemoryContext test = AllocSetContextCreate(old, "custom parameter contract test",
			ALLOCSET_SMALL_SIZES);
		volatile bool rejected = false;
		CustomScan *custom;
		PlannerInfo *root;

		MemoryContextSwitchTo(test);
		custom = makeNode(CustomScan);
		root = makeNode(PlannerInfo);
		root->glob = makeNode(PlannerGlobal);
		root->parse = makeNode(Query);
		custom->custom_params = bms_make_singleton(paramid);
		custom->custom_param_ids = list_make1_int(paramid);
		custom->custom_param_exprs = list_make1(makeBoolConst(true, false));
		switch (malformed)
		{
			case 1: custom->custom_param_exprs = NIL; break;
			case 2: linitial(custom->custom_param_exprs) = NULL; break;
			case 3: custom->custom_params = NULL; break;
			case 4:
				custom->custom_param_ids = lappend_int(custom->custom_param_ids, paramid);
				custom->custom_param_exprs = lappend(custom->custom_param_exprs, makeBoolConst(true, false));
				break;
			case 5: linitial_int(custom->custom_param_ids) = -1; break;
		}
		PG_TRY();
		{
			(void) set_plan_references(root, &custom->scan.plan);
		}
		PG_CATCH();
		{
			ErrorData *error;

			MemoryContextSwitchTo(old);
			error = CopyErrorData();
			FlushErrorState();
			if (error->sqlerrcode != ERRCODE_INTERNAL_ERROR)
				ReThrowError(error);
			FreeErrorData(error);
			rejected = true;
		}
		PG_END_TRY();
		MemoryContextSwitchTo(old);
		MemoryContextDelete(test);
		if (rejected != (malformed != 0))
			elog(ERROR, "unexpected custom parameter contract result in case %d", malformed);
	}
	PG_RETURN_BOOL(true);
}
