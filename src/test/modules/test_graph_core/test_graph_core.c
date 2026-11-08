/* Test-only SQL entry points for G1-010 common graph contracts. */
#include "postgres.h"

#include "catalog/pg_type_d.h"
#include "executor/graph_adapter.h"
#include "fmgr.h"
#include "nodes/graphnodes.h"
#include "nodes/makefuncs.h"
#include "nodes/parsenodes.h"
#include "nodes/queryjumble.h"
#include "utils/builtins.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(test_graph_node_contract);
PG_FUNCTION_INFO_V1(test_graph_provider_contract);

static GgcProviderIdentity *
make_provider_identity(uint32 kind, const char *name, uint32 abi_version)
{
	GgcProviderIdentity *identity = makeNode(GgcProviderIdentity);

	identity->struct_size = sizeof(GgcProviderIdentity);
	identity->abi_version = abi_version;
	identity->owner_extension_oid = InvalidOid;
	identity->kind = kind;
	identity->name = pstrdup(name);
	return identity;
}

static GgcFixedProgram *
make_test_program(void)
{
	GgcFixedProgram *program = makeNode(GgcFixedProgram);
	GgcVariable *source = makeNode(GgcVariable);
	GgcVariable *edge = makeNode(GgcVariable);
	GgcVariable *destination = makeNode(GgcVariable);
	GgcFixedHop *hop = makeNode(GgcFixedHop);
	GgcPathConstraint *constraint = makeNode(GgcPathConstraint);
	GgcOutput  *output = makeNode(GgcOutput);

	source->variable_id = 1;
	source->entity_kind = GGC_VERTEX;
	source->relation_oid = 9101;
	source->fallback_rti = 1;
	source->identity_attnum = 1;
	source->key_type = INT8OID;
	source->user_visible = true;
	edge->variable_id = 2;
	edge->entity_kind = GGC_EDGE;
	edge->relation_oid = 9102;
	edge->fallback_rti = 2;
	edge->identity_attnum = 1;
	edge->source_attnum = 2;
	edge->destination_attnum = 3;
	edge->key_type = INT8OID;
	destination->variable_id = 3;
	destination->entity_kind = GGC_VERTEX;
	destination->relation_oid = 9103;
	destination->fallback_rti = 3;
	destination->identity_attnum = 1;
	destination->key_type = INT8OID;
	destination->user_visible = true;

	constraint->constraint_id = 1;
	constraint->mode = GGC_TRAIL;
	constraint->scope_kind = GGC_SCOPE_PATH;
	constraint->edge_variables = bms_make_singleton(2);
	constraint->vertex_variables = bms_add_member(
		bms_make_singleton(1), 3);
	constraint->first_vertex = 1;
	constraint->last_vertex = 3;

	hop->stage_id = 10;
	hop->input_variable = 1;
	hop->edge_variable = 2;
	hop->output_variable = 3;
	hop->direction = GGC_OUT;
	hop->required_variables = bms_make_singleton(1);
	hop->quals = NIL;
	hop->constraint_ids = list_make1_int(1);

	output->result_attnum = 1;
	output->result_type = INT8OID;
	output->result_typmod = -1;
	output->required_variables = bms_make_singleton(3);

	program->semantic_version = GGC_GRAPH_SEMANTIC_VERSION;
	program->frontend_kind = 1;
	program->frontend_provider = make_provider_identity(1, "pgq_frontend", 1);
	program->catalog_provider = make_provider_identity(2, "pgq_catalog", 1);
	program->result_provider = make_provider_identity(3, "pgq_result", 1);
	program->graph_oid = 9001;
	program->graph_rti = 1;
	program->variables = list_make3(source, edge, destination);
	program->hops = list_make1(hop);
	program->path_constraints = list_make1(constraint);
	program->stage_order = list_make1_int(10);
	program->outputs = list_make1(output);
	program->required_outer = NULL;
	program->semantic_fingerprint = UINT64CONST(0x1020304050607080);
	program->catalog_generation = 7;
	return program;
}

static Query *
make_test_query(GgcFixedProgram *program)
{
	Query	   *query = makeNode(Query);

	query->commandType = CMD_SELECT;
	query->querySource = QSRC_ORIGINAL;
	query->canSetTag = true;
	query->graph_program = program;
	query->jointree = makeNode(FromExpr);
	return query;
}

