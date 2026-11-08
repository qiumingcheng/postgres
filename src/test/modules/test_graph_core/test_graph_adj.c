/* SQL diagnostic for graph_adj through the public bounded batch layer. */
#include "postgres.h"

#include "access/genam.h"
#include "access/graphadj.h"
#include "access/xact.h"
#include "catalog/pg_type_d.h"
#include "executor/graph_adj_provider.h"
#include "executor/graph_batch.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

PG_FUNCTION_INFO_V1(test_graph_adj_walk);

typedef struct TestGraphAdjBinding
{
	uint64		ordinal;
	int64		key;
	GgcEndpointHandleV2 handle;
} TestGraphAdjBinding;

typedef struct TestGraphAdjVector
{
	TestGraphAdjBinding *items;
	uint32		count;
	uint32		capacity;
} TestGraphAdjVector;

static void
append_binding(TestGraphAdjVector *vector, uint64 ordinal, int64 key,
			   const GgcEndpointHandleV2 *handle)
{
	TestGraphAdjBinding *binding;

	if (vector->count == vector->capacity)
	{
		vector->capacity = vector->capacity == 0 ? 32 : vector->capacity * 2;
		if (vector->items == NULL)
			vector->items = palloc(sizeof(*vector->items) * vector->capacity);
		else
			vector->items = repalloc(vector->items,
				sizeof(*vector->items) * vector->capacity);
	}
	binding = &vector->items[vector->count++];
	binding->ordinal = ordinal;
	binding->key = key;
	if (handle)
		binding->handle = *handle;
	else
		MemSet(&binding->handle, 0, sizeof(binding->handle));
}

static void
accumulate_instrumentation(GgcBatchInstrumentation *target,
						   const GgcBatchInstrumentation *source)
{
	target->parent_bindings += source->parent_bindings;
	target->endpoint_requests += source->endpoint_requests;
	target->shared_parent_requests += source->shared_parent_requests;
	target->endpoint_batches += source->endpoint_batches;
	target->candidate_batches += source->candidate_batches;
	target->candidates_received += source->candidates_received;
	target->candidate_bytes += source->candidate_bytes;
	target->heap_page_groups += source->heap_page_groups;
	target->prefetch_submitted += source->prefetch_submitted;
	target->prefetch_already_cached += source->prefetch_already_cached;
	target->prefetch_io_started += source->prefetch_io_started;
	target->tableam_fetches += source->tableam_fetches;
	target->visibility_rejects += source->visibility_rejects;
	target->visibility_reuses += source->visibility_reuses;
	target->hot_redirects += source->hot_redirects;
	target->restored_bindings += source->restored_bindings;
	target->shared_blks_hit += source->shared_blks_hit;
	target->shared_blks_read += source->shared_blks_read;
	target->provider_directory_lookups +=
		source->provider_directory_lookups;
	target->provider_ref_direct_lookups +=
		source->provider_ref_direct_lookups;
	target->provider_ref_fallbacks += source->provider_ref_fallbacks;
	target->provider_slot_dereferences +=
		source->provider_slot_dereferences;
	target->provider_physical_candidates +=
		source->provider_physical_candidates;
	target->peak_candidate_count = Max(target->peak_candidate_count,
		source->peak_candidate_count);
	target->peak_candidate_bytes = Max(target->peak_candidate_bytes,
		source->peak_candidate_bytes);
	target->peak_inflight_pages = Max(target->peak_inflight_pages,
		source->peak_inflight_pages);
	target->peak_pins = Max(target->peak_pins, source->peak_pins);
	target->peak_restore_bytes = Max(target->peak_restore_bytes,
		source->peak_restore_bytes);
}

