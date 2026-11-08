/*-------------------------------------------------------------------------
 * graph_pgq_plan.c
 *
 * The one wire-format boundary for SQL/PGQ execution.  No unchecked
 * list indexing, callback pointers or cached catalog objects escape here.
 * The wire payload uses normal List/Integer/String Nodes, so copyObject,
 * equal and the normal node read/write machinery retain their contracts.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "executor/graph_access.h"
#include "executor/graph_pgq_plan.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"

#define PGQ_PLAN_TAG "GgcPgqFixedPlan"
#define PGQ_NATIVE_PLAN_TAG "GgcPgqNativePlan"

static void
invalid_plan(const char *field)
{
	ereport(ERROR,
			(errcode(ERRCODE_DATA_CORRUPTED),
			 errmsg("invalid graph plan metadata"),
			 errdetail("Invalid %s in graph plan version %d.",
					   field, GGC_PGQ_PLAN_VERSION)));
}

/* Every wire field is checked before intVal/strVal/list_nth touches it.
 * An empty List is represented by NULL, including lists nested in Lists. */
static List *
node_list(const void *node, const char *field)
{
	if (node != NULL && !IsA(node, List))
		invalid_plan(field);
	return (List *) node;
}

static Node *
field_node(const List *list, int position, const char *field)
{
	if (position < 0 || position >= list_length(list))
		invalid_plan(field);
	return list_nth(list, position);
}

static int
integer_node(const Node *node, const char *field)
{
	if (node == NULL || !IsA(node, Integer))
		invalid_plan(field);
	return ((const Integer *) node)->ival;
}

static int
integer_field(const List *list, int position, const char *field)
{
	return integer_node(field_node(list, position, field), field);
}

static int
bounded_field(const List *list, int position, int min, int max,
			  const char *field)
{
	int value = integer_field(list, position, field);

	if (value < min || value > max)
		invalid_plan(field);
	return value;
}

static List *
list_field(const List *list, int position, const char *field)
{
	return node_list(field_node(list, position, field), field);
}

static List *
decode_envelope(const CustomScan *scan, const char *expected_tag)
{
	List *envelope;
	Node *tag;

	if (scan == NULL || !IsA(scan, CustomScan))
		invalid_plan("custom scan node");
	envelope = node_list(scan->custom_private, "version envelope");
	if (list_length(envelope) != 3)
		invalid_plan("version envelope length");
	tag = field_node(envelope, 0, "format tag");
	if (!tag || !IsA(tag, String) || strVal(tag) == NULL ||
		strcmp(strVal(tag), expected_tag) != 0)
		invalid_plan("format tag");
	if (integer_field(envelope, 1, "format version") != GGC_PGQ_PLAN_VERSION)
		invalid_plan("unsupported format version");
	return list_field(envelope, 2, "payload");
}

static uint64
unsigned_node(const Node *node, bool allow_integer, const char *field)
{
	const char *text;
	uint64 value = 0;

	if (node != NULL && IsA(node, Integer) && allow_integer)
	{
		int integer = ((const Integer *) node)->ival;

		if (integer < 0)
			invalid_plan(field);
		return (uint64) integer;
	}
	if (node == NULL || !IsA(node, String))
		invalid_plan(field);
	text = ((const String *) node)->sval;
	if (text == NULL || *text == '\0')
		invalid_plan(field);
	for (; *text; text++)
	{
		unsigned int digit;

		if (*text < '0' || *text > '9')
			invalid_plan(field);
		digit = *text - '0';
		if (value > (PG_UINT64_MAX - digit) / 10)
			invalid_plan(field);
		value = value * 10 + digit;
	}
	return value;
}

static uint64
unsigned_field(const List *list, int position, const char *field)
{
	return unsigned_node(field_node(list, position, field), false, field);
}

static Size
size_field(const List *list, int position, const char *field)
{
	uint64 value = unsigned_field(list, position, field);

	if (value > SIZE_MAX)
		invalid_plan(field);
	return (Size) value;
}

static void
decode_provider(List *wire, GgcProviderRef *provider)
{
	Node *name;

	if (list_length(wire) != 5)
		invalid_plan("provider fields");
	provider->struct_size = bounded_field(wire, 0, 0, INT_MAX, "provider size");
	provider->abi_version = bounded_field(wire, 1, 0, INT_MAX, "provider ABI");
	provider->owner_extension_oid = (Oid) integer_field(wire, 2, "provider owner");
	provider->kind = bounded_field(wire, 3, 0, INT_MAX, "provider kind");
	name = field_node(wire, 4, "provider name");
	if (!name || !IsA(name, String) || strVal(name) == NULL ||
		strlen(strVal(name)) >= NAMEDATALEN)
		invalid_plan("provider name");
	namestrcpy(&provider->name, strVal(name));
	/* ABI/kind availability is deliberately checked by provider admission,
	 * not here: diagnostics exercise fallback from a rejected provider. */
}

