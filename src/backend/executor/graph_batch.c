/*-------------------------------------------------------------------------
 * graph_batch.c
 *
 * Common two-level bounded graph batching.  Providers produce physical
 * candidates; this layer groups a bounded prefix by real heap page, performs
 * optional bounded prefetch, follows HOT/MVCC through Table AM, and restores
 * every parent binding.  It deliberately does not evaluate per-parent quals.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type_d.h"
#include "executor/graph_batch.h"
#include "executor/graph_access.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/timestamp.h"

typedef struct GgcOwnedParent
{
	/* Endpoint key/handle belong to the deduplicated request. Restoration
	 * needs only binding identity/order and the reference to that request. */
	uint64		parent_id;
	uint64		parent_ordinal;
	uint32		request_id;
} GgcOwnedParent;

typedef struct GgcCandidateOrder
{
	uint32		candidate_index;
	Oid			relation_oid;
	BlockNumber block;
	OffsetNumber offset;
} GgcCandidateOrder;

typedef struct GgcFetchedCandidate
{
	GgcBatchTupleView view;
	uint32		next_for_request;
} GgcFetchedCandidate;

typedef struct GgcRestoreRow
{
	uint32		fetched_index;
	uint32		parent_index;
	uint64		parent_ordinal;
	uint64		candidate_ordinal;
} GgcRestoreRow;

typedef struct GgcRelationState
{
	Oid			relation_oid;
	Relation	relation;
	IndexFetchTableData *fetch;
	TupleTableSlot *fetch_slot;
	TupleTableSlot *output_slot;
} GgcRelationState;

struct GgcBatchState
{
	MemoryContext context;
	MemoryContext request_context;
	MemoryContext candidate_context;
	MemoryContext restore_context;
	MemoryContext relation_context;
	MemoryContext provider_context;
	const GgcAdjProviderV2 *provider;
	GgcAdjScanV2 *provider_scan;
	GgcOpenRequestV2 request_template;
	GgcOpenRequestV2 provider_request;
	GgcBatchOptions options;
	GgcOwnedParent *parents;
	uint32		parent_count;
	GgcEndpointRequestV2 *requests;
	uint32	   *request_parent_counts;
	uint32	   *request_parent_offsets;
	uint32	   *request_parent_indexes;
	uint32		request_count;
	uint32		request_batch_start;
	uint32		request_batch_count;
	GgcAdjItemV2 *candidates;
	uint32		candidate_capacity;
	uint32		candidate_count;
	uint32		candidate_pos;
	GgcFetchedCandidate *fetched;
	uint32		fetched_count;
	GgcRestoreRow *restore_rows;
	uint32		restore_count;
	uint32		restore_pos;
	uint32	   *restore_request_heads;
	uint32		restore_parent;
	uint32		restore_link;
	uint64		restore_generation;
	GgcRelationState *relations;
	uint32		relation_count;
	uint32		relation_capacity;
	Size		persistent_restore_bytes;
	bool		provider_eof;
	bool		first_row_returned;
	bool		ordered_parents;
	bool		linear_restore;
	bool		timing;
	bool		destroyed;
	TimestampTz started_at;
	GgcBatchInstrumentation instrumentation;
	GgcProviderInstrumentation provider_baseline;
	const GgcIndexLeaseOps *lease_ops;
	TupleTableSlot *lease_slot;
	Oid lease_relation;
	uint64 lease_groups;
};

static void destroy_state(GgcBatchState *state);

static double
elapsed_microseconds(instr_time started)
{
	instr_time	finished;

	INSTR_TIME_SET_CURRENT_FAST(finished);
	INSTR_TIME_SUBTRACT(finished, started);
	return INSTR_TIME_GET_MILLISEC(finished) * 1000.0;
}

static bool
handle_equal(const GgcEndpointHandleV2 *left,
			 const GgcEndpointHandleV2 *right)
{
	if (left->origin != right->origin)
		return false;
	if (left->origin == GGC_HANDLE_NONE)
		return true;
	return GgcProviderRefEqual(&left->provider, &right->provider) &&
		left->index_relid == right->index_relid &&
		left->build_generation == right->build_generation &&
		left->endpoint_domain == right->endpoint_domain &&
		left->slot_id == right->slot_id &&
		left->slot_generation == right->slot_generation;
}

typedef struct GgcParentSortContext
{
	const GgcBatchParent *parents;
	uint32		comparisons;
} GgcParentSortContext;

/* Sort equal requests together, with the first input parent first in each
 * group. Compare the same fields as handle_equal(), never struct padding or
 * bytes after a provider name's terminating NUL. An invalid provider size
 * cannot compare equal even to itself, so keep such requests independent. */
static int
parent_request_compare(const void *left, const void *right, void *arg)
{
	uint32		li = *(const uint32 *) left;
	uint32		ri = *(const uint32 *) right;
	GgcParentSortContext *context = arg;
	const GgcBatchParent *lp = &context->parents[li];
	const GgcBatchParent *rp = &context->parents[ri];
	const GgcEndpointHandleV2 *a = &lp->handle;
	const GgcEndpointHandleV2 *b = &rp->handle;
	int64		lk = DatumGetInt64(lp->key);
	int64		rk = DatumGetInt64(rp->key);
	bool		av;
	bool		bv;
	int			cmp;

	if ((++context->comparisons & 1023) == 0)
		CHECK_FOR_INTERRUPTS();
	if (lk != rk)
		return lk < rk ? -1 : 1;
	if (a->origin != b->origin)
		return a->origin < b->origin ? -1 : 1;
	if (a->origin == GGC_HANDLE_NONE)
		goto input_order;
	av = a->provider.struct_size == sizeof(GgcProviderRef);
	bv = b->provider.struct_size == sizeof(GgcProviderRef);
	if (av != bv)
		return av ? -1 : 1;
	if (!av)
		goto input_order;
	if (a->provider.abi_version != b->provider.abi_version)
		return a->provider.abi_version < b->provider.abi_version ? -1 : 1;
	if (a->provider.owner_extension_oid != b->provider.owner_extension_oid)
		return a->provider.owner_extension_oid < b->provider.owner_extension_oid ? -1 : 1;
	if (a->provider.kind != b->provider.kind)
		return a->provider.kind < b->provider.kind ? -1 : 1;
	cmp = strncmp(NameStr(a->provider.name), NameStr(b->provider.name), NAMEDATALEN);
	if (cmp)
		return cmp;
	if (a->index_relid != b->index_relid)
		return a->index_relid < b->index_relid ? -1 : 1;
	if (a->build_generation != b->build_generation)
		return a->build_generation < b->build_generation ? -1 : 1;
	if (a->endpoint_domain != b->endpoint_domain)
		return a->endpoint_domain < b->endpoint_domain ? -1 : 1;
	if (a->slot_id != b->slot_id)
		return a->slot_id < b->slot_id ? -1 : 1;
	if (a->slot_generation != b->slot_generation)
		return a->slot_generation < b->slot_generation ? -1 : 1;

input_order:
	return li < ri ? -1 : (li > ri ? 1 : 0);
}

static int
candidate_order_compare(const void *left, const void *right)
{
	const GgcCandidateOrder *a = left;
	const GgcCandidateOrder *b = right;

	if (a->relation_oid != b->relation_oid)
		return a->relation_oid < b->relation_oid ? -1 : 1;
	if (a->block != b->block)
		return a->block < b->block ? -1 : 1;
	if (a->offset != b->offset)
		return a->offset < b->offset ? -1 : 1;
	return a->candidate_index < b->candidate_index ? -1 :
		(a->candidate_index > b->candidate_index ? 1 : 0);
}