Datum
test_graph_adj_walk(PG_FUNCTION_ARGS)
{
	Oid			edge_oid = PG_GETARG_OID(0);
	Oid			index_oid = PG_GETARG_OID(1);
	ArrayType  *start_array = PG_GETARG_ARRAYTYPE_P(2);
	int32		hops = PG_GETARG_INT32(3);
	int32		direction = PG_GETARG_INT32(4);
	int32		handle_mode = PG_GETARG_INT32(5);
	Datum	   *keys;
	bool	   *nulls;
	int			nkeys;
	EState	   *estate;
	Snapshot	snapshot;
	GgcOpenRequestV2 request = {0};
	GgcBatchOptions options = {0};
	GgcBatchInstrumentation total = {0};
	TestGraphAdjVector frontier = {0};
	Relation	index;
	Oid			opfamily;
	uint64		next_ordinal = 1;
	StringInfoData result;

	if (hops < 1 || hops > 8 ||
		(direction != GGC_OUT && direction != GGC_IN) ||
		handle_mode < 0 || handle_mode > 2)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid graph_adj walk diagnostic options")));
	deconstruct_array(start_array, INT8OID, sizeof(int64),
		FLOAT8PASSBYVAL, TYPALIGN_DOUBLE, &keys, &nulls, &nkeys);
	if (nkeys <= 0 || nkeys > 1024)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("graph_adj walk requires a bounded nonempty start array")));
	for (int i = 0; i < nkeys; i++)
	{
		if (nulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("null graph_adj walk start key is unsupported")));
		append_binding(&frontier, next_ordinal++, DatumGetInt64(keys[i]), NULL);
	}

	index = index_open(index_oid, AccessShareLock);
	if (!index->rd_index || index->rd_index->indrelid != edge_oid ||
		index->rd_index->indnkeyatts != 2)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("graph_adj walk index does not match its edge relation")));
	opfamily = index->rd_opfamily[direction == GGC_OUT ? 0 : 1];
	index_close(index, AccessShareLock);

	estate = CreateExecutorState();
	snapshot = RegisterSnapshot(GetActiveSnapshot());
	estate->es_snapshot = snapshot;
	request.struct_size = sizeof(request);
	request.abi_version = GGC_ADJ_ABI_VERSION;
	request.estate = estate;
	request.scan_context = estate->es_query_cxt;
	request.resource_owner = CurrentResourceOwner;
	request.snapshot = snapshot;
	request.command_id = GetCurrentCommandId(false);
	GetUserIdAndSecContext(&request.effective_role, &request.security_context);
	request.edge_relid = edge_oid;
	request.index_relid = index_oid;
	request.source_attnum = 2;
	request.destination_attnum = 3;
	request.endpoint_type = INT8OID;
	request.endpoint_opfamily = opfamily;
	request.endpoint_domain = edge_oid;
	request.direction = direction;
	request.required_caps = GGC_CAP_ROOT_TID | GGC_CAP_COMPLETE_CANDIDATES |
		GGC_CAP_BATCH | GGC_CAP_PAUSE_RESUME | GGC_CAP_SSI |
		GGC_CAP_HANDLE_INPUT | GGC_CAP_HANDLE_OUTPUT;
	request.build_generation = GRAPHADJ_BUILD_GENERATION;
	request.budget.max_endpoints = 64;
	request.budget.max_candidates = 256;
	request.budget.max_inflight_pages = 64;
	request.budget.max_pins = 8;
	request.budget.max_candidate_bytes = 1024 * 1024;
	request.budget.max_restore_bytes = 4 * 1024 * 1024;
	request.budget.preserve_order = true;
	options.struct_size = sizeof(options);
	options.version = GGC_BATCH_EXEC_VERSION;
	options.prefetch = true;
	options.small_parent_threshold = 2;
	options.small_candidate_limit = 16;

	for (int32 hop = 0; hop < hops && frontier.count > 0; hop++)
	{
		GgcBatchParent *parents = palloc0(sizeof(*parents) * frontier.count);
		GgcBatchState *state;
		GgcBatchResult row;
		GgcBatchInstrumentation stage = {0};
		TestGraphAdjVector next = {0};

		for (uint32 i = 0; i < frontier.count; i++)
		{
			parents[i].parent_id = i + 1;
			parents[i].parent_ordinal = frontier.items[i].ordinal;
			parents[i].key = Int64GetDatum(frontier.items[i].key);
			parents[i].handle = frontier.items[i].handle;
			if (hop > 0 && handle_mode == 1)
			{
				parents[i].handle.provider.kind++;
				namestrcpy(&parents[i].handle.provider.name, "foreign_adj");
			}
			else if (hop > 0 && handle_mode == 2)
				parents[i].handle.slot_generation++;
		}
		state = GgcBatchBegin(GetGgcGraphAdjProvider(), &request,
			parents, frontier.count, &options);
		while (GgcBatchNext(state, &row))
		{
			if (!row.candidate.opposite_key_available ||
				row.candidate.opposite_isnull ||
				row.candidate.opposite_handle.origin !=
				GGC_HANDLE_PERSISTED_ADJACENCY)
				ereport(ERROR,
						(errcode(ERRCODE_DATA_CORRUPTED),
						 errmsg("graph_adj provider returned an incomplete binding")));
			if (next.count >= 100000)
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("graph_adj walk diagnostic output limit exceeded")));
			append_binding(&next, next_ordinal++,
				DatumGetInt64(row.candidate.opposite_key),
				&row.candidate.opposite_handle);
		}
		GgcBatchGetInstrumentation(state, &stage);
		accumulate_instrumentation(&total, &stage);
		GgcBatchEnd(state);
		pfree(parents);
		pfree(frontier.items);
		frontier = next;
	}

	initStringInfo(&result);
	appendStringInfo(&result,
		"{\"provider\":\"%s\",\"hops\":%d,\"handle_mode\":%d,"
		"\"final_keys\":[",
		NameStr(GetGgcGraphAdjProvider()->identity.name), hops, handle_mode);
	for (uint32 i = 0; i < frontier.count; i++)
	{
		if (i > 0)
			appendStringInfoChar(&result, ',');
		appendStringInfo(&result, "%lld", (long long) frontier.items[i].key);
	}
	appendStringInfo(&result,
		"],\"final_binding_count\":%u,\"parent_bindings\":%llu,"
		"\"endpoint_requests\":%llu,\"shared_parent_requests\":%llu,"
		"\"candidate_batches\":%llu,\"physical_candidates\":%llu,"
		"\"tableam_fetches\":%llu,\"visibility_rejects\":%llu,"
		"\"hot_redirects\":%llu,"
		"\"restored_bindings\":%llu,\"heap_page_groups\":%llu,"
		"\"directory_lookups\":%llu,\"ref_direct_lookups\":%llu,"
		"\"ref_fallbacks\":%llu,\"slot_dereferences\":%llu,"
		"\"provider_physical_candidates\":%llu,"
		"\"peak_candidate_count\":%u,\"peak_restore_bytes\":%zu}",
		frontier.count,
		(unsigned long long) total.parent_bindings,
		(unsigned long long) total.endpoint_requests,
		(unsigned long long) total.shared_parent_requests,
		(unsigned long long) total.candidate_batches,
		(unsigned long long) total.candidates_received,
		(unsigned long long) total.tableam_fetches,
		(unsigned long long) total.visibility_rejects,
		(unsigned long long) total.hot_redirects,
		(unsigned long long) total.restored_bindings,
		(unsigned long long) total.heap_page_groups,
		(unsigned long long) total.provider_directory_lookups,
		(unsigned long long) total.provider_ref_direct_lookups,
		(unsigned long long) total.provider_ref_fallbacks,
		(unsigned long long) total.provider_slot_dereferences,
		(unsigned long long) total.provider_physical_candidates,
		total.peak_candidate_count, total.peak_restore_bytes);

	pfree(frontier.items);
	UnregisterSnapshot(snapshot);
	FreeExecutorState(estate);
	PG_RETURN_TEXT_P(cstring_to_text(result.data));
}
