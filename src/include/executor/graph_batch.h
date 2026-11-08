/*-------------------------------------------------------------------------
 * Public bounded candidate fetch and parent-restore support for graph stages.
 *-------------------------------------------------------------------------
 */
#ifndef GRAPH_BATCH_H
#define GRAPH_BATCH_H

#include "access/htup_details.h"
#include "executor/graph_provider.h"
#include "executor/tuptable.h"

#define GGC_BATCH_EXEC_VERSION 2U

typedef enum GgcBatchReason
{
	GGC_BATCH_REASON_NONE = 0,
	GGC_BATCH_REASON_CANDIDATE_LIMIT,
	GGC_BATCH_REASON_CANDIDATE_BYTES,
	GGC_BATCH_REASON_PAGE_LIMIT,
	GGC_BATCH_REASON_RESTORE_BYTES,
	GGC_BATCH_REASON_SMALL_REQUEST,
	GGC_BATCH_REASON_FIRST_ROW,
	GGC_BATCH_REASON_PROVIDER_EOF
} GgcBatchReason;

/* One logical parent binding.  Equal endpoint requests may share one probe. */
typedef struct GgcBatchParent
{
	uint64		parent_id;
	uint64		parent_ordinal;
	Datum		key;
	GgcEndpointHandleV2 handle;
} GgcBatchParent;

typedef struct GgcBatchOptions
{
	uint32		struct_size;
	uint32		version;
	bool		prefetch;
	bool		first_row;
	uint32		small_parent_threshold;
	uint32		small_candidate_limit;
} GgcBatchOptions;

typedef struct GgcBatchInstrumentation
{
	uint64		parent_bindings;
	uint64		endpoint_requests;
	uint64		shared_parent_requests;
	uint64		endpoint_batches;
	uint64		candidate_batches;
	uint64		candidates_received;
	uint64		candidate_bytes;
	uint64		heap_page_groups;
	uint64		prefetch_submitted;
	uint64		prefetch_already_cached;
	uint64		prefetch_io_started;
	uint64		tableam_fetches;
	uint64		visibility_rejects;
	uint64		visibility_reuses;
	uint64		hot_redirects;
	uint64		restored_bindings;
	uint64		shared_blks_hit;
	uint64		shared_blks_read;
	uint64		rescan_count;
	uint64		first_row_us;
	uint32		peak_candidate_count;
	Size		peak_candidate_bytes;
	uint32		peak_inflight_pages;
	uint32		peak_pins;
	Size		peak_restore_bytes; /* charged restore capacity, not allocator RSS */
	uint64		small_batches;
	uint64		first_row_batches;
	uint64		provider_directory_lookups;
	uint64		provider_ref_direct_lookups;
	uint64		provider_ref_fallbacks;
	uint64		provider_slot_dereferences;
	uint64		provider_physical_candidates;
	double		batch_setup_us;
	double		provider_getnext_us;
	double		heap_restore_us;
	GgcBatchReason last_reason;
} GgcBatchInstrumentation;

typedef struct GgcBatchResult
{
	uint64		parent_id;
	uint64		parent_ordinal;
	GgcAdjItemV2 candidate;
	ItemPointerData visible_tid;
	TupleTableSlot *slot;          /* borrowed until next/rescan/end */
	/* Query-local identity of the MVCC-checked physical candidate. The index
	 * is dense within one bounded restore group, never a persistent edge ID.
	 * Generation is unique within one GgcBatchState, including its rescans. */
	uint64		restore_generation;
	uint32		fetched_index;
	uint32		fetched_count;
	uint32		candidate_parent_count;
} GgcBatchResult;

/*
 * Read-only alternative for consumers that only need user-column values.
 * All pointers refer to owned, MVCC-checked restore data, not an unpinned
 * shared heap page. Borrowed until ANY Next/NextView/NextViews/NextBindings/
 * Rescan/End call.
 * Consumers
 * retaining values must copy them; they must not mutate/free the tuple, arrays
 * or Datums. Arbitrary expression evaluation can still use GgcBatchNext's slot.
 * The cursor, ordering, multiplicity, budgets and cleanup are shared by both
 * interfaces; mixing them does not duplicate or skip rows.
 */
typedef struct GgcBatchView
{
	uint64		parent_id;
	uint64		parent_ordinal;
	const GgcAdjItemV2 *candidate;
	ItemPointerData visible_tid;
	HeapTuple	tuple;		/* read-only, owned by restore arena */
	TupleDesc	tuple_desc;
	const Datum *values;		/* optional cached deformation */
	const bool *nulls;
	uint64		restore_generation;
	uint32		fetched_index;
	uint32		fetched_count;
	uint32		candidate_parent_count;
} GgcBatchView;