static int
restore_row_compare(const void *left, const void *right)
{
	const GgcRestoreRow *a = left;
	const GgcRestoreRow *b = right;

	if (a->parent_ordinal != b->parent_ordinal)
		return a->parent_ordinal < b->parent_ordinal ? -1 : 1;
	if (a->candidate_ordinal != b->candidate_ordinal)
		return a->candidate_ordinal < b->candidate_ordinal ? -1 : 1;
	return a->parent_index < b->parent_index ? -1 :
		(a->parent_index > b->parent_index ? 1 : 0);
}

static int
fetched_order_compare(const void *left, const void *right, void *arg)
{
	const GgcCandidateOrder *a = left;
	const GgcCandidateOrder *b = right;
	const GgcBatchState *state = arg;
	uint64 a_ordinal = state->fetched[a->candidate_index].view.candidate.original_ordinal;
	uint64 b_ordinal = state->fetched[b->candidate_index].view.candidate.original_ordinal;

	return a_ordinal < b_ordinal ? -1 : (a_ordinal > b_ordinal ? 1 : 0);
}

static void
validate_common_request(const GgcAdjProviderV2 *provider,
						const GgcOpenRequestV2 *request,
						const GgcBatchOptions *options,
						uint32 parent_count)
{
	Size		candidate_capacity;

	if (!provider || provider->struct_size != sizeof(*provider) ||
		provider->abi_version != GGC_ADJ_ABI_VERSION ||
		!provider->validate || !provider->begin_batch ||
		!provider->getnext_batch || !provider->rescan || !provider->end)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("invalid graph adjacency provider ABI")));
	if ((provider->capabilities & (GGC_CAP_BATCH | GGC_CAP_ROOT_TID |
		 GGC_CAP_COMPLETE_CANDIDATES | GGC_CAP_RESCAN)) !=
		(GGC_CAP_BATCH | GGC_CAP_ROOT_TID |
		 GGC_CAP_COMPLETE_CANDIDATES | GGC_CAP_RESCAN))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("graph provider lacks bounded fetch capabilities")));
	if (!request || request->struct_size != sizeof(*request) ||
		request->abi_version != GGC_ADJ_ABI_VERSION ||
		!GgcBatchBudgetIsBounded(&request->budget) ||
		request->endpoint_type != INT8OID || parent_count == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid bounded graph batch request")));
	if (!options || options->struct_size != sizeof(*options) ||
		options->version != GGC_BATCH_EXEC_VERSION ||
		(options->small_parent_threshold > 0 &&
		 options->small_candidate_limit == 0))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid graph batch options")));
	candidate_capacity = Min((Size) request->budget.max_candidates,
		request->budget.max_candidate_bytes / sizeof(GgcAdjItemV2));
	if (candidate_capacity == 0 || candidate_capacity > PG_INT32_MAX ||
		!AllocSizeIsValid(candidate_capacity * sizeof(GgcAdjItemV2)) ||
		parent_count > PG_UINT32_MAX / 2 ||
		!AllocSizeIsValid((Size) parent_count * sizeof(GgcOwnedParent)))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("graph batch dimensions exceed allocation limits")));
}

static void
build_request_mapping(GgcBatchState *state,
					  const GgcBatchParent *parents, uint32 parent_count)
{
	MemoryContext old;
	Size		base_bytes;
	bool		sort_requests = parent_count > 64;
	Size		sort_bytes = sort_requests ?
		mul_size((Size) parent_count, sizeof(uint32)) : 0;

	MemoryContextReset(state->request_context);
	/* Charge allocated capacity, not the smaller number of distinct endpoints
	 * discovered later. Reject before allocating an over-budget parent map. */
	base_bytes = mul_size((Size) parent_count,
		sizeof(GgcOwnedParent) + sizeof(GgcEndpointRequestV2) + sizeof(uint32));
	if (add_size(base_bytes, sort_bytes) >=
		state->request_template.budget.max_restore_bytes)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("graph parent restore map exceeds byte budget")));
	old = MemoryContextSwitchTo(state->request_context);
	state->parents = palloc(sizeof(*state->parents) * parent_count);
	state->requests = palloc0(sizeof(*state->requests) * parent_count);
	state->request_parent_counts = palloc0(sizeof(uint32) * parent_count);
	state->parent_count = parent_count;
	state->request_count = 0;
	state->ordered_parents = true;
	state->request_parent_offsets = NULL;
	state->request_parent_indexes = NULL;

	/* The usual small parent batch needs no sorting workspace. For larger
	 * batches, find representatives with one sort, then build requests in
	 * original input order below. Thus neither request IDs nor parent order
	 * depend on the sorting implementation. Only the temporary index array
	 * is allocated; the not-yet-published parent map holds representatives. */
	if (sort_requests)
	{
		GgcParentSortContext context = {parents, 0};
		uint32	   *order = palloc(sort_bytes);
		uint32		representative = 0;

		for (uint32 i = 0; i < parent_count; i++)
		{
			if ((i & 1023) == 0)
				CHECK_FOR_INTERRUPTS();
			order[i] = i;
		}
		qsort_arg(order, parent_count, sizeof(*order),
				  parent_request_compare, &context);
		for (uint32 i = 0; i < parent_count; i++)
		{
			uint32		p = order[i];

			if ((i & 1023) == 0)
				CHECK_FOR_INTERRUPTS();
			if (i == 0 ||
				DatumGetInt64(parents[p].key) != DatumGetInt64(parents[representative].key) ||
				!handle_equal(&parents[p].handle, &parents[representative].handle))
				representative = p;
			state->parents[p].request_id = representative + 1;
		}
		state->instrumentation.peak_restore_bytes = Max(
			state->instrumentation.peak_restore_bytes, base_bytes + sort_bytes);
		pfree(order);
	}

	for (uint32 i = 0; i < parent_count; i++)
	{
		uint32		request_index = state->request_count;
		int64		key = DatumGetInt64(parents[i].key);

		if ((i & 1023) == 0)
			CHECK_FOR_INTERRUPTS();
		if (i > 0 && parents[i - 1].parent_ordinal >= parents[i].parent_ordinal)
			state->ordered_parents = false;

		if (sort_requests)
		{
			uint32		representative = state->parents[i].request_id - 1;

			Assert(representative <= i);
			if (representative < i)
				request_index = state->parents[representative].request_id - 1;
		}
		else
			for (request_index = 0; request_index < state->request_count;
				 request_index++)
				if (DatumGetInt64(state->requests[request_index].key) == key &&
					handle_equal(&state->requests[request_index].handle,
								 &parents[i].handle))
					break;
		if (request_index == state->request_count)
		{
			if (state->request_count == PG_UINT32_MAX)
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("graph endpoint request ID overflow")));
			state->requests[request_index].request_id = request_index + 1;
			state->requests[request_index].key = Int64GetDatum(key);
			state->requests[request_index].handle = parents[i].handle;
			state->request_count++;
		}
		state->request_parent_counts[request_index]++;
		state->parents[i].parent_id = parents[i].parent_id;
		state->parents[i].parent_ordinal = parents[i].parent_ordinal;
		state->parents[i].request_id = request_index + 1;
	}
	/* The ordered iterator only needs parents -> request. Build the reverse
	 * map for the generic (including duplicate ordinal) ordering path only. */
	if (!state->request_template.budget.preserve_order || !state->ordered_parents)
	{
		uint32	   *write_positions;
		Size		temporary_bytes = (Size) state->request_count * sizeof(uint32);

		base_bytes = add_size(base_bytes,
			mul_size((Size) state->request_count + 1 + parent_count, sizeof(uint32)));
		if (add_size(base_bytes, temporary_bytes) >=
			state->request_template.budget.max_restore_bytes)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("graph parent restore map exceeds byte budget")));
		state->request_parent_offsets =
			palloc(sizeof(uint32) * (state->request_count + 1));
		state->request_parent_indexes = palloc(sizeof(uint32) * parent_count);
		write_positions = palloc(temporary_bytes);
		state->request_parent_offsets[0] = 0;
		for (uint32 i = 0; i < state->request_count; i++)
		{
			if ((i & 1023) == 0)
				CHECK_FOR_INTERRUPTS();
			state->request_parent_offsets[i + 1] =
				state->request_parent_offsets[i] + state->request_parent_counts[i];
		}
		memcpy(write_positions, state->request_parent_offsets,
			   sizeof(uint32) * state->request_count);
		for (uint32 i = 0; i < parent_count; i++)
		{
			uint32		request_index = state->parents[i].request_id - 1;

			if ((i & 1023) == 0)
				CHECK_FOR_INTERRUPTS();
			state->request_parent_indexes[write_positions[request_index]++] = i;
		}
		pfree(write_positions);
		state->instrumentation.peak_restore_bytes = Max(
			state->instrumentation.peak_restore_bytes, base_bytes + temporary_bytes);
	}
	MemoryContextSwitchTo(old);

	state->persistent_restore_bytes = base_bytes;
	state->instrumentation.peak_restore_bytes = Max(
		state->instrumentation.peak_restore_bytes, base_bytes);
	state->instrumentation.parent_bindings += parent_count;
	state->instrumentation.endpoint_requests += state->request_count;
	state->instrumentation.shared_parent_requests +=
		parent_count - state->request_count;
}

