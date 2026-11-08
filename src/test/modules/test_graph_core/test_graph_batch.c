/* SQL diagnostics for the public bounded heap-fetch batching layer. */
#include "postgres.h"

#include "access/genam.h"
#include "access/xact.h"
#include "catalog/pg_type_d.h"
#include "executor/graph_batch.h"
#include "test_graph_provider.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/wait_event.h"

PG_FUNCTION_INFO_V1(test_graph_batch);
PG_FUNCTION_INFO_V1(test_graph_batch_cleanup_state);

typedef struct TestWrappedScan
{
	GgcAdjScanV2 *inner_scan;
	const GgcAdjProviderV2 *provider;
	int			mode;
	int			pause_ms;
	int			calls;
} TestWrappedScan;

static const GgcAdjProviderV2 *wrapped_inner;
static GgcAdjProviderV2 wrapped_provider;
static int	wrapped_mode;
static int	wrapped_pause_ms;
static int	wrapped_active;
static uint64 wrapped_validate_calls;
static uint64 wrapped_begin_calls;
static uint64 wrapped_getnext_calls;
static uint64 wrapped_end_calls;

static bool
wrapped_validate(const GgcOpenRequestV2 *request, GgcRejectReason *reason)
{
	bool		valid;

	wrapped_validate_calls++;
	valid = wrapped_inner->validate(request, reason);
	/* Deliberate opt-in diagnostic: stop after positive validation so another
	 * session can publish an ACL invalidation before startup tries to reuse it. */
	if (valid && wrapped_mode == 6 && wrapped_validate_calls == 1)
	{
		pgstat_report_wait_start(PG_WAIT_EXTENSION);
		for (int elapsed = 0; elapsed < wrapped_pause_ms; elapsed += 10)
		{
			CHECK_FOR_INTERRUPTS();
			pg_usleep(10000L);
		}
		pgstat_report_wait_end();
	}
	return valid;
}

static GgcAdjScanV2 *
wrapped_begin(const GgcOpenRequestV2 *request)
{
	TestWrappedScan *scan = MemoryContextAllocZero(request->scan_context,
											 sizeof(*scan));

	scan->provider = wrapped_inner;
	scan->mode = wrapped_mode;
	scan->pause_ms = wrapped_pause_ms;
	scan->inner_scan = wrapped_inner->begin_batch(request);
	wrapped_active++;
	wrapped_begin_calls++;
	return (GgcAdjScanV2 *) scan;
}

static int
wrapped_getnext(GgcAdjScanV2 *opaque, GgcAdjItemV2 *items, int capacity)
{
	TestWrappedScan *scan = (TestWrappedScan *) opaque;
	int			count = scan->provider->getnext_batch(scan->inner_scan,
												 items, capacity);

	scan->calls++;
	wrapped_getnext_calls++;
	if (scan->calls == 1 && count > 0 && scan->mode == 1)
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("graph batch injected provider error")));
	if (scan->calls == 1 && count > 0 && scan->mode == 2)
	{
		pgstat_report_wait_start(PG_WAIT_EXTENSION);
		for (int elapsed = 0; elapsed < scan->pause_ms; elapsed += 10)
		{
			CHECK_FOR_INTERRUPTS();
			pg_usleep(10000L);
		}
		pgstat_report_wait_end();
	}
	/* Diagnostic provider: return the same candidates in reverse physical
	 * delivery order, retaining their logical ordinals. */
	if (scan->mode == 4)
		for (int i = 0; i < count / 2; i++)
		{
			GgcAdjItemV2 tmp = items[i];

			items[i] = items[count - i - 1];
			items[count - i - 1] = tmp;
		}
	return count;
}

static void
wrapped_rescan(GgcAdjScanV2 *opaque, const GgcOpenRequestV2 *request)
{
	TestWrappedScan *scan = (TestWrappedScan *) opaque;

	scan->provider->rescan(scan->inner_scan, request);
	scan->calls = 0;
}

static void
wrapped_end(GgcAdjScanV2 *opaque)
{
	TestWrappedScan *scan = (TestWrappedScan *) opaque;

	scan->provider->end(scan->inner_scan);
	wrapped_active--;
	wrapped_end_calls++;
	pfree(scan);
}

const GgcAdjProviderV2 *
GetTestIndexProvider(Oid indexid)
{
	Relation index = index_open(indexid, AccessShareLock);
	const GgcAdjProviderV2 *provider = GgcGetIndexAdjProvider(index);

	index_close(index, AccessShareLock);
	if (provider == NULL)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("index does not provide graph adjacency access")));
	return provider;
}