static void
decode_options(GgcPgqPlan *plan, List *wire)
{
	GgcFixedExecOptions *options = &plan->options;
	int count = list_length(wire);

	if (count != 11 && count != 12 && count != 13)
		invalid_plan("executor options length");
	plan->pause_ms = bounded_field(wire, 0, 0, INT_MAX, "diagnostic pause");
	options->struct_size = sizeof(*options);
	options->version = GGC_FIXED_EXEC_VERSION;
	options->anchor_attnum = 1;
	options->anchor_batch_size = bounded_field(wire, 1, 1, INT_MAX, "anchor batch");
	options->max_parent_batch = bounded_field(wire, 2, 1, INT_MAX, "parent batch");
	options->max_expanded_edges = unsigned_node(
		field_node(wire, 3, "work budget"), true, "work budget");
	if (!options->max_expanded_edges)
		invalid_plan("zero work budget");
	options->max_path_bindings = options->max_expanded_edges;
	plan->wrapper_mode = bounded_field(wire, 4, 0, INT_MAX, "diagnostic wrapper");
	options->batch_budget.max_endpoints = bounded_field(wire, 5, 1, INT_MAX, "endpoint batch");
	options->batch_budget.max_candidates = bounded_field(wire, 6, 1, INT_MAX, "candidate batch");
	options->batch_budget.max_inflight_pages = bounded_field(wire, 7, 1, INT_MAX, "page batch");
	options->batch_budget.max_pins = 4;
	options->batch_budget.max_candidate_bytes = mul_size(
		options->batch_budget.max_candidates, sizeof(GgcAdjItemV2));
	options->batch_budget.max_restore_bytes = size_field(wire, 8, "restore byte budget");
	options->batch_options.struct_size = sizeof(GgcBatchOptions);
	options->batch_options.version = GGC_BATCH_EXEC_VERSION;
	options->batch_options.prefetch = bounded_field(wire, 9, 0, 1, "prefetch flag");
	options->batch_budget.preserve_order = bounded_field(wire, 10, 0, 1, "order flag");
	options->batch_options.small_parent_threshold = 2;
	options->batch_options.small_candidate_limit = 2;
	if (!GgcBatchBudgetIsBounded(&options->batch_budget))
		invalid_plan("unbounded candidate workspace");
	plan->has_factor_budget = count >= 12;
	if (plan->has_factor_budget)
		plan->factor_bytes = size_field(wire, 11, "factor byte budget");
	if (count == 13)
		plan->endpoint_set = bounded_field(wire, 12, 0, 1, "endpoint-set flag");
}