static void
set_provider_request(GgcBatchState *state, uint32 start)
{
	uint32		remaining = state->request_count - start;

	state->request_batch_start = start;
	state->request_batch_count = Min(remaining,
		state->request_template.budget.max_endpoints);
	state->provider_request = state->request_template;
	state->provider_request.scan_context = state->provider_context;
	state->provider_request.requests = state->requests + start;
	state->provider_request.request_count = state->request_batch_count;
}

static void
start_provider(GgcBatchState *state)
{
	GgcRejectReason reason = GGC_ACCEPT;
	instr_time	started;

	if (state->timing)
		INSTR_TIME_SET_CURRENT_FAST(started);
	set_provider_request(state, 0);
	if (!state->provider->validate(&state->provider_request, &reason))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("graph provider rejected endpoint batch (%d)", reason)));
	state->provider_scan = state->provider->begin_batch(&state->provider_request);
	if (state->timing)
		state->instrumentation.batch_setup_us += elapsed_microseconds(started);
	state->instrumentation.endpoint_batches++;
}

static bool
advance_endpoint_batch(GgcBatchState *state)
{
	uint32		next = state->request_batch_start + state->request_batch_count;
	instr_time	started;

	if (next >= state->request_count)
	{
		state->provider_eof = true;
		state->instrumentation.last_reason = GGC_BATCH_REASON_PROVIDER_EOF;
		return false;
	}
	if (state->timing)
		INSTR_TIME_SET_CURRENT_FAST(started);
	set_provider_request(state, next);
	/* The provider rescan callback owns request and immutable-scope checks. */
	state->provider->rescan(state->provider_scan, &state->provider_request);
	if (state->timing)
		state->instrumentation.batch_setup_us += elapsed_microseconds(started);
	state->instrumentation.endpoint_batches++;
	return true;
}

static void
close_relations(GgcBatchState *state)
{
	for (uint32 i = 0; i < state->relation_count; i++)
	{
		GgcRelationState *relation = &state->relations[i];

		if (relation->fetch_slot)
		{
			ExecClearTuple(relation->fetch_slot);
			ExecDropSingleTupleTableSlot(relation->fetch_slot);
		}
		if (relation->output_slot)
		{
			ExecClearTuple(relation->output_slot);
			ExecDropSingleTupleTableSlot(relation->output_slot);
		}
		if (relation->fetch)
			table_index_fetch_end(relation->fetch);
		if (relation->relation)
			table_close(relation->relation, NoLock);
	}
	state->relations = NULL;
	state->relation_count = 0;
	state->relation_capacity = 0;
}

static void
reset_restore_batch(GgcBatchState *state)
{
	if (state->restore_generation == PG_UINT64_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("graph restore generation overflow")));
	state->restore_generation++;
	/* Output slots borrow tuples from the restore arena. */
	for (uint32 i = 0; i < state->relation_count; i++)
		ExecClearTuple(state->relations[i].output_slot);
	MemoryContextReset(state->restore_context);
	state->fetched = NULL;
	state->fetched_count = 0;
	state->restore_rows = NULL;
	state->restore_count = 0;
	state->restore_pos = 0;
	state->restore_request_heads = NULL;
	state->restore_parent = 0;
	state->restore_link = 0;
}

static GgcRelationState *
get_relation_state(GgcBatchState *state, Oid relation_oid)
{
	GgcRelationState *relation;
	MemoryContext old;

	for (uint32 i = 0; i < state->relation_count; i++)
		if (state->relations[i].relation_oid == relation_oid)
			return &state->relations[i];
	if (state->relation_count == state->relation_capacity)
	{
		uint32		capacity = state->relation_capacity ?
			state->relation_capacity * 2 : 1;

		if (capacity < state->relation_capacity ||
			!AllocSizeIsValid((Size) capacity * sizeof(*state->relations)))
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("graph relation-state allocation overflow")));
		old = MemoryContextSwitchTo(state->relation_context);
		if (state->relations)
		{
			state->relations = repalloc(state->relations,
				(Size) capacity * sizeof(*state->relations));
			MemSet(state->relations + state->relation_capacity, 0,
				(Size) (capacity - state->relation_capacity) *
				sizeof(*state->relations));
		}
		else
			state->relations = palloc0(
				(Size) capacity * sizeof(*state->relations));
		MemoryContextSwitchTo(old);
		state->relation_capacity = capacity;
	}
	relation = &state->relations[state->relation_count++];
	relation->relation_oid = relation_oid;
	relation->relation = table_open(relation_oid, AccessShareLock);
	if (relation->relation->rd_rel->relkind != RELKIND_RELATION ||
		relation->relation->rd_rel->relam != HEAP_TABLE_AM_OID)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("graph batch currently requires an ordinary heap relation")));
	relation->fetch = table_index_fetch_begin(relation->relation, SO_NONE);
	relation->fetch_slot = table_slot_create(relation->relation, NULL);
	/* Restored tuples already live in our arena, not in a shared buffer.
	 * A buffer slot would copy the tuple again for every parent binding. */
	relation->output_slot = MakeSingleTupleTableSlot(
		RelationGetDescr(relation->relation), &TTSOpsHeapTuple);
	return relation;
}

static uint32
request_index_for_item(GgcBatchState *state, const GgcAdjItemV2 *item)
{
	uint32		index;

	if (item->request_id == 0 || item->request_id > state->request_count)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("graph provider returned an unknown request ID")));
	index = item->request_id - 1;
	return index;
}

static bool
page_seen(GgcBatchState *state, uint32 begin, uint32 end,
		  Oid relation_oid, BlockNumber block)
{
	for (uint32 i = begin; i < end; i++)
		if (state->candidates[i].edge_relid == relation_oid &&
			ItemPointerGetBlockNumber(&state->candidates[i].root_tid) == block)
			return true;
	return false;
}

static bool
relation_seen(GgcBatchState *state, uint32 begin, uint32 end, Oid relation_oid)
{
	for (uint32 i = begin; i < end; i++)
		if (state->candidates[i].edge_relid == relation_oid)
			return true;
	return false;
}