Datum
test_graph_node_contract(PG_FUNCTION_ARGS)
{
	GgcFixedProgram *program = make_test_program();
	GgcFixedProgram *copied = copyObject(program);
	char	   *serialized = nodeToString(program);
	GgcFixedProgram *readback;
	Query	   *query_a = make_test_query(copyObject(program));
	Query	   *query_b = copyObject(query_a);
	Query	   *query_generation = copyObject(query_a);
	Query	   *query_dependencies = copyObject(query_a);
	GgcVariable *mapped;
	GgcFixedHop *dependency_hop;
	bool		copy_equal;
	bool		out_read_equal;
	bool		query_jumble_equal;
	bool		mapping_changes_jumble;
	bool		dependency_changes_jumble;
	bool		generation_ignored;
	int64		query_id_a;
	int64		query_id_b;
	int64		query_id_generation;
	char	   *result;

	readback = stringToNode(serialized);
	copy_equal = equal(program, copied);
	out_read_equal = equal(program, readback);
	EnableQueryId();
	(void) JumbleQuery(query_a);
	(void) JumbleQuery(query_b);
	query_id_a = query_a->queryId;
	query_jumble_equal = query_id_a == query_b->queryId;
	mapped = linitial_node(GgcVariable, query_b->graph_program->variables);
	mapped->fallback_rti++;
	(void) JumbleQuery(query_b);
	query_id_b = query_b->queryId;
	mapping_changes_jumble = query_id_a != query_id_b;
	dependency_hop = linitial_node(GgcFixedHop,
								query_dependencies->graph_program->hops);
	dependency_hop->required_variables =
		bms_add_member(dependency_hop->required_variables, 3);
	(void) JumbleQuery(query_dependencies);
	dependency_changes_jumble = query_id_a != query_dependencies->queryId;
	query_generation->graph_program->catalog_generation++;
	(void) JumbleQuery(query_generation);
	query_id_generation = query_generation->queryId;
	generation_ignored = query_id_a == query_id_generation;

	result = psprintf(
		"{\"copy_equal\":%s,\"out_read_equal\":%s,"
		"\"query_jumble_equal\":%s,\"mapping_changes_jumble\":%s,"
		"\"dependency_changes_jumble\":%s,"
		"\"catalog_generation_ignored_by_jumble\":%s,"
		"\"query_id_a\":%lld,\"query_id_mapping_changed\":%lld,"
		"\"query_id_generation_changed\":%lld,\"serialized_bytes\":%zu}",
		copy_equal ? "true" : "false",
		out_read_equal ? "true" : "false",
		query_jumble_equal ? "true" : "false",
		mapping_changes_jumble ? "true" : "false",
		dependency_changes_jumble ? "true" : "false",
		generation_ignored ? "true" : "false",
		(long long) query_id_a, (long long) query_id_b,
		(long long) query_id_generation, strlen(serialized));
	PG_RETURN_TEXT_P(cstring_to_text(result));
}

static int estimate_calls;
static int begin_calls;

static bool
test_estimate(const GgcEstimateRequestV2 *request, GgcEstimateV2 *estimate)
{
	estimate_calls++;
	MemSet(estimate, 0, sizeof(*estimate));
	estimate->logical_edges = request->bindings;
	return true;
}

static GgcAdjScanV2 *
test_begin(const GgcOpenRequestV2 *request)
{
	begin_calls++;
	return (GgcAdjScanV2 *) request;
}