static void
decode_stages(GgcPgqPlan *plan, List *wire)
{
	int position = 0;

	plan->stages = palloc0(mul_size(plan->stage_count, sizeof(*plan->stages)));
	plan->reverse_access = palloc0(mul_size(plan->stage_count, sizeof(*plan->reverse_access)));
	foreach_ptr(Node, entry, wire)
	{
		GgcFixedStagePlan *stage = &plan->stages[position];
		List *fields;
		int count;

		if (entry != NULL && IsA(entry, Integer))
		{
			int previous = intVal(entry);

			if (previous < 0 || previous >= position)
				invalid_plan("stage backward reference");
			*stage = plan->stages[previous];
			plan->reverse_access[position] = plan->reverse_access[previous];
			position++;
			continue;
		}
		fields = node_list(entry, "stage fields");
		count = list_length(fields);
		if (count != 13 && count != 15 && count != 16 && count != 21)
			invalid_plan("stage field count");
		plan->stage_specs++;
		stage->edge_relid = (Oid) integer_field(fields, 0, "edge relation");
		stage->direction = bounded_field(fields, 1, GGC_OUT, GGC_IN, "stage direction");
		decode_provider(list_field(fields, 2, "primary provider"), &stage->primary.provider);
		stage->primary.index_relid = (Oid) integer_field(fields, 3, "primary index");
		stage->primary.build_generation = unsigned_field(fields, 4, "primary generation");
		stage->has_fallback = bounded_field(fields, 5, 0, 1, "fallback flag");
		decode_provider(list_field(fields, 6, "fallback provider"), &stage->fallback.provider);
		stage->fallback.index_relid = (Oid) integer_field(fields, 7, "fallback index");
		stage->fallback.build_generation = unsigned_field(fields, 8, "fallback generation");
		stage->graph_oid = (Oid) integer_field(fields, 9, "graph oid");
		stage->catalog_generation = unsigned_field(fields, 10, "catalog generation");
		stage->statistics_generation = unsigned_field(fields, 11, "statistics generation");
		stage->endpoint_domain = (uint32) integer_field(fields, 12, "endpoint domain");
		stage->edge_identity_attnum = 1;
		stage->source_attnum = 2;
		stage->destination_attnum = 3;
		stage->payload_attnum = plan->endpoint_set ? InvalidAttrNumber : 4;
		if (count >= 15)
		{
			stage->vertex_relid = (Oid) integer_field(fields, 13, "vertex relation");
			stage->vertex_index_relid = (Oid) integer_field(fields, 14, "vertex index");
			stage->vertex_key_attnum = 1;
		}
		if (count >= 16)
		{
			Oid reverse = (Oid) integer_field(fields, 15, "reverse index");

			if (OidIsValid(reverse))
			{
				plan->reverse_access[position] = stage->primary;
				plan->reverse_access[position].index_relid = reverse;
				plan->has_reverse = true;
			}
		}
		if (count == 21)
		{
			stage->edge_identity_attnum = bounded_field(fields, 16, 1, PG_INT16_MAX, "edge key column");
			stage->source_attnum = bounded_field(fields, 17, 1, PG_INT16_MAX, "source column");
			stage->destination_attnum = bounded_field(fields, 18, 1, PG_INT16_MAX, "destination column");
			stage->payload_attnum = bounded_field(fields, 19, 0, PG_INT16_MAX, "payload column");
			stage->vertex_key_attnum = bounded_field(fields, 20, 1, PG_INT16_MAX, "vertex key column");
			if (plan->endpoint_set)
				stage->payload_attnum = InvalidAttrNumber;
		}
		position++;
	}
}

static Index *
decode_rtis(List *wire, int count, const char *field)
{
	Index *result;
	int position = 0;

	if (list_length(wire) != count)
		invalid_plan(field);
	result = palloc(mul_size(count, sizeof(*result)));
	foreach_ptr(Node, entry, wire)
	{
		int rti = integer_node(entry, field);

		if (rti <= 0)
			invalid_plan(field);
		result[position++] = rti;
	}
	return result;
}

static void
decode_join(GgcPgqPlan *plan, List *wire)
{
	GgcPgqJoinMetadata *join = &plan->join;
	int count = list_length(wire);
	List *vertices;

	if ((count != 17 && count != 23) || integer_field(wire, 0, "join flag") != 1)
		invalid_plan("join metadata length or mode");
	join->logical_state = bounded_field(wire, 1, GGC_STATS_ABSENT, GGC_STATS_INVALID, "logical statistics state");
	join->physical_state = bounded_field(wire, 2, GGC_STATS_ABSENT, GGC_STATS_INVALID, "physical statistics state");
	join->cost_state = bounded_field(wire, 3, GGC_STATS_ABSENT, GGC_STATS_INVALID, "cost statistics state");
	join->uncertainty_unknown = bounded_field(wire, 4, 0, 1, "uncertainty flag");
	join->calibrated = bounded_field(wire, 5, 0, 1, "calibration flag");
	join->logical_generation = unsigned_field(wire, 6, "logical generation");
	join->physical_generation = unsigned_field(wire, 7, "physical generation");
	join->cost_generation = unsigned_field(wire, 8, "cost generation");
	join->cost_profile_version = bounded_field(wire, 9, 0, INT_MAX, "cost profile version");
	join->covered_relids = bounded_field(wire, 10, 0, INT_MAX, "covered relations");
	join->consumed_quals = bounded_field(wire, 11, 0, INT_MAX, "consumed predicates");
	join->required_outer = bounded_field(wire, 12, 0, INT_MAX, "required outer count");
	join->request_rti = bounded_field(wire, 13, 1, INT_MAX, "request rti");
	join->stage_rtis = decode_rtis(list_field(wire, 14, "stage rtis"), plan->stage_count, "stage rtis");
	vertices = list_field(wire, 15, "vertex rtis");
	if (vertices != NIL)
		join->vertex_rtis = decode_rtis(vertices, plan->stage_count + 1, "vertex rtis");
	plan->options.anchor_attnum = bounded_field(wire, 16, 1, PG_INT16_MAX, "anchor column");
	if (count == 23)
	{
		List *inputs = list_field(wire, 17, "input positions");
		List *matches = list_field(wire, 18, "match positions");

		if (list_length(inputs) != (int) plan->stage_count ||
			list_length(matches) != (int) plan->stage_count)
			invalid_plan("binding position counts");
		join->has_binding_positions = true;
		join->input_positions = palloc(mul_size(plan->stage_count, sizeof(uint32)));
		join->match_positions = palloc(mul_size(plan->stage_count, sizeof(int32)));
		for (uint32 i = 0; i < plan->stage_count; i++)
		{
			join->input_positions[i] = bounded_field(inputs, i, 0, i, "input binding position");
			join->match_positions[i] = bounded_field(matches, i, -1, i, "matched binding position");
		}
		join->anchor_position = bounded_field(wire, 19, 0, plan->stage_count, "anchor position");
		join->candidates = bounded_field(wire, 20, 1, INT_MAX, "candidate count");
		join->candidate_budget = bounded_field(wire, 21, 1, INT_MAX, "candidate budget");
		join->enumeration_limited = bounded_field(wire, 22, 0, 1, "enumeration flag");
	}
}