static bool
load_candidate_batch(GgcBatchState *state)
{
	uint32		capacity = state->candidate_capacity;
	GgcBatchReason reason = GGC_BATCH_REASON_NONE;
	int			count;
	MemoryContext old;
	instr_time	started;

	while (!state->provider_eof)
	{
		MemoryContextReset(state->candidate_context);
		old = MemoryContextSwitchTo(state->candidate_context);
		state->candidates = palloc(sizeof(*state->candidates) *
			state->candidate_capacity);
		MemoryContextSwitchTo(old);

		if (state->options.first_row && !state->first_row_returned)
		{
			capacity = 1;
			reason = GGC_BATCH_REASON_FIRST_ROW;
		}
		else if (state->options.small_parent_threshold > 0 &&
				 state->parent_count <= state->options.small_parent_threshold)
		{
			capacity = Min(capacity, state->options.small_candidate_limit);
			reason = GGC_BATCH_REASON_SMALL_REQUEST;
		}
		if (state->timing)
			INSTR_TIME_SET_CURRENT_FAST(started);
		count = state->provider->getnext_batch(state->provider_scan,
			state->candidates, capacity);
		if (state->timing)
			state->instrumentation.provider_getnext_us +=
				elapsed_microseconds(started);
		if (count < 0 || count > capacity)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("graph provider returned an invalid batch size")));
		if (count == 0)
		{
			if (advance_endpoint_batch(state))
				continue;
			return false;
		}
		state->candidate_count = count;
		state->candidate_pos = 0;
		state->instrumentation.candidate_batches++;
		state->instrumentation.candidates_received += count;
		state->instrumentation.candidate_bytes +=
			(Size) count * sizeof(GgcAdjItemV2);
		state->instrumentation.peak_candidate_count = Max(
			state->instrumentation.peak_candidate_count, (uint32) count);
		state->instrumentation.peak_candidate_bytes = Max(
			state->instrumentation.peak_candidate_bytes,
			(Size) state->candidate_capacity * sizeof(GgcAdjItemV2));
		if (reason == GGC_BATCH_REASON_FIRST_ROW)
			state->instrumentation.first_row_batches++;
		else if (reason == GGC_BATCH_REASON_SMALL_REQUEST)
			state->instrumentation.small_batches++;
		else if ((Size) state->request_template.budget.max_candidates <=
				 state->request_template.budget.max_candidate_bytes /
				 sizeof(GgcAdjItemV2))
			reason = GGC_BATCH_REASON_CANDIDATE_LIMIT;
		else
			reason = GGC_BATCH_REASON_CANDIDATE_BYTES;
		state->instrumentation.last_reason = reason;
		return true;
	}
	return false;
}

bool
GgcBatchEnableIndexLease(GgcBatchState *state)
{
	if (!state || state->destroyed || state->first_row_returned ||
		state->candidate_count || state->provider_eof ||
		state->parent_count != 1 || state->request_count != 1 ||
		state->request_template.budget.preserve_order || state->options.first_row)
		return false;
	state->lease_ops = GgcGetIndexLease(state->provider);
	return state->lease_ops != NULL;
}

uint64
GgcBatchGetIndexLeaseGroups(const GgcBatchState *state)
{
	return state->lease_groups;
}

/* One ownership transfer per bounded group, not per candidate. Normal public
 * index advancement immediately precedes its heap fetch, preserving HOT,
 * SSI and the index AM's dead-tuple hint bookkeeping. No external callbacks
 * are allowed while the provider cursor is borrowed. */
static bool
fill_index_lease(GgcBatchState *state)
{
	MemoryContext caller = CurrentMemoryContext;
	uint32 capacity = state->candidate_capacity;
	uint32 consumed = 0;
	uint32 pages = 0;
	Size bytes;
	Size per_candidate = sizeof(GgcFetchedCandidate) + sizeof(BlockNumber) +
		MAXALIGN(HEAPTUPLESIZE) + MaxHeapTupleSize;
	GgcIndexLease lease = {0};
	BlockNumber *blocks;
	bool eof = false;
	BufferUsage started_usage = pgBufferUsage;
	BufferUsage delta_usage = {0};
	instr_time started;

	reset_restore_batch(state);
	if (state->provider_eof)
		return false;
	if (state->options.small_parent_threshold > 0)
		capacity = Min(capacity, state->options.small_candidate_limit);
	/* Bound even the worst case of a different heap page for every TID. */
	capacity = Min(capacity, state->request_template.budget.max_inflight_pages);
	if (state->timing)
		INSTR_TIME_SET_CURRENT_FAST(started);
	PG_TRY();
	{
		if (!state->lease_ops->begin(state->provider_scan, capacity, false, &lease))
			state->provider_eof = true;
		else
		{
			IndexScanDesc scan = lease.scan;
			MemoryContext ancestor;
			AttrNumber opposite_attnum = state->request_template.direction == GGC_OUT ?
				state->request_template.destination_attnum : state->request_template.source_attnum;

			for (ancestor = lease.am_context; ancestor;
				 ancestor = MemoryContextGetParent(ancestor))
				if (ancestor == state->provider_context)
					break;
			if (!scan || !lease.am_context || !scan->heapRelation ||
				!scan->indexRelation || !lease.capacity || lease.capacity > capacity ||
				lease.ordinal > PG_UINT64_MAX - lease.capacity ||
				lease.request_id != state->requests[0].request_id ||
				RelationGetRelid(scan->heapRelation) != state->request_template.edge_relid ||
				RelationGetRelid(scan->indexRelation) != state->request_template.index_relid ||
				scan->xs_snapshot != state->request_template.snapshot ||
				scan->heapRelation->rd_rel->relam != HEAP_TABLE_AM_OID ||
				scan->heapRelation->rd_rel->relkind != RELKIND_RELATION ||
				!ancestor)
				elog(ERROR, "graph provider returned an invalid index lease");
			if (!state->lease_slot)
			{
				MemoryContextSwitchTo(state->relation_context);
				state->lease_slot = table_slot_create(scan->heapRelation, NULL);
				state->lease_relation = RelationGetRelid(scan->heapRelation);
				MemoryContextSwitchTo(caller);
			}
			bytes = state->persistent_restore_bytes +
				MemoryContextMemAllocated(state->relation_context, true);
			if (bytes >= state->request_template.budget.max_restore_bytes ||
				(state->request_template.budget.max_restore_bytes - bytes) / per_candidate == 0)
				ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
								errmsg("one graph index lease tuple exceeds restore budget")));
			capacity = Min(lease.capacity,
				(state->request_template.budget.max_restore_bytes - bytes) / per_candidate);
			state->fetched = MemoryContextAllocZero(state->restore_context,
				capacity * sizeof(*state->fetched));
			blocks = MemoryContextAlloc(state->restore_context, capacity * sizeof(*blocks));
			bytes += capacity * (sizeof(*state->fetched) + sizeof(*blocks));
			state->lease_groups++;
			MemoryContextSwitchTo(lease.am_context);
			while (consumed < capacity)
			{
				ItemPointer tid;
				ItemPointerData root;
				BlockNumber block;
				bool seen = false;
				bool isnull;
				Datum opposite;
				HeapTuple tuple;
				GgcBatchTupleView *view;

				CHECK_FOR_INTERRUPTS();
				tid = index_getnext_tid(scan, ForwardScanDirection);
				if (!tid)
				{
					eof = true;
					break;
				}
				root = *tid;
				if (!ItemPointerIsValid(&root) || scan->xs_recheck)
					elog(ERROR, "graph index lease requires an exact heap TID");
				block = ItemPointerGetBlockNumber(&root);
				for (uint32 i = 0; i < pages; i++)
					if (blocks[i] == block)
						seen = true;
				if (!seen)
					blocks[pages++] = block;
				consumed++;
				state->instrumentation.tableam_fetches++;
				if (!index_fetch_heap(scan, state->lease_slot))
				{
					if (scan->xs_heap_continue)
						elog(ERROR, "graph index lease requires one MVCC tuple per TID");
					state->instrumentation.visibility_rejects++;
					ExecClearTuple(state->lease_slot);
					continue;
				}
				if (scan->xs_heap_continue)
					elog(ERROR, "graph index lease requires one MVCC tuple per TID");
				opposite = slot_getattr(state->lease_slot, opposite_attnum, &isnull);
				if (isnull)
					elog(ERROR, "visible graph edge has a null opposite endpoint");
				MemoryContextSwitchTo(state->restore_context);
				tuple = ExecCopySlotHeapTuple(state->lease_slot);
				MemoryContextSwitchTo(lease.am_context);
				bytes += MAXALIGN(HEAPTUPLESIZE) + tuple->t_len;
				if (bytes > state->request_template.budget.max_restore_bytes)
					ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
									errmsg("visible graph tuple exceeds index lease budget")));
				view = &state->fetched[state->fetched_count].view;
				view->candidate.request_id = lease.request_id;
				view->candidate.original_ordinal = lease.ordinal + consumed - 1;
				view->candidate.edge_relid = state->request_template.edge_relid;
				view->candidate.root_tid = root;
				view->candidate.opposite_key = Int64GetDatum(GgcIntegerKeyValue(opposite,
					TupleDescAttr(state->lease_slot->tts_tupleDescriptor, opposite_attnum - 1)->atttypid));
				view->candidate.opposite_key_available = true;
				view->visible_tid = state->lease_slot->tts_tid;
				view->tuple = tuple;
				view->tuple_desc = RelationGetDescr(scan->heapRelation);
				view->restore_generation = state->restore_generation;
				view->fetched_index = state->fetched_count++;
				view->candidate_parent_count = 1;
				if (!ItemPointerEquals(&root, &view->visible_tid))
					state->instrumentation.hot_redirects++;
				ExecClearTuple(state->lease_slot);
			}
			MemoryContextSwitchTo(caller);
			state->lease_ops->finish(state->provider_scan, &lease, consumed, eof);
			state->provider_eof = eof;
			state->restore_count = state->fetched_count;
			for (uint32 i = 0; i < state->fetched_count; i++)
				state->fetched[i].view.fetched_count = state->fetched_count;
			if (consumed)
			{
				state->instrumentation.candidate_batches++;
				if (state->options.small_parent_threshold > 0)
					state->instrumentation.small_batches++;
			}
			state->instrumentation.candidates_received += consumed;
			state->instrumentation.candidate_bytes += consumed * sizeof(GgcAdjItemV2);
			state->instrumentation.heap_page_groups += pages;
			state->instrumentation.peak_candidate_count = Max(state->instrumentation.peak_candidate_count, consumed);
			state->instrumentation.peak_candidate_bytes = Max(state->instrumentation.peak_candidate_bytes, capacity * sizeof(GgcAdjItemV2));
			state->instrumentation.peak_inflight_pages = Max(state->instrumentation.peak_inflight_pages, pages);
			/* Conservative index traversal + HOT heap upper bound, not actual pins. */
			state->instrumentation.peak_pins = Max(state->instrumentation.peak_pins, 4U);
			state->instrumentation.peak_restore_bytes = Max(state->instrumentation.peak_restore_bytes, bytes);
		}
	}
	PG_CATCH();
	{
		/* Outer Next cleanup may delete both AM and restore contexts. */
		MemoryContextSwitchTo(caller);
		PG_RE_THROW();
	}
	PG_END_TRY();
	BufferUsageAccumDiff(&delta_usage, &pgBufferUsage, &started_usage);
	state->instrumentation.shared_blks_hit += delta_usage.shared_blks_hit;
	state->instrumentation.shared_blks_read += delta_usage.shared_blks_read;
	/* Fused timing includes provider advancement; do not count it twice. */
	if (state->timing)
		state->instrumentation.heap_restore_us += elapsed_microseconds(started);
	state->instrumentation.last_reason = state->provider_eof ? GGC_BATCH_REASON_PROVIDER_EOF :
		GGC_BATCH_REASON_CANDIDATE_LIMIT;
	return consumed > 0;
}