/* One immutable record per MVCC-visible physical candidate, shared by all
 * restored parents in this bounded group. No pointer refers to an unpinned
 * heap page. Neither this record nor any referenced value may be modified. */
typedef struct GgcBatchTupleView
{
	GgcAdjItemV2 candidate;
	ItemPointerData visible_tid;
	HeapTuple	tuple;
	TupleDesc	tuple_desc;
	const Datum *values;
	const bool *nulls;
	uint64		restore_generation;
	uint32		fetched_index;
	uint32		fetched_count;
	uint32		candidate_parent_count;
} GgcBatchTupleView;

typedef struct GgcBatchBindingView
{
	uint64		parent_id;
	uint64		parent_ordinal;
	const GgcBatchTupleView *tuple_view;
} GgcBatchBindingView;

static inline Datum
GgcBatchTupleViewGetAttr(const GgcBatchTupleView *view, AttrNumber attnum,
						 bool *isnull)
{
	Assert(attnum > 0 && attnum <= view->tuple_desc->natts);
	if (view->values)
	{
		*isnull = view->nulls[attnum - 1];
		return view->values[attnum - 1];
	}
	return heap_getattr(view->tuple, attnum, view->tuple_desc, isnull);
}

/* attnum must be a validated user attribute of this relation. */
static inline Datum
GgcBatchViewGetAttr(const GgcBatchView *view, AttrNumber attnum, bool *isnull)
{
	Assert(attnum > 0 && attnum <= view->tuple_desc->natts);
	if (view->values)
	{
		*isnull = view->nulls[attnum - 1];
		return view->values[attnum - 1];
	}
	return heap_getattr(view->tuple, attnum, view->tuple_desc, isnull);
}

typedef struct GgcBatchState GgcBatchState;

/* Explicit opt-in before the first read of a singleton unordered request.
 * False leaves the ordinary batch path unchanged; Rescan clears opt-in.
 * Uses the same small-request policy and Next view lifetime as ordinary
 * batching. The pin counter is a conservative upper bound, not a sample. */
extern bool GgcBatchEnableIndexLease(GgcBatchState *state);
extern uint64 GgcBatchGetIndexLeaseGroups(const GgcBatchState *state);

/* max_restore_bytes bounds charged request/restore array capacities and tuple
 * payloads, including temporary request sorting. It is not a hard bound on
 * all memory-context blocks or the provider's separately bounded workspace. */
extern GgcBatchState *GgcBatchBegin(const GgcAdjProviderV2 *provider,
									const GgcOpenRequestV2 *request_template,
									const GgcBatchParent *parents,
									uint32 parent_count,
									const GgcBatchOptions *options);
extern bool GgcBatchNext(GgcBatchState *state, GgcBatchResult *result);
extern bool GgcBatchNextView(GgcBatchState *state, GgcBatchView *view);
/* Caller owns the capacity-sized metadata array. All returned views share one
 * restore-group lifetime and remain valid until ANY next/rescan/end call.
 * Short batches are normal; only zero means EOF. The first-row policy still
 * returns exactly one view before buffering additional logical bindings. */
extern uint32 GgcBatchNextViews(GgcBatchState *state, GgcBatchView *views,
								uint32 capacity);
/* Same cursor, bounds and first-row policy as NextViews, but repeat only
 * parent identity plus a reference, not all physical tuple metadata. All
 * references expire on ANY Next/NextView/NextViews/NextBindings/Rescan/End.
 * A consumer must drain its returned array before calling another entry. */
extern uint32 GgcBatchNextBindings(GgcBatchState *state,
								   GgcBatchBindingView *views, uint32 capacity);
/* Only a reuse hint, never authorization: Rescan still validates the live
 * provider scope. Budgets and endpoint requests may differ. */
extern bool GgcBatchCanRescan(const GgcBatchState *state,
							 const GgcAdjProviderV2 *provider,
							 const GgcOpenRequestV2 *request_template);
/* Start a new accounting interval after draining a batch, before Rescan.
 * Does not reset the scan, restore generation, or lifetime provider counters. */
extern void GgcBatchResetInstrumentation(GgcBatchState *state);
extern void GgcBatchRescan(GgcBatchState *state,
							 const GgcOpenRequestV2 *request_template,
							 const GgcBatchParent *parents,
							 uint32 parent_count,
							 const GgcBatchOptions *options);
extern void GgcBatchGetInstrumentation(const GgcBatchState *state,
										 GgcBatchInstrumentation *instrumentation);
extern void GgcBatchEnd(GgcBatchState *state);

#endif							/* GRAPH_BATCH_H */