static void
decode_properties(GgcPgqPlan *plan, List *wire)
{
	int position = 0;

	plan->property_count = list_length(wire);
	plan->properties = palloc0(mul_size(plan->property_count, sizeof(*plan->properties)));
	foreach_ptr(Node, entry, wire)
	{
		List *fields = node_list(entry, "property specification");
		GgcPgqPropertySpec *property = &plan->properties[position++];

		if (list_length(fields) != 5 && list_length(fields) != 6)
			invalid_plan("property field count");
		property->rti = bounded_field(fields, 0, 1, INT_MAX, "property rti");
		property->relation = (Oid) integer_field(fields, 1, "property relation");
		property->index = (Oid) integer_field(fields, 2, "property index");
		property->kind = bounded_field(fields, 3, GGC_PGQ_OUTPUT_VERTEX,
			GGC_PGQ_OUTPUT_EDGE, "property binding kind");
		property->position = bounded_field(fields, 4, 0,
			plan->stage_count - (property->kind == GGC_PGQ_OUTPUT_EDGE), "property binding position");
		property->key_attribute = list_length(fields) == 6 ?
			bounded_field(fields, 5, 1, PG_INT16_MAX, "property key column") : 1;
	}
}

static void
decode_outputs(GgcPgqPlan *plan, List *wire)
{
	int position = 0;

	plan->output_count = list_length(wire);
	if (plan->output_count > PG_INT16_MAX)
		invalid_plan("output count");
	plan->outputs = palloc0(mul_size(plan->output_count, sizeof(*plan->outputs)));
	foreach_ptr(Node, entry, wire)
	{
		List *fields = node_list(entry, "output mapping");
		GgcPgqOutputMapping *mapping = &plan->outputs[position++];
		int upper;

		if (list_length(fields) != 5)
			invalid_plan("output field count");
		mapping->kind = bounded_field(fields, 0, GGC_PGQ_OUTPUT_VERTEX,
			GGC_PGQ_OUTPUT_PROPERTY, "output binding kind");
		upper = mapping->kind == GGC_PGQ_OUTPUT_PROPERTY ? plan->property_count - 1 :
			(int) plan->stage_count - (mapping->kind == GGC_PGQ_OUTPUT_EDGE ||
								mapping->kind == GGC_PGQ_OUTPUT_PAYLOAD);
		if (mapping->kind == GGC_PGQ_OUTPUT_FILTER_ONLY)
			upper = -1;		/* no binding exists: local callback owns this value */
		mapping->position = bounded_field(fields, 1,
			mapping->kind == GGC_PGQ_OUTPUT_FILTER_ONLY ? -1 : 0, upper, "output binding position");
		mapping->typbyval = bounded_field(fields, 2, 0, 1, "output by-value flag");
		mapping->typlen = bounded_field(fields, 3, -2, PG_INT16_MAX, "output type length");
		if (mapping->typlen == 0 ||
			(mapping->typbyval && mapping->typlen != 1 && mapping->typlen != 2 &&
			 mapping->typlen != 4 && mapping->typlen != (int) sizeof(Datum)))
			invalid_plan("output type representation");
		mapping->attribute = bounded_field(fields, 4, 1, PG_INT16_MAX, "output source column");
	}
}