static bool
fill_restore_batch(GgcBatchState *state)
{
	uint32		begin;
	uint32		end;
	uint32		page_count = 0;
	uint64		max_rows = 0;
	Size		fixed_bytes = 0;
	Size		reserved_tuple_bytes = 0;
	Size		restore_bytes;
	GgcBatchReason stop_reason = GGC_BATCH_REASON_NONE;
	GgcCandidateOrder *order;
	BufferUsage start_usage;
	BufferUsage delta_usage = {0};
	MemoryContext old;
	instr_time	restore_started;
	Oid			last_relation = InvalidOid;
	BlockNumber last_block = InvalidBlockNumber;
	bool		linear_restore = state->request_template.budget.preserve_order &&
		state->ordered_parents;
	uint32	   *request_heads = NULL;

	reset_restore_batch(state);
	for (;;)
	{
		if (state->candidate_pos < state->candidate_count)
			break;
		if (!load_candidate_batch(state))
			return false;
	}

	begin = state->candidate_pos;
	end = begin;
	if (linear_restore)
		fixed_bytes = (Size) state->request_count * sizeof(uint32);
	while (end < state->candidate_count)
	{
		GgcAdjItemV2 *item = &state->candidates[end];
		uint32		request_index;
		BlockNumber block;
		bool		new_page;
		bool		new_relation;
		Size		added;
		Size		deformed_bytes = 0;

		if (!OidIsValid(item->edge_relid) ||
			!ItemPointerIsValid(&item->root_tid))
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("graph provider returned an invalid heap candidate")));
		if (item->edge_relid != state->request_template.edge_relid)
			ereport(ERROR,
					(errcode(ERRCODE_DATA_CORRUPTED),
					 errmsg("graph provider returned a candidate for another edge relation")));
		request_index = request_index_for_item(state, item);
		if (state->request_parent_counts[request_index] > 1)
		{
			GgcRelationState *relation = get_relation_state(state, item->edge_relid);

			deformed_bytes = (Size) RelationGetDescr(relation->relation)->natts *
				(sizeof(Datum) + sizeof(bool));
		}
		block = ItemPointerGetBlockNumber(&item->root_tid);
		new_page = !page_seen(state, begin, end, item->edge_relid, block);
		new_relation = !relation_seen(state, begin, end, item->edge_relid);
		if (new_page && page_count >=
			state->request_template.budget.max_inflight_pages)
		{
			stop_reason = GGC_BATCH_REASON_PAGE_LIMIT;
			break;
		}
		added = sizeof(GgcCandidateOrder) + sizeof(GgcFetchedCandidate) +
			(linear_restore ? 0 :
			 (Size) state->request_parent_counts[request_index] *
			 sizeof(GgcRestoreRow)) +
			(new_relation ? sizeof(GgcRelationState) : 0) + deformed_bytes;
		/*
		 * Tuple width is not known until Table AM has followed the HOT chain.
		 * Reserve the largest heap tuple here so a later visible version can
		 * never turn an otherwise accepted batch into an over-budget batch.
		 * This is deliberately conservative; a tighter width estimate belongs
		 * in the adaptive policy, not in this correctness boundary.
		 */
		if (state->persistent_restore_bytes + fixed_bytes + added +
			reserved_tuple_bytes + MAXALIGN(HEAPTUPLESIZE) + MaxHeapTupleSize >
			state->request_template.budget.max_restore_bytes)
		{
			if (end == begin)
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("one graph candidate parent map exceeds restore budget")));
			stop_reason = GGC_BATCH_REASON_RESTORE_BYTES;
			break;
		}
		fixed_bytes += added;
		reserved_tuple_bytes += MAXALIGN(HEAPTUPLESIZE) + MaxHeapTupleSize;
		max_rows += state->request_parent_counts[request_index];
		if (max_rows > PG_UINT32_MAX)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("graph restored binding count overflow")));
		if (new_page)
			page_count++;
		end++;
	}
	Assert(end > begin);
	if (state->timing)
		INSTR_TIME_SET_CURRENT_FAST(restore_started);

	old = MemoryContextSwitchTo(state->restore_context);
	order = palloc(sizeof(*order) * (end - begin));
	state->fetched = palloc0(sizeof(*state->fetched) * (end - begin));
	state->linear_restore = linear_restore;
	if (!linear_restore)
		state->restore_rows = palloc0(sizeof(*state->restore_rows) * max_rows);
	if (linear_restore)
	{
		request_heads = palloc0(sizeof(uint32) * state->request_count);
		state->restore_request_heads = request_heads;
	}
	MemoryContextSwitchTo(old);
	for (uint32 i = begin; i < end; i++)
	{
		uint32		position = i - begin;

		order[position].candidate_index = i;
		order[position].relation_oid = state->candidates[i].edge_relid;
		order[position].block = ItemPointerGetBlockNumber(
			&state->candidates[i].root_tid);
		order[position].offset = ItemPointerGetOffsetNumber(
			&state->candidates[i].root_tid);
	}
	qsort(order, end - begin, sizeof(*order), candidate_order_compare);

	start_usage = pgBufferUsage;
	for (uint32 i = 0; i < end - begin; i++)
	{
		GgcRelationState *relation;
		PrefetchBufferResult prefetch;

		relation = get_relation_state(state, order[i].relation_oid);
		if (!state->options.prefetch ||
			(order[i].relation_oid == last_relation && order[i].block == last_block))
			continue;
		prefetch = PrefetchBuffer(relation->relation, MAIN_FORKNUM,
			order[i].block);
		state->instrumentation.prefetch_submitted++;
		if (BufferIsValid(prefetch.recent_buffer))
			state->instrumentation.prefetch_already_cached++;
		if (prefetch.initiated_io)
			state->instrumentation.prefetch_io_started++;
		last_relation = order[i].relation_oid;
		last_block = order[i].block;
	}

	restore_bytes = state->persistent_restore_bytes + fixed_bytes;
	for (uint32 i = 0; i < end - begin; i++)
	{
		GgcAdjItemV2 *item = &state->candidates[order[i].candidate_index];
		GgcRelationState *relation = get_relation_state(state, item->edge_relid);
		ItemPointerData visible_tid = item->root_tid;
		bool		call_again = false;
		bool		all_dead = false;
		bool		visible;
		bool		found_visible = false;
		HeapTuple	tuple = NULL;
		bool		should_free = false;
		Size		tuple_bytes = 0;

		do
		{
			ExecClearTuple(relation->fetch_slot);
			call_again = false;
			visible = table_index_fetch_tuple(relation->fetch, &visible_tid,
				state->request_template.snapshot, relation->fetch_slot,
				&call_again, &all_dead);
			state->instrumentation.tableam_fetches++;
			if (visible)
			{
				if (found_visible)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("multiple visible tuples for one graph candidate are unsupported")));
				found_visible = true;
				old = MemoryContextSwitchTo(state->restore_context);
				tuple = ExecFetchSlotHeapTuple(relation->fetch_slot, false,
					&should_free);
				tuple_bytes = MAXALIGN(HEAPTUPLESIZE) + tuple->t_len;
				if (restore_bytes + tuple_bytes >
					state->request_template.budget.max_restore_bytes)
				{
					if (should_free)
						pfree(tuple);
					MemoryContextSwitchTo(old);
					ereport(ERROR,
							(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
							 errmsg("visible graph tuple exceeds restore byte budget")));
				}
				if (!should_free)
					tuple = heap_copytuple(tuple);
				MemoryContextSwitchTo(old);
				restore_bytes += tuple_bytes;
			}
		} while (call_again);
		if (!found_visible)
		{
			state->instrumentation.visibility_rejects++;
			ExecClearTuple(relation->fetch_slot);
			continue;
		}

		state->fetched[state->fetched_count].view.candidate = *item;
		state->fetched[state->fetched_count].view.visible_tid =
			relation->fetch_slot->tts_tid;
		state->fetched[state->fetched_count].view.tuple = tuple;
		state->fetched[state->fetched_count].view.tuple_desc = RelationGetDescr(relation->relation);
		if (state->request_parent_counts[item->request_id - 1] > 1)
		{
			GgcFetchedCandidate *fetched = &state->fetched[state->fetched_count];
			TupleDesc descriptor = RelationGetDescr(relation->relation);
			Datum	   *values;
			bool	   *nulls;

			/* Only repeated parent bindings amortize deformation. Datums point
			 * into the owned tuple copy, never into an unpinned heap page. Both
			 * arrays are charged to this bounded restore batch above. */
			values = MemoryContextAlloc(state->restore_context,
				(Size) descriptor->natts * (sizeof(Datum) + sizeof(bool)));
			nulls = (bool *) (values + descriptor->natts);
			heap_deform_tuple(tuple, descriptor, values, nulls);
			fetched->view.values = values;
			fetched->view.nulls = nulls;
		}
		if (!ItemPointerEquals(&item->root_tid,
						   &relation->fetch_slot->tts_tid))
			state->instrumentation.hot_redirects++;
		{
			GgcFetchedCandidate *fetched = &state->fetched[state->fetched_count];
			bool		isnull;
			AttrNumber opposite_attnum = state->request_template.direction == GGC_OUT ?
				state->request_template.destination_attnum :
				state->request_template.source_attnum;
			Datum		opposite;

			/* Shared candidates already own a fully deformed visible tuple.
			 * Reuse it instead of deforming the fetch slot a second time.
			 * Single-parent candidates keep the original lazy slot path. */
			if (fetched->view.values)
			{
				isnull = fetched->view.nulls[opposite_attnum - 1];
				opposite = fetched->view.values[opposite_attnum - 1];
			}
			else
				opposite = slot_getattr(relation->fetch_slot,
					opposite_attnum, &isnull);

			if (isnull)
				ereport(ERROR,
						(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
						 errmsg("visible graph edge has a null opposite endpoint")));
			state->fetched[state->fetched_count].view.candidate.opposite_key =
				Int64GetDatum(GgcIntegerKeyValue(opposite,
					TupleDescAttr(relation->fetch_slot->tts_tupleDescriptor, opposite_attnum - 1)->atttypid));
			state->fetched[state->fetched_count].view.candidate.opposite_isnull = false;
			state->fetched[state->fetched_count].view.candidate.opposite_key_available = true;
		}
		if (linear_restore)
			state->restore_count += state->request_parent_counts[item->request_id - 1];
		else
		{
			uint32		request_index = item->request_id - 1;
			uint32		parent_begin = state->request_parent_offsets[request_index];
			uint32		parent_end = state->request_parent_offsets[request_index + 1];

			for (uint32 position = parent_begin; position < parent_end; position++)
			{
				uint32		p = state->request_parent_indexes[position];
				GgcRestoreRow *row;

				row = &state->restore_rows[state->restore_count++];
				row->fetched_index = state->fetched_count;
				row->parent_index = p;
				row->parent_ordinal = state->parents[p].parent_ordinal;
				row->candidate_ordinal = item->original_ordinal;
			}
		}
		if (state->request_parent_counts[item->request_id - 1] > 1)
			state->instrumentation.visibility_reuses +=
				state->request_parent_counts[item->request_id - 1] - 1;
		state->fetched_count++;
		ExecClearTuple(relation->fetch_slot);
		CHECK_FOR_INTERRUPTS();
	}
	BufferUsageAccumDiff(&delta_usage, &pgBufferUsage, &start_usage);
	state->instrumentation.shared_blks_hit += delta_usage.shared_blks_hit;
	state->instrumentation.shared_blks_read += delta_usage.shared_blks_read;
	if (linear_restore)
	{
		/* Order only the small physical-candidate set. The ordered-parent
		 * iterator in Next follows these links lazily: never allocate the
		 * much larger parent x candidate product. One-based links leave zero
		 * as the end marker. Unordered/duplicate ordinals retain the full map. */
		for (uint32 i = 0; i < state->fetched_count; i++)
			order[i].candidate_index = i;
		qsort_arg(order, state->fetched_count, sizeof(*order),
				  fetched_order_compare, state);
		for (uint32 i = state->fetched_count; i > 0; i--)
		{
			uint32 index = order[i - 1].candidate_index;
			GgcFetchedCandidate *fetched = &state->fetched[index];
			uint32 request_index = fetched->view.candidate.request_id - 1;

			fetched->next_for_request = request_heads[request_index];
			request_heads[request_index] = index + 1;
		}
	}
	else if (state->request_template.budget.preserve_order && state->restore_count > 1)
		qsort(state->restore_rows, state->restore_count,
			  sizeof(*state->restore_rows), restore_row_compare);

	/* Stable after all visible candidates are known; initialize once per
	 * physical tuple, not once per parent x candidate logical binding. */
	for (uint32 i = 0; i < state->fetched_count; i++)
	{
		GgcBatchTupleView *view = &state->fetched[i].view;

		view->restore_generation = state->restore_generation;
		view->fetched_index = i;
		view->fetched_count = state->fetched_count;
		view->candidate_parent_count =
			state->request_parent_counts[view->candidate.request_id - 1];
	}

	state->candidate_pos = end;
	state->instrumentation.heap_page_groups += page_count;
	state->instrumentation.peak_inflight_pages = Max(
		state->instrumentation.peak_inflight_pages, page_count);
	state->instrumentation.peak_pins = Max(
		state->instrumentation.peak_pins, state->fetched_count ? 1U : 0U);
	state->instrumentation.peak_restore_bytes = Max(
		state->instrumentation.peak_restore_bytes, restore_bytes);
	if (stop_reason != GGC_BATCH_REASON_NONE)
		state->instrumentation.last_reason = stop_reason;
	if (state->timing)
		state->instrumentation.heap_restore_us +=
			elapsed_microseconds(restore_started);
	return true;
}