static void
wrapped_instrumentation(GgcAdjScanV2 *opaque,
						GgcProviderInstrumentation *instrumentation)
{
	TestWrappedScan *scan = (TestWrappedScan *) opaque;

	MemSet(instrumentation, 0, sizeof(*instrumentation));
	if (scan->provider->get_instrumentation)
		scan->provider->get_instrumentation(scan->inner_scan, instrumentation);
}

const GgcAdjProviderV2 *
GetTestWrappedGraphProvider(const GgcAdjProviderV2 *inner, int mode, int pause_ms)
{
	if (wrapped_active && wrapped_inner != inner)
		elog(ERROR, "cannot change an active test graph provider");
	wrapped_inner = inner;
	wrapped_provider = *wrapped_inner;
	/* Opt-in must be deliberate; normal fault/counter wrappers keep validating
	 * each stage. Mode 6 only pauses and preserves the inner validation contract. */
	if (mode != 6)
		wrapped_provider.capabilities &= ~(GGC_CAP_STARTUP_REUSE |
			GGC_CAP_PATH_INDEPENDENT);
	wrapped_provider.identity.kind = 9001;
	namestrcpy(&wrapped_provider.identity.name, "test_graph_wrapper");
	wrapped_provider.validate = wrapped_validate;
	wrapped_provider.begin_batch = wrapped_begin;
	wrapped_provider.getnext_batch = wrapped_getnext;
	wrapped_provider.rescan = wrapped_rescan;
	wrapped_provider.end = wrapped_end;
	wrapped_provider.get_instrumentation = wrapped_instrumentation;
	wrapped_mode = mode;
	wrapped_pause_ms = pause_ms;
	GgcRegisterAdjProvider(&wrapped_provider);
	return &wrapped_provider;
}

void
UnregisterTestWrappedGraphProvider(void)
{
	GgcUnregisterAdjProvider(&wrapped_provider);
}

void
ResetTestWrappedGraphProviderCounters(void)
{
	if (wrapped_active != 0)
		elog(ERROR, "cannot reset active wrapped graph provider counters");
	wrapped_validate_calls = 0;
	wrapped_begin_calls = 0;
	wrapped_getnext_calls = 0;
	wrapped_end_calls = 0;
}

void
GetTestWrappedGraphProviderCounters(uint64 *validate_calls,
								uint64 *begin_calls,
								uint64 *getnext_calls,
								uint64 *end_calls, int *active)
{
	*validate_calls = wrapped_validate_calls;
	*begin_calls = wrapped_begin_calls;
	*getnext_calls = wrapped_getnext_calls;
	*end_calls = wrapped_end_calls;
	*active = wrapped_active;
}

static void
append_instrumentation(StringInfo result,
					   const GgcBatchInstrumentation *i)
{
	appendStringInfo(result,
		",\"instrumentation\":{"
		"\"parent_bindings\":%llu,\"endpoint_requests\":%llu,"
		"\"shared_parent_requests\":%llu,\"endpoint_batches\":%llu,"
		"\"candidate_batches\":%llu,\"candidates_received\":%llu,"
		"\"candidate_bytes\":%llu,\"heap_page_groups\":%llu,"
		"\"prefetch_submitted\":%llu,\"prefetch_already_cached\":%llu,"
		"\"prefetch_io_started\":%llu,\"tableam_fetches\":%llu,"
		"\"visibility_rejects\":%llu,\"visibility_reuses\":%llu,"
		"\"hot_redirects\":%llu,\"restored_bindings\":%llu,"
		"\"shared_blks_hit\":%llu,\"shared_blks_read\":%llu,"
		"\"rescan_count\":%llu,\"first_row_us\":%llu,"
		"\"peak_candidate_count\":%u,\"peak_candidate_bytes\":%zu,"
		"\"peak_inflight_pages\":%u,\"peak_pins\":%u,"
		"\"peak_restore_bytes\":%zu,\"small_batches\":%llu,"
		"\"first_row_batches\":%llu,\"last_reason\":%d}",
		(unsigned long long) i->parent_bindings,
		(unsigned long long) i->endpoint_requests,
		(unsigned long long) i->shared_parent_requests,
		(unsigned long long) i->endpoint_batches,
		(unsigned long long) i->candidate_batches,
		(unsigned long long) i->candidates_received,
		(unsigned long long) i->candidate_bytes,
		(unsigned long long) i->heap_page_groups,
		(unsigned long long) i->prefetch_submitted,
		(unsigned long long) i->prefetch_already_cached,
		(unsigned long long) i->prefetch_io_started,
		(unsigned long long) i->tableam_fetches,
		(unsigned long long) i->visibility_rejects,
		(unsigned long long) i->visibility_reuses,
		(unsigned long long) i->hot_redirects,
		(unsigned long long) i->restored_bindings,
		(unsigned long long) i->shared_blks_hit,
		(unsigned long long) i->shared_blks_read,
		(unsigned long long) i->rescan_count,
		(unsigned long long) i->first_row_us,
		i->peak_candidate_count, i->peak_candidate_bytes,
		i->peak_inflight_pages, i->peak_pins, i->peak_restore_bytes,
		(unsigned long long) i->small_batches,
		(unsigned long long) i->first_row_batches, i->last_reason);
}