static void
decode_locals(GgcPgqPlan *plan, List *wire)
{
	int position = 0;

	if (wire != NIL && list_length(wire) != 2 * (int) plan->stage_count)
		invalid_plan("local mapping count");
	plan->local_count = 2 * plan->stage_count;
	plan->locals = palloc0(mul_size(plan->local_count, sizeof(*plan->locals)));
	foreach_ptr(Node, entry, wire)
	{
		List *columns = node_list(entry, "local column mappings");
		GgcPgqLocalMapping *mapping = &plan->locals[position++];
		int column = 0;

		mapping->count = list_length(columns);
		mapping->columns = palloc0(mul_size(mapping->count, sizeof(*mapping->columns)));
		foreach_ptr(Node, pair, columns)
		{
			List *values;
			int target;
			int attribute;

			if (!pair || !IsA(pair, IntList) || list_length((List *) pair) != 2)
				invalid_plan("local column pair");
			values = (List *) pair;
			target = linitial_int(values);
			attribute = lsecond_int(values);
			if (target <= 0 || target > plan->output_count ||
				attribute <= 0 || attribute > PG_INT16_MAX)
				invalid_plan("local column position");
			mapping->columns[column++] = (GgcPgqLocalColumn) {target, attribute};
		}
	}
}

List *
GgcPgqEncodeFixedPlan(List *payload)
{
	return list_make3(makeString(pstrdup(PGQ_PLAN_TAG)),
		makeInteger(GGC_PGQ_PLAN_VERSION), payload);
}

GgcPgqPlan *
GgcPgqDecodeFixedPlan(const CustomScan *scan)
{
	List *payload;
	List *stages;
	GgcPgqPlan *plan;
	int fields;
	int expressions;

	payload = decode_envelope(scan, PGQ_PLAN_TAG);
	expressions = list_length(node_list(scan->custom_exprs, "predicate lists"));
	(void) node_list(scan->custom_scan_tlist, "scan target list");
	fields = list_length(payload);
	if (fields != 2 && fields != 4 && fields != 5 && fields != 6)
		invalid_plan("payload field count");
	stages = list_field(payload, 0, "stage list");
	if (list_length(stages) < 1 || list_length(stages) > PG_INT16_MAX)
		invalid_plan("stage count");
	if (list_length(node_list(scan->custom_plans, "anchor children")) != 1)
		invalid_plan("anchor child count");
	plan = palloc0(sizeof(*plan));
	plan->version = GGC_PGQ_PLAN_VERSION;
	plan->join_mode = fields >= 4;
	plan->stage_count = list_length(stages);
	decode_options(plan, list_field(payload, 1, "executor options"));
	decode_stages(plan, stages);
	if (plan->join_mode)
	{
		decode_join(plan, list_field(payload, 2, "join metadata"));
		decode_properties(plan, fields == 6 ? list_field(payload, 5, "properties") : NIL);
		decode_outputs(plan, list_field(payload, 3, "output mappings"));
		decode_locals(plan, fields >= 5 ? list_field(payload, 4, "local mappings") : NIL);
		if (plan->output_count != list_length(scan->custom_scan_tlist))
			invalid_plan("scan target/mapping count");
		/* datumCopy must use the actual scan-slot representation, not an
		 * unchecked length/by-value flag from private metadata. */
		for (int i = 0; i < plan->output_count; i++)
		{
			Node *entry = list_nth(scan->custom_scan_tlist, i);
			int16 typlen;
			bool typbyval;

			if (!entry || !IsA(entry, TargetEntry) ||
				castNode(TargetEntry, entry)->resno != i + 1)
				invalid_plan("scan target position");
			get_typlenbyval(exprType((Node *) castNode(TargetEntry, entry)->expr),
				&typlen, &typbyval);
			if (plan->outputs[i].typlen != typlen ||
				plan->outputs[i].typbyval != typbyval)
				invalid_plan("scan target representation");
		}
		/* An unused canonical fourth column must not cause a heap read. */
		for (uint32 i = 0; i < plan->stage_count; i++)
		{
			bool needed = false;

			for (int j = 0; j < plan->output_count; j++)
				if (plan->outputs[j].kind == GGC_PGQ_OUTPUT_PAYLOAD &&
					plan->outputs[j].position == (int) i)
					needed = true;
			if (!needed)
				plan->stages[i].payload_attnum = InvalidAttrNumber;
		}
	}
	if (expressions != 0 && expressions != 2 * (int) plan->stage_count &&
		expressions != 3 * (int) plan->stage_count)
		invalid_plan("predicate stage count");
	foreach_ptr(Node, entry, scan->custom_exprs)
		(void) node_list(entry, "predicate clause list");
	if (!plan->join_mode && expressions != 0)
		invalid_plan("diagnostic predicates");
	if (!plan->join_mode && !plan->endpoint_set)
	{
		const Oid types[] = {INT8OID, INT8ARRAYOID, INT8ARRAYOID, TEXTARRAYOID};
		int i = 0;

		/* The diagnostic bag consumer writes four fixed columns, regardless
		 * of outer projection.  A shorter or differently typed slot is unsafe
		 * even when the provider and all stage metadata are otherwise valid. */
		if (list_length(scan->custom_scan_tlist) != lengthof(types))
			invalid_plan("diagnostic scan target count");
		foreach_ptr(Node, entry, scan->custom_scan_tlist)
		{
			if (!entry || !IsA(entry, TargetEntry) ||
				castNode(TargetEntry, entry)->resno != i + 1 ||
				exprType((Node *) castNode(TargetEntry, entry)->expr) != types[i])
				invalid_plan("diagnostic scan target type");
			i++;
		}
	}
	if (plan->endpoint_set &&
		(expressions != 0 || !plan->has_factor_budget || plan->factor_bytes == 0 ||
		 list_length(scan->custom_scan_tlist) != 1 ||
		 (plan->join_mode && (plan->output_count != 1 ||
		  plan->outputs[0].kind != GGC_PGQ_OUTPUT_VERTEX ||
		  plan->outputs[0].position != (int) plan->stage_count))))
		invalid_plan("endpoint-set output contract");
	if (plan->endpoint_set)
	{
		Node *target = linitial(scan->custom_scan_tlist);

		if (!target || !IsA(target, TargetEntry) ||
			!GgcIntegerKeyType(exprType((Node *) castNode(TargetEntry, target)->expr)))
			invalid_plan("endpoint-set target type");
	}
	return plan;
}