GgcBatchState *
GgcBatchBegin(const GgcAdjProviderV2 *provider,
			  const GgcOpenRequestV2 *request_template,
			  const GgcBatchParent *parents, uint32 parent_count,
			  const GgcBatchOptions *options)
{
	GgcBatchState *state;
	MemoryContext context;

	validate_common_request(provider, request_template, options, parent_count);
	context = AllocSetContextCreate(request_template->scan_context,
		"graph bounded batch", ALLOCSET_DEFAULT_SIZES);
	state = MemoryContextAllocZero(context, sizeof(*state));
	state->context = context;
	state->request_context = AllocSetContextCreate(context,
		"graph parent restore map", ALLOCSET_DEFAULT_SIZES);
	state->candidate_context = AllocSetContextCreate(context,
		"graph candidate batch", ALLOCSET_DEFAULT_SIZES);
	state->restore_context = AllocSetContextCreate(context,
		"graph heap restore batch", ALLOCSET_DEFAULT_SIZES);
	state->relation_context = AllocSetContextCreate(context,
		"graph heap restore relations", ALLOCSET_DEFAULT_SIZES);
	state->provider_context = AllocSetContextCreate(context,
		"graph provider batch", ALLOCSET_DEFAULT_SIZES);
	state->provider = provider;
	state->request_template = *request_template;
	state->options = *options;
	state->candidate_capacity = Min((Size) request_template->budget.max_candidates,
		request_template->budget.max_candidate_bytes / sizeof(GgcAdjItemV2));
	state->timing = (request_template->estate->es_instrument &
		INSTRUMENT_TIMER) != 0;
	state->started_at = GetCurrentTimestamp();
	PG_TRY();
	{
		build_request_mapping(state, parents, parent_count);
		start_provider(state);
	}
	PG_CATCH();
	{
		destroy_state(state);
		PG_RE_THROW();
	}
	PG_END_TRY();
	return state;
}