typedef struct TestBatchViewCursor
{
	GgcBatchView views[3];
	GgcBatchBindingView bindings[3];
	uint32		count;
	uint32		pos;
	uint32		first_count;
	uint32		max_count;
	uint64		returned;
} TestBatchViewCursor;

/* A reuse hint must compare every immutable field, not just the index OID. */
static void
check_rescan_scope(GgcBatchState *state, const GgcAdjProviderV2 *provider,
				   const GgcOpenRequestV2 *request)
{
	GgcOpenRequestV2 changed;

	if (!GgcBatchCanRescan(state, provider, request) ||
		GgcBatchCanRescan(state, NULL, request))
		elog(ERROR, "graph batch reuse provider check failed");
#define CHECK_CHANGED(field, value) \
	do { \
		changed = *request; \
		changed.field = (value); \
		if (GgcBatchCanRescan(state, provider, &changed)) \
			elog(ERROR, "graph batch reused changed scope: %s", #field); \
	} while (0)
	CHECK_CHANGED(struct_size, 0);
	CHECK_CHANGED(abi_version, 0);
	CHECK_CHANGED(estate, NULL);
	CHECK_CHANGED(scan_context, NULL);
	CHECK_CHANGED(resource_owner, NULL);
	CHECK_CHANGED(snapshot, NULL);
	CHECK_CHANGED(command_id, request->command_id + 1);
	CHECK_CHANGED(effective_role, InvalidOid);
	CHECK_CHANGED(security_context, request->security_context ^ 1);
	CHECK_CHANGED(graph_oid, request->graph_oid + 1);
	CHECK_CHANGED(edge_relid, InvalidOid);
	CHECK_CHANGED(index_relid, InvalidOid);
	CHECK_CHANGED(source_attnum, 0);
	CHECK_CHANGED(destination_attnum, 0);
	CHECK_CHANGED(endpoint_type, InvalidOid);
	CHECK_CHANGED(endpoint_collation, request->endpoint_collation + 1);
	CHECK_CHANGED(endpoint_opfamily, InvalidOid);
	CHECK_CHANGED(endpoint_domain, request->endpoint_domain + 1);
	CHECK_CHANGED(direction, 0);
	CHECK_CHANGED(required_caps, request->required_caps ^ 1);
	CHECK_CHANGED(catalog_generation, request->catalog_generation + 1);
	CHECK_CHANGED(build_generation, request->build_generation + 1);
#undef CHECK_CHANGED
	changed = *request;
	changed.budget.max_candidates++;
	if (!GgcBatchCanRescan(state, provider, &changed))
		elog(ERROR, "graph batch reuse rejected a mutable budget");
}

static bool
test_batch_next(GgcBatchState *state, GgcBatchResult *row,
				TestBatchViewCursor *cursor, bool use_view, bool use_bulk,
				bool mixed, bool use_bindings, bool mixed_bindings,
				Datum *identity, bool *isnull)
{
	GgcBatchView local;
	const GgcBatchView *view;

	if (use_bindings || (mixed_bindings && cursor->returned % 4 == 0))
	{
		const GgcBatchBindingView *binding;
		const GgcBatchTupleView *tuple;

		if (cursor->pos >= cursor->count)
		{
			cursor->count = GgcBatchNextBindings(state, cursor->bindings,
				mixed_bindings ? 1 : lengthof(cursor->bindings));
			cursor->pos = 0;
			if (!cursor->count)
				return false;
			if (!cursor->first_count)
				cursor->first_count = cursor->count;
			cursor->max_count = Max(cursor->max_count, cursor->count);
			for (uint32 i = 0; i < cursor->count; i++)
			{
				const GgcBatchTupleView *a = cursor->bindings[i].tuple_view;

				if (a->restore_generation != cursor->bindings[0].tuple_view->restore_generation)
					elog(ERROR, "graph batch references crossed a restore lifetime");
				for (uint32 j = 0; j < i; j++)
				{
					const GgcBatchTupleView *b = cursor->bindings[j].tuple_view;

					if ((a->fetched_index == b->fetched_index) != (a == b))
						elog(ERROR, "graph batch did not share physical tuple identity");
				}
			}
		}
		binding = &cursor->bindings[cursor->pos++];
		tuple = binding->tuple_view;
		row->parent_id = binding->parent_id;
		row->parent_ordinal = binding->parent_ordinal;
		row->candidate = tuple->candidate;
		row->visible_tid = tuple->visible_tid;
		row->slot = NULL;
		row->restore_generation = tuple->restore_generation;
		row->fetched_index = tuple->fetched_index;
		row->fetched_count = tuple->fetched_count;
		row->candidate_parent_count = tuple->candidate_parent_count;
		*identity = GgcBatchTupleViewGetAttr(tuple, 1, isnull);
		cursor->returned++;
		return true;
	}
	else if (use_bulk || (mixed_bindings && cursor->returned % 4 == 1))
	{
		if (cursor->pos >= cursor->count)
		{
			cursor->count = GgcBatchNextViews(state, cursor->views,
				mixed_bindings ? 1 : lengthof(cursor->views));
			cursor->pos = 0;
			if (!cursor->count)
				return false;
			if (!cursor->first_count)
				cursor->first_count = cursor->count;
			cursor->max_count = Max(cursor->max_count, cursor->count);
			for (uint32 i = 1; i < cursor->count; i++)
				if (cursor->views[i].restore_generation != cursor->views[0].restore_generation)
					elog(ERROR, "graph batch views crossed a restore lifetime");
		}
		view = &cursor->views[cursor->pos++];
	}
	else if (use_view || (mixed && cursor->returned % 2 == 0) ||
			 (mixed_bindings && cursor->returned % 4 == 2))
	{
		if (!GgcBatchNextView(state, &local))
			return false;
		view = &local;
	}
	else
	{
		if (!GgcBatchNext(state, row))
			return false;
		*identity = slot_getattr(row->slot, 1, isnull);
		cursor->returned++;
		return true;
	}
	row->parent_id = view->parent_id;
	row->parent_ordinal = view->parent_ordinal;
	row->candidate = *view->candidate;
	row->visible_tid = view->visible_tid;
	row->slot = NULL;
	row->restore_generation = view->restore_generation;
	row->fetched_index = view->fetched_index;
	row->fetched_count = view->fetched_count;
	row->candidate_parent_count = view->candidate_parent_count;
	*identity = GgcBatchViewGetAttr(view, 1, isnull);
	cursor->returned++;
	return true;
}

Datum
test_graph_batch(PG_FUNCTION_ARGS)
{
	Datum	   *keys;
	Datum	   *ordinals;
	bool	   *key_nulls;
	bool	   *ordinal_nulls;
	int			nkeys;
	int			nordinals;
	int			max_endpoints = PG_GETARG_INT32(5);
	int			max_candidates = PG_GETARG_INT32(6);
	int			max_pages = PG_GETARG_INT32(7);
	int			candidate_bytes = PG_GETARG_INT32(8);
	int			restore_bytes = PG_GETARG_INT32(9);
	bool		first_row = PG_GETARG_BOOL(10);
	int			small_threshold = PG_GETARG_INT32(11);
	int			small_limit = PG_GETARG_INT32(12);
	bool		prefetch = PG_GETARG_BOOL(13);
	int			diagnostic_mode = PG_GETARG_INT32(14);
	int			raw_mode = diagnostic_mode % 23;
	int			lease_mode = diagnostic_mode / 23;
	bool		lease_enabled = false;
	uint64		lease_groups = 0;
	int			mode = raw_mode == 22 ? 0 : raw_mode >= 17 ? raw_mode - 17 :
		raw_mode == 16 ? 3 : raw_mode >= 11 ? raw_mode - 11 :
		raw_mode == 10 ? 0 : raw_mode >= 5 ? raw_mode - 5 : raw_mode;
	bool		use_view = raw_mode >= 5 && raw_mode < 10;
	bool		use_bulk = raw_mode >= 11 && raw_mode <= 16;
	bool		mixed = raw_mode == 10;
	bool		use_bindings = raw_mode >= 17 && raw_mode <= 21;
	bool		mixed_bindings = raw_mode == 22;
	int			pause_ms = PG_GETARG_INT32(15);
	EState	   *estate;
	Snapshot	snapshot;
	GgcOpenRequestV2 request = {0};
	GgcBatchOptions options = {0};
	GgcBatchParent *parents;
	GgcBatchState *state;
	GgcBatchResult row;
	TestBatchViewCursor view_cursor = {0};
	Datum		edge_identity;
	bool		edge_identity_isnull;
	GgcBatchInstrumentation instrumentation;
	const GgcAdjProviderV2 *provider;
	Relation	index;
	StringInfoData result;
	bool		first = true;
	uint64		rows = 0;
	uint64		qual_calls = 0;
	int			phases = mode == 3 ? 2 : 1;

	if (max_endpoints <= 0 || max_candidates <= 0 || max_pages < 4 ||
		candidate_bytes <= 0 || restore_bytes <= 0 || small_threshold < 0 ||
		small_limit <= 0 || diagnostic_mode < 0 || diagnostic_mode > 68 ||
		pause_ms < 0 || pause_ms > 15000)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid graph batch diagnostic options")));
	deconstruct_array(PG_GETARG_ARRAYTYPE_P(3), INT8OID, sizeof(int64),
					  FLOAT8PASSBYVAL, TYPALIGN_DOUBLE,
					  &keys, &key_nulls, &nkeys);
	deconstruct_array(PG_GETARG_ARRAYTYPE_P(4), INT8OID, sizeof(int64),
					  FLOAT8PASSBYVAL, TYPALIGN_DOUBLE,
					  &ordinals, &ordinal_nulls, &nordinals);
	if (nkeys <= 0 || nkeys != nordinals || nkeys > 10000)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("parent key and ordinal arrays must have equal bounded length")));
	parents = palloc0(sizeof(*parents) * nkeys);
	for (int i = 0; i < nkeys; i++)
	{
		if (key_nulls[i] || ordinal_nulls[i])
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("null graph batch parent is unsupported")));
		parents[i].parent_id = 1000 + i;
		parents[i].parent_ordinal = DatumGetInt64(ordinals[i]);
		parents[i].key = Int64GetDatum(DatumGetInt64(keys[i]));
	}

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
	request.edge_relid = PG_GETARG_OID(0);
	request.index_relid = PG_GETARG_OID(1);
	request.source_attnum = 2;
	request.destination_attnum = 3;
	request.endpoint_type = INT8OID;
	request.endpoint_domain = 77;
	request.direction = PG_GETARG_INT32(2);
	request.required_caps = GGC_CAP_ROOT_TID | GGC_CAP_COMPLETE_CANDIDATES |
		GGC_CAP_BATCH | GGC_CAP_PAUSE_RESUME | GGC_CAP_SSI;
	request.budget.max_endpoints = max_endpoints;
	request.budget.max_candidates = max_candidates;
	request.budget.max_inflight_pages = max_pages;
	request.budget.max_pins = 4;
	request.budget.max_candidate_bytes = candidate_bytes;
	request.budget.max_restore_bytes = restore_bytes;
	request.budget.preserve_order = lease_mode != 1;
	index = index_open(request.index_relid, AccessShareLock);
	provider = GgcGetIndexAdjProvider(index);
	if (provider == NULL)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("index does not provide graph adjacency access")));
	request.endpoint_opfamily = InvalidOid;
	for (int i = 0; i < index->rd_index->indnkeyatts; i++)
		if (index->rd_index->indkey.values[i] ==
			(request.direction == GGC_OUT ? 2 : 3))
		{
			request.endpoint_opfamily = index->rd_opfamily[i];
			break;
		}
	index_close(index, AccessShareLock);
	options.struct_size = sizeof(options);
	options.version = GGC_BATCH_EXEC_VERSION;
	options.prefetch = prefetch;
	options.first_row = first_row;
	options.small_parent_threshold = small_threshold;
	options.small_candidate_limit = small_limit;
	if (mode == 1 || mode == 2 || mode == 4)
		provider = GetTestWrappedGraphProvider(provider, mode, pause_ms);
	state = GgcBatchBegin(provider, &request, parents, nkeys, &options);
	if (lease_mode)
		lease_enabled = GgcBatchEnableIndexLease(state);
	if (raw_mode == 16)
		check_rescan_scope(state, provider, &request);

	initStringInfo(&result);
	appendStringInfoString(&result, "{\"rows\":[");
	for (int phase = 0; phase < phases; phase++)
	{
		view_cursor.count = view_cursor.pos = 0;
		if (phase > 0 && raw_mode == 16)
			GgcBatchResetInstrumentation(state);
		if (phase > 0)
		{
			GgcBatchRescan(state, &request, parents, nkeys, &options);
			if (lease_mode)
				lease_enabled = GgcBatchEnableIndexLease(state);
		}
		while (test_batch_next(state, &row, &view_cursor, use_view, use_bulk,
							  mixed, use_bindings, mixed_bindings,
							  &edge_identity, &edge_identity_isnull))
		{
			bool		isnull = edge_identity_isnull;
			int64		edge_id = DatumGetInt64(edge_identity);
			int64		opposite = DatumGetInt64(row.candidate.opposite_key);
			bool		qual_pass;

			if (isnull || !row.candidate.opposite_key_available ||
				row.candidate.opposite_isnull)
				elog(ERROR, "graph batch diagnostic received an incomplete visible edge");
			/* Deliberately parent-correlated and evaluated once per restored row. */
			qual_calls++;
			qual_pass = ((edge_id + row.parent_id) & 1) == 0;
			if (rows++ >= 20000)
				elog(ERROR, "graph batch diagnostic output limit exceeded");
			if (!first)
				appendStringInfoChar(&result, ',');
			first = false;
			appendStringInfo(&result,
				"{\"phase\":%d,\"parent_id\":%llu,\"parent_ordinal\":%llu,"
				"\"request\":%u,\"candidate_ordinal\":%llu,\"edge_id\":%lld,"
				"\"opposite\":%lld,\"root_tid\":\"(%u,%u)\","
				"\"visible_tid\":\"(%u,%u)\",\"qual_pass\":%s,"
				"\"restore_generation\":%llu,\"fetched_index\":%u,"
				"\"fetched_count\":%u,\"candidate_parent_count\":%u}",
				phase, (unsigned long long) row.parent_id,
				(unsigned long long) row.parent_ordinal,
				row.candidate.request_id,
				(unsigned long long) row.candidate.original_ordinal,
				(long long) edge_id, (long long) opposite,
				ItemPointerGetBlockNumber(&row.candidate.root_tid),
				ItemPointerGetOffsetNumber(&row.candidate.root_tid),
				ItemPointerGetBlockNumber(&row.visible_tid),
				ItemPointerGetOffsetNumber(&row.visible_tid),
				qual_pass ? "true" : "false",
				(unsigned long long) row.restore_generation,
				row.fetched_index, row.fetched_count, row.candidate_parent_count);
		}
	}
	GgcBatchGetInstrumentation(state, &instrumentation);
	lease_groups = GgcBatchGetIndexLeaseGroups(state);
	GgcBatchEnd(state);
	appendStringInfo(&result, "],\"row_count\":%llu,\"qual_calls\":%llu",
		(unsigned long long) rows, (unsigned long long) qual_calls);
	append_instrumentation(&result, &instrumentation);
	if (lease_mode)
		appendStringInfo(&result, ",\"lease_enabled\":%s,\"lease_groups\":%llu",
			lease_enabled ? "true" : "false", (unsigned long long) lease_groups);
	appendStringInfo(&result, ",\"first_view_count\":%u,\"max_view_count\":%u",
		view_cursor.first_count, view_cursor.max_count);
	appendStringInfo(&result, ",\"wrapper_active_after_end\":%d}",
		wrapped_active);
	UnregisterSnapshot(snapshot);
	FreeExecutorState(estate);
	PG_RETURN_TEXT_P(cstring_to_text(result.data));
}

Datum
test_graph_batch_cleanup_state(PG_FUNCTION_ARGS)
{
	char   *result = psprintf("{\"active\":%d,\"end_calls\":%llu}",
		wrapped_active, (unsigned long long) wrapped_end_calls);

	PG_RETURN_TEXT_P(cstring_to_text(result));
}