/* Native wire layout: [tag, version, [column maps, parameter maps, hops]].
 * No Expr or Plan is hidden in custom_private: core setrefs, dependency and
 * serialization processing continue to own those ordinary PG objects. */
List *
GgcPgqEncodeNativePlan(List *columns, List *parameters, int hops)
{
	return list_make3(makeString(pstrdup(PGQ_NATIVE_PLAN_TAG)),
		makeInteger(GGC_PGQ_PLAN_VERSION),
		list_make3(columns, parameters, makeInteger(hops)));
}

static List *
native_int_list(const void *node, const char *field)
{
	if (node != NULL && !IsA(node, IntList))
		invalid_plan(field);
	return (List *) node;
}

/* Check the immediate child tag before accessing the Plan base struct, not
 * the internals of ordinary child plans.  Their own executors validate them. */
static Plan *
native_child(Node *node)
{
	if (node == NULL)
		invalid_plan("native child");
	switch (nodeTag(node))
	{
		case T_Result: case T_ProjectSet: case T_ModifyTable:
		case T_Append: case T_MergeAppend: case T_RecursiveUnion:
		case T_BitmapAnd: case T_BitmapOr:
		case T_SeqScan: case T_SampleScan: case T_IndexScan:
		case T_IndexOnlyScan: case T_BitmapIndexScan: case T_BitmapHeapScan:
		case T_TidScan: case T_TidRangeScan: case T_SubqueryScan:
		case T_FunctionScan: case T_TableFuncScan: case T_ValuesScan:
		case T_CteScan: case T_NamedTuplestoreScan: case T_WorkTableScan:
		case T_ForeignScan: case T_CustomScan:
		case T_NestLoop: case T_MergeJoin: case T_HashJoin:
		case T_Material: case T_Sort: case T_IncrementalSort: case T_Memoize:
		case T_Group: case T_Agg: case T_WindowAgg: case T_Unique:
		case T_Gather: case T_GatherMerge: case T_Hash:
		case T_SetOp: case T_LockRows: case T_Limit:
			return (Plan *) node;
		default:
			invalid_plan("native child node tag");
	}
	return NULL;
}