/*
 * Return the next logical binding without constructing a mutable tuple slot
 * or copying the wide candidate/handle structure for every parent. Callers
 * provide the exception frame that owns cleanup.
 */
static inline bool
next_binding_internal(GgcBatchState *state, GgcBatchBindingView *view)
{
	uint32		fetched_index;
	uint32		parent_index;
	GgcOwnedParent *parent;

	while (state->restore_pos >= state->restore_count)
		if (!(state->lease_ops ? fill_index_lease(state) : fill_restore_batch(state)))
			return false;
	if (state->lease_ops)
	{
		fetched_index = state->restore_pos;
		parent_index = 0;
	}
	else if (state->linear_restore)
	{
		while (!state->restore_link && state->restore_parent < state->parent_count)
		{
			uint32 request_index = state->parents[state->restore_parent++].request_id - 1;

			state->restore_link = state->restore_request_heads[request_index];
		}
		Assert(state->restore_link > 0);
		parent_index = state->restore_parent - 1;
		fetched_index = state->restore_link - 1;
		state->restore_link = state->fetched[fetched_index].next_for_request;
	}
	else
	{
		GgcRestoreRow *row = &state->restore_rows[state->restore_pos];

		fetched_index = row->fetched_index;
		parent_index = row->parent_index;
	}
	state->restore_pos++;
	parent = &state->parents[parent_index];
	view->parent_id = parent->parent_id;
	view->parent_ordinal = parent->parent_ordinal;
	view->tuple_view = &state->fetched[fetched_index].view;
	state->instrumentation.restored_bindings++;
	if (!state->first_row_returned)
	{
		state->first_row_returned = true;
		state->instrumentation.first_row_us =
			TimestampDifferenceMicroseconds(state->started_at, GetCurrentTimestamp());
	}
	return true;
}

static inline bool
next_view_internal(GgcBatchState *state, GgcBatchView *view)
{
	GgcBatchBindingView binding;
	const GgcBatchTupleView *tuple;

	if (!next_binding_internal(state, &binding))
		return false;
	tuple = binding.tuple_view;
	view->parent_id = binding.parent_id;
	view->parent_ordinal = binding.parent_ordinal;
	view->candidate = &tuple->candidate;
	view->visible_tid = tuple->visible_tid;
	view->tuple = tuple->tuple;
	view->tuple_desc = tuple->tuple_desc;
	view->values = tuple->values;
	view->nulls = tuple->nulls;
	view->restore_generation = tuple->restore_generation;
	view->fetched_index = tuple->fetched_index;
	view->fetched_count = tuple->fetched_count;
	view->candidate_parent_count = tuple->candidate_parent_count;
	return true;
}

bool
GgcBatchNext(GgcBatchState *state, GgcBatchResult *result)
{
	bool		found = false;

	if (!state || state->destroyed || !result)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid graph batch state")));
	PG_TRY();
	{
		GgcBatchView view;

		if (next_view_internal(state, &view))
		{
			GgcRelationState *relation = get_relation_state(state, view.candidate->edge_relid);

			ExecStoreHeapTuple(view.tuple, relation->output_slot, false);
			if (view.values)
			{
				int natts = view.tuple_desc->natts;

				memcpy(relation->output_slot->tts_values, view.values,
					   sizeof(Datum) * natts);
				memcpy(relation->output_slot->tts_isnull, view.nulls,
					   sizeof(bool) * natts);
				relation->output_slot->tts_nvalid = natts;
			}
			result->parent_id = view.parent_id;
			result->parent_ordinal = view.parent_ordinal;
			result->candidate = *view.candidate;
			result->visible_tid = view.visible_tid;
			result->slot = relation->output_slot;
			result->restore_generation = view.restore_generation;
			result->fetched_index = view.fetched_index;
			result->fetched_count = view.fetched_count;
			result->candidate_parent_count = view.candidate_parent_count;
			found = true;
		}
	}
	PG_CATCH();
	{
		destroy_state(state);
		PG_RE_THROW();
	}
	PG_END_TRY();
	return found;
}