Datum
test_graph_provider_contract(PG_FUNCTION_ARGS)
{
	GgcProviderRef provider = {0};
	GgcProviderRef other = {0};
	GgcEndpointHandleV2 handle = {0};
	GgcBatchBudgetV2 budget = {
		.max_endpoints = 32,
		.max_candidates = 4096,
		.max_inflight_pages = 64,
		.max_pins = 16,
		.max_candidate_bytes = 1024 * 1024,
		.max_restore_bytes = 512 * 1024,
		.preserve_order = true
	};
	GgcEstimateRequestV2 estimate_request = {0};
	GgcEstimateV2 estimate = {0};
	GgcAdjProviderV2 callbacks = {0};
	bool		scope_match;
	bool		provider_mismatch;
	bool		build_mismatch;
	bool		domain_mismatch;
	bool		budget_bounded;
	bool		zero_budget_rejected;
	bool		abi_size_contract;
	bool		estimate_only;
	char	   *result;

	provider.struct_size = sizeof(GgcProviderRef);
	provider.abi_version = GGC_ADJ_ABI_VERSION;
	provider.owner_extension_oid = InvalidOid;
	provider.kind = 4;
	namestrcpy(&provider.name, "graph_adj");
	other = provider;
	namestrcpy(&other.name, "btree_adj");
	handle.provider = provider;
	handle.index_relid = 9201;
	handle.build_generation = 9;
	handle.endpoint_domain = 77;
	handle.slot_id = 123;
	handle.slot_generation = 4;
	handle.origin = GGC_HANDLE_PERSISTED_ADJACENCY;

	scope_match = GgcEndpointHandleScopeMatches(&handle, &provider, 9201, 9, 77);
	provider_mismatch = !GgcEndpointHandleScopeMatches(&handle, &other, 9201, 9, 77);
	build_mismatch = !GgcEndpointHandleScopeMatches(&handle, &provider, 9201, 10, 77);
	domain_mismatch = !GgcEndpointHandleScopeMatches(&handle, &provider, 9201, 9, 78);
	budget_bounded = GgcBatchBudgetIsBounded(&budget);
	budget.max_pins = 0;
	zero_budget_rejected = !GgcBatchBudgetIsBounded(&budget);

	callbacks.struct_size = sizeof(GgcAdjProviderV2);
	callbacks.abi_version = GGC_ADJ_ABI_VERSION;
	callbacks.identity = provider;
	callbacks.estimate = test_estimate;
	callbacks.begin_batch = test_begin;
	abi_size_contract = provider.struct_size == sizeof(GgcProviderRef) &&
		callbacks.struct_size == sizeof(GgcAdjProviderV2) &&
		callbacks.abi_version == GGC_ADJ_ABI_VERSION;
	estimate_request.struct_size = sizeof(GgcEstimateRequestV2);
	estimate_request.abi_version = GGC_ADJ_ABI_VERSION;
	estimate_request.bindings = 12.0;
	estimate_calls = 0;
	begin_calls = 0;
	(void) callbacks.estimate(&estimate_request, &estimate);
	estimate_only = estimate_calls == 1 && begin_calls == 0 &&
		estimate.logical_edges == 12.0;

	result = psprintf(
		"{\"adj_abi_version\":%u,\"catalog_abi_version\":%u,"
		"\"result_abi_version\":%u,\"provider_ref_size\":%zu,"
		"\"handle_size\":%zu,\"open_request_size\":%zu,"
		"\"estimate_request_size\":%zu,\"catalog_provider_size\":%zu,"
		"\"result_adapter_size\":%zu,\"scope_match\":%s,"
		"\"provider_mismatch_rejected\":%s,\"build_mismatch_rejected\":%s,"
		"\"domain_mismatch_rejected\":%s,\"persisted_origin\":%u,"
		"\"budget_bounded\":%s,\"zero_budget_rejected\":%s,"
		"\"abi_size_contract\":%s,"
		"\"estimate_only\":%s,\"estimate_calls\":%d,\"begin_calls\":%d}",
		GGC_ADJ_ABI_VERSION, GGC_CATALOG_ABI_VERSION,
		GGC_RESULT_ABI_VERSION, sizeof(GgcProviderRef),
		sizeof(GgcEndpointHandleV2), sizeof(GgcOpenRequestV2),
		sizeof(GgcEstimateRequestV2), sizeof(GgcCatalogProviderV1),
		sizeof(GgcResultAdapterV1), scope_match ? "true" : "false",
		provider_mismatch ? "true" : "false",
		build_mismatch ? "true" : "false",
		domain_mismatch ? "true" : "false", handle.origin,
		budget_bounded ? "true" : "false",
		zero_budget_rejected ? "true" : "false",
		abi_size_contract ? "true" : "false",
		estimate_only ? "true" : "false", estimate_calls, begin_calls);
	PG_RETURN_TEXT_P(cstring_to_text(result));
}