/* The executor maps physical target positions, including resjunk columns. */
static Expr **
native_targets(List *list)
{
	Expr **targets;
	int i = 0;

	(void) node_list(list, "native target list");
	if (list_length(list) > PG_INT16_MAX)
		invalid_plan("native target width");
	targets = palloc0(mul_size(list_length(list), sizeof(*targets)));
	foreach_ptr(Node, node, list)
	{
		TargetEntry *entry;

		if (node == NULL || !IsA(node, TargetEntry))
			invalid_plan("native target node tag");
		entry = (TargetEntry *) node;
		if (entry->resno != i + 1 || entry->expr == NULL ||
			!OidIsValid(exprType((Node *) entry->expr)))
			invalid_plan("native target position/type");
		targets[i++] = entry->expr;
	}
	return targets;
}

static bool
native_same_type(Node *left, Node *right)
{
	return exprType(left) == exprType(right) &&
		exprTypmod(left) == exprTypmod(right) &&
		exprCollation(left) == exprCollation(right);
}

static int
native_param_compare(const void *a, const void *b)
{
	const GgcPgqNativeParam *left = a;
	const GgcPgqNativeParam *right = b;

	return (left->id > right->id) - (left->id < right->id);
}

typedef struct NativeBindingScope
{
	int width;
	Expr **targets;
	Bitmapset *bound;
} NativeBindingScope;

/* Check only our slot/binding contract, leaving PG expression semantics to
 * ExecInitExpr.  A predicate at stage N may see N and earlier bindings. */
static bool
native_binding_walker(Node *node, NativeBindingScope *scope)
{
	if (node == NULL)
		return false;
	if (IsA(node, Var))
	{
		Var *var = (Var *) node;
		int column = var->varattno - 1;

		if (var->varno != INDEX_VAR || var->varlevelsup != 0 ||
			column < 0 || column >= scope->width ||
			!bms_is_member(column, scope->bound) ||
			!native_same_type(node, (Node *) scope->targets[column]))
			invalid_plan("native expression binding");
		return false;
	}
	return expression_tree_walker(node, native_binding_walker, scope);
}

static List *
native_quals(const void *node, NativeBindingScope *scope)
{
	List *quals = node_list(node, "native clauses");

	foreach_ptr(Node, qual, quals)
	{
		if (qual == NULL || exprType(qual) != BOOLOID)
			invalid_plan("native predicate type");
		native_binding_walker(qual, scope);
	}
	return quals;
}