bool
GgcBatchNextView(GgcBatchState *state, GgcBatchView *view)
{
	bool		found = false;

	if (!state || state->destroyed || !view)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid graph batch view request")));
	PG_TRY();
	{
		found = next_view_internal(state, view);
	}
	PG_CATCH();
	{
		destroy_state(state);
		PG_RE_THROW();
	}
	PG_END_TRY();
	return found;
}

uint32
GgcBatchNextViews(GgcBatchState *state, GgcBatchView *views, uint32 capacity)
{
	uint32		count = 0;
	bool		first_row;

	if (!state || state->destroyed || !views || capacity == 0 ||
		capacity > MaxAllocSize / sizeof(*views))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid graph batch view array request")));
	first_row = state->options.first_row && !state->first_row_returned;
	PG_TRY();
	{
		CHECK_FOR_INTERRUPTS();
		if (next_view_internal(state, &views[0]))
		{
			uint32 remaining = Min(capacity - 1,
				state->restore_count - state->restore_pos);

			count = 1;
			/* Never refill/reset the restore arena with outstanding views.
			 * A short returned batch is a lifetime boundary, not EOF. */
			if (!first_row)
				for (uint32 i = 0; i < remaining; i++)
				{
					if (!next_view_internal(state, &views[count]))
						elog(ERROR, "graph restore group ended during view delivery");
					count++;
					CHECK_FOR_INTERRUPTS();
				}
		}
	}
	PG_CATCH();
	{
		destroy_state(state);
		PG_RE_THROW();
	}
	PG_END_TRY();
	return count;
}

uint32
GgcBatchNextBindings(GgcBatchState *state, GgcBatchBindingView *views,
					 uint32 capacity)
{
	uint32		count = 0;
	bool		first_row;

	if (!state || state->destroyed || !views || capacity == 0 ||
		capacity > MaxAllocSize / sizeof(*views))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid graph batch binding array request")));
	first_row = state->options.first_row && !state->first_row_returned;
	PG_TRY();
	{
		CHECK_FOR_INTERRUPTS();
		if (next_binding_internal(state, &views[0]))
		{
			uint32 remaining = Min(capacity - 1,
				state->restore_count - state->restore_pos);

			count = 1;
			/* Do not refill with outstanding borrowed physical records. */
			if (!first_row)
				for (uint32 i = 0; i < remaining; i++)
				{
					if (!next_binding_internal(state, &views[count]))
						elog(ERROR, "graph restore group ended during binding delivery");
					count++;
					CHECK_FOR_INTERRUPTS();
				}
		}
	}
	PG_CATCH();
	{
		destroy_state(state);
		PG_RE_THROW();
	}
	PG_END_TRY();
	return count;
}

bool
GgcBatchCanRescan(const GgcBatchState *state,
				  const GgcAdjProviderV2 *provider,
				  const GgcOpenRequestV2 *request_template)
{
	const GgcOpenRequestV2 *previous;

	if (!state || state->destroyed || state->provider != provider ||
		!request_template)
		return false;
	previous = &state->request_template;
/* Compare fields explicitly: struct padding is not a scope fingerprint. */
#define SAME(f) (previous->f == request_template->f)
	return SAME(struct_size) && SAME(abi_version) && SAME(estate) &&
		SAME(scan_context) && SAME(resource_owner) && SAME(snapshot) &&
		SAME(command_id) && SAME(effective_role) && SAME(security_context) &&
		SAME(graph_oid) && SAME(edge_relid) && SAME(index_relid) &&
		SAME(source_attnum) && SAME(destination_attnum) && SAME(endpoint_type) &&
		SAME(endpoint_collation) && SAME(endpoint_opfamily) &&
		SAME(endpoint_domain) && SAME(direction) && SAME(required_caps) &&
		SAME(catalog_generation) && SAME(build_generation);
#undef SAME
}

void
GgcBatchResetInstrumentation(GgcBatchState *state)
{
	if (!state || state->destroyed || !state->provider_eof ||
		state->restore_pos < state->restore_count)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("graph batch accounting reset requires a drained batch")));
	/* Providers report lifetime counters; subtract this checkpoint on read,
	 * rather than resetting provider state or leaking prior hops into this one. */
	if (state->provider_scan && state->provider->get_instrumentation)
		state->provider->get_instrumentation(state->provider_scan,
			&state->provider_baseline);
	MemSet(&state->instrumentation, 0, sizeof(state->instrumentation));
	state->lease_groups = 0;
}

void
GgcBatchRescan(GgcBatchState *state,
			   const GgcOpenRequestV2 *request_template,
			   const GgcBatchParent *parents, uint32 parent_count,
			   const GgcBatchOptions *options)
{
	if (!state || state->destroyed)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid graph batch rescan state")));
	validate_common_request(state->provider, request_template, options,
		parent_count);
	PG_TRY();
	{
		reset_restore_batch(state);
		state->lease_ops = NULL;
		if (state->lease_slot && state->lease_relation != request_template->edge_relid)
		{
			ExecDropSingleTupleTableSlot(state->lease_slot);
			state->lease_slot = NULL;
		}
		MemoryContextReset(state->candidate_context);
		state->candidates = NULL;
		state->candidate_count = state->candidate_pos = 0;
		state->request_template = *request_template;
		state->options = *options;
		state->candidate_capacity = Min((Size) request_template->budget.max_candidates,
			request_template->budget.max_candidate_bytes / sizeof(GgcAdjItemV2));
		state->provider_eof = false;
		state->first_row_returned = false;
		state->started_at = GetCurrentTimestamp();
		build_request_mapping(state, parents, parent_count);
		set_provider_request(state, 0);
		/* Provider rescan is the authoritative request/scope validation gate. */
		{
			instr_time	started;

			if (state->timing)
				INSTR_TIME_SET_CURRENT_FAST(started);
			state->provider->rescan(state->provider_scan,
				&state->provider_request);
			if (state->timing)
				state->instrumentation.batch_setup_us +=
					elapsed_microseconds(started);
		}
		state->instrumentation.endpoint_batches++;
		state->instrumentation.rescan_count++;
	}
	PG_CATCH();
	{
		destroy_state(state);
		PG_RE_THROW();
	}
	PG_END_TRY();
}

void
GgcBatchGetInstrumentation(const GgcBatchState *state,
						   GgcBatchInstrumentation *instrumentation)
{
	if (!state || state->destroyed || !instrumentation)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid graph batch instrumentation request")));
	*instrumentation = state->instrumentation;
	if (state->provider_scan && state->provider->get_instrumentation)
	{
		GgcProviderInstrumentation provider_instrumentation = {0};

		state->provider->get_instrumentation(state->provider_scan,
			&provider_instrumentation);
#define PROVIDER_DELTA(field) \
		instrumentation->provider_##field = provider_instrumentation.field - \
			state->provider_baseline.field
		PROVIDER_DELTA(directory_lookups);
		PROVIDER_DELTA(ref_direct_lookups);
		PROVIDER_DELTA(ref_fallbacks);
		PROVIDER_DELTA(slot_dereferences);
		PROVIDER_DELTA(physical_candidates);
#undef PROVIDER_DELTA
	}
}

static void
destroy_state(GgcBatchState *state)
{
	MemoryContext context;

	if (!state || state->destroyed)
		return;
	state->destroyed = true;
	context = state->context;
	if (state->lease_slot)
		ExecDropSingleTupleTableSlot(state->lease_slot);
	close_relations(state);
	if (state->provider_scan)
	{
		state->provider->end(state->provider_scan);
		state->provider_scan = NULL;
	}
	MemoryContextDelete(context);
}

void
GgcBatchEnd(GgcBatchState *state)
{
	destroy_state(state);
}