GgcPgqNativePlan *
GgcPgqDecodeNativePlan(const CustomScan *scan)
{
	List *payload = decode_envelope(scan, PGQ_NATIVE_PLAN_TAG);
	List *maps;
	List *params;
	ListCell *idcell;
	ListCell *exprcell;
	GgcPgqNativePlan *plan = palloc0(sizeof(*plan));
	NativeBindingScope scope = {0};
	bool *used;
	int i = 0;

	if (list_length(payload) != 3 || scan->scan.scanrelid != 0)
		invalid_plan("native payload/scan relation");
	maps = list_field(payload, 0, "native column maps");
	params = list_field(payload, 1, "native parameter maps");
	plan->version = GGC_PGQ_PLAN_VERSION;
	plan->hops = bounded_field(payload, 2, 1, INT_MAX, "native hop count");
	plan->stage_count = list_length(node_list(scan->custom_plans, "native children"));
	/* A repeated graph variable needs no new access stage.  Do not equate
	 * the number of native bindings with 2*hops+1. */
	if (plan->stage_count < 1 || list_length(maps) != plan->stage_count ||
		list_length(params) != plan->stage_count ||
		list_length(node_list(scan->custom_exprs, "native predicate lists")) != plan->stage_count)
		invalid_plan("native stage count");
	scope.targets = native_targets(scan->custom_scan_tlist);
	scope.width = plan->column_count = list_length(scan->custom_scan_tlist);
	plan->stages = palloc0(mul_size(plan->stage_count, sizeof(*plan->stages)));
	(void) native_int_list(scan->custom_param_ids, "native public parameter ids");
	plan->parameter_count = list_length(scan->custom_param_ids);
	if (list_length(node_list(scan->custom_param_exprs, "native parameter sources")) !=
		plan->parameter_count || bms_num_members(scan->custom_params) != plan->parameter_count)
		invalid_plan("native parameter source count");
	plan->parameters = palloc0(mul_size(plan->parameter_count, sizeof(*plan->parameters)));
	used = palloc0(mul_size(plan->parameter_count, sizeof(*used)));
	forboth(idcell, scan->custom_param_ids, exprcell, scan->custom_param_exprs)
	{
		GgcPgqNativeParam *param = &plan->parameters[i++];
		Node *source = lfirst(exprcell);
		Var *var;

		param->id = lfirst_int(idcell);
		if (param->id < 0 || !bms_is_member(param->id, scan->custom_params) ||
			source == NULL || !IsA(source, Var))
			invalid_plan("native parameter source");
		var = (Var *) source;
		param->column = var->varattno - 1;
		if (var->varno != INDEX_VAR || var->varlevelsup != 0 ||
			param->column < 0 || param->column >= scope.width ||
			!native_same_type(source, (Node *) scope.targets[param->column]))
			invalid_plan("native parameter source column/type");
		param->type = var->vartype;
	}
	qsort(plan->parameters, plan->parameter_count, sizeof(*plan->parameters),
		native_param_compare);
	for (i = 1; i < plan->parameter_count; i++)
		if (plan->parameters[i-1].id == plan->parameters[i].id)
			invalid_plan("duplicate native parameter source");
	/* One-time gating happens before even the anchor is bound. */
	(void) native_quals(scan->scan.plan.qual, &scope);
	i = 0;
	foreach_ptr(Node, childnode, scan->custom_plans)
	{
		GgcPgqNativeStage *stage = &plan->stages[i];
		Plan *child = native_child(childnode);
		Expr **targets = native_targets(child->targetlist);
		List *columns = native_int_list(list_nth(maps, i), "native stage columns");
		List *parameters = node_list(list_nth(params, i), "native stage parameters");
		int j = 0;

		stage->column_count = list_length(columns);
		if (stage->column_count != list_length(child->targetlist))
			invalid_plan("native child/column map width");
		stage->columns = palloc0(mul_size(stage->column_count, sizeof(*stage->columns)));
		foreach_int(attribute, columns)
		{
			if (attribute < 1 || attribute > scope.width ||
				!native_same_type((Node *) targets[j], (Node *) scope.targets[attribute-1]))
				invalid_plan("native child column/type");
			stage->columns[j++] = attribute - 1;
		}
		stage->parameter_count = list_length(parameters);
		stage->parameters = palloc0(mul_size(stage->parameter_count, sizeof(*stage->parameters)));
		j = 0;
		foreach_ptr(Node, entry, parameters)
		{
			List *pair = native_int_list(entry, "native parameter pair");
			GgcPgqNativeParam key = {0};
			GgcPgqNativeParam *source;

			if (list_length(pair) != 2)
				invalid_plan("native parameter pair width");
			key.id = linitial_int(pair);
			source = bsearch(&key, plan->parameters, plan->parameter_count,
				sizeof(*plan->parameters), native_param_compare);
			if (source == NULL || lsecond_int(pair) != source->column + 1 ||
				!bms_is_member(source->column, scope.bound))
				invalid_plan("native parameter prior binding/source");
			stage->parameters[j++] = *source;
			used[source - plan->parameters] = true;
		}
		qsort(stage->parameters, stage->parameter_count, sizeof(*stage->parameters),
			native_param_compare);
		for (j = 1; j < stage->parameter_count; j++)
			if (stage->parameters[j-1].id == stage->parameters[j].id)
				invalid_plan("duplicate native stage parameter");
		for (j = 0; j < stage->column_count; j++)
			scope.bound = bms_add_member(scope.bound, stage->columns[j]);
		stage->quals = native_quals(list_nth(scan->custom_exprs, i), &scope);
		pfree(targets);
		i++;
	}
	if (bms_num_members(scope.bound) != scope.width)
		invalid_plan("unbound native output column");
	for (i = 0; i < plan->parameter_count; i++)
		if (!used[i])
			invalid_plan("unused native parameter source");
	/* Outer projection also executes over the completed binding slot. */
	pfree(native_targets(scan->scan.plan.targetlist));
	native_binding_walker((Node *) scan->scan.plan.targetlist, &scope);
	bms_free(scope.bound);
	pfree(scope.targets);
	pfree(used);
	return plan;
}

void
GgcPgqValidateNativeParams(const GgcPgqNativePlan *plan, List *param_exec_types)
{
	if (param_exec_types != NIL && !IsA(param_exec_types, OidList))
		invalid_plan("statement parameter types");
	for (int i = 0; i < plan->parameter_count; i++)
	{
		const GgcPgqNativeParam *param = &plan->parameters[i];

		if (param->id >= list_length(param_exec_types) ||
			list_nth_oid(param_exec_types, param->id) != param->type)
			invalid_plan("native statement parameter slot/type");
	}
}
