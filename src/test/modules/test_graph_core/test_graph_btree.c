/* SQL diagnostics for the real ordinary B-tree provider, not a mock AM. */
#include "postgres.h"

#include "access/genam.h"
#include "access/nbtree.h"
#include "access/relscan.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_type_d.h"
#include "catalog/pg_am_d.h"
#include "executor/graph_btree_provider.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/wait_event.h"

PG_FUNCTION_INFO_V1(test_graph_btree_provider);
PG_FUNCTION_INFO_V1(test_graph_btree_mark_restore);

/* Deliberately run the AM in a context that is reset after every tuple. */
static bool
mark_test_next(IndexScanDesc scan, MemoryContext scratch, ScanDirection dir,
			   ItemPointer expected_tid, int64 expected_key)
{
	MemoryContext old = MemoryContextSwitchTo(scratch);
	ItemPointer tid = index_getnext_tid(scan, dir);
	bool		found = tid != NULL;

	if (found && expected_tid && !ItemPointerEquals(tid, expected_tid))
		elog(ERROR, "B-tree restored a different heap TID");
	if (found && scan->xs_want_itup && expected_key != 0)
	{
		bool isnull;
		Datum key = index_getattr(scan->xs_itup, 1, scan->xs_itupdesc, &isnull);

		if (isnull || DatumGetInt64(key) != expected_key)
			elog(ERROR, "B-tree restored a different index-only tuple");
	}
	MemoryContextSwitchTo(old);
	MemoryContextReset(scratch);
	return found;
}

/* The SQL fixture has exactly 5000 distinct bigint keys, 1 through 5000. */
Datum
test_graph_btree_mark_restore(PG_FUNCTION_ARGS)
{
	Relation heap;
	Relation index;
	MemoryContext owner;
	MemoryContext scratch;
	MemoryContext old;
	IndexScanDesc scan;
	BTScanOpaque so;
	ScanDirection dir = PG_GETARG_BOOL(3) ? BackwardScanDirection : ForwardScanDirection;
	int64		second_key = PG_GETARG_BOOL(3) ? 4999 : 2;
	ItemPointerData second_tid;
	int			count = 0;

	if (!superuser())
		elog(ERROR, "B-tree mark diagnostic requires superuser");
	heap = table_open(PG_GETARG_OID(0), AccessShareLock);
	index = index_open(PG_GETARG_OID(1), AccessShareLock);
	if (index->rd_rel->relam != BTREE_AM_OID ||
		index->rd_index->indrelid != RelationGetRelid(heap) ||
		TupleDescAttr(RelationGetDescr(index), 0)->atttypid != INT8OID)
		elog(ERROR, "invalid B-tree mark diagnostic index");
	owner = AllocSetContextCreate(CurrentMemoryContext, "B-tree mark test",
								  ALLOCSET_DEFAULT_SIZES);
	scratch = AllocSetContextCreate(owner, "B-tree mark test scratch",
									ALLOCSET_SMALL_SIZES);
	old = MemoryContextSwitchTo(owner);
	scan = index_beginscan(heap, index, GetActiveSnapshot(), NULL, 0, 0, 0);
	scan->xs_want_itup = PG_GETARG_BOOL(2);
	index_rescan(scan, NULL, 0, NULL, 0);
	MemoryContextSwitchTo(old);
	so = (BTScanOpaque) scan->opaque;

	/* A heap scan must not activate an earlier index-only tuple workspace. */
	while (mark_test_next(scan, scratch, dir, NULL, 0))
		count++;
	if (count != 5000 || (!scan->xs_want_itup && (so->currTuples || so->markTuples)))
		elog(ERROR, "B-tree scan retained index-only state or lost tuples");

	for (int round = 0; round < 3; round++)
	{
		index_rescan(scan, NULL, 0, NULL, 0);
		if (BTScanPosIsValid(so->markPos))
			elog(ERROR, "B-tree rescan retained a marked position");
		if (!mark_test_next(scan, scratch, dir, NULL, 0))
			elog(ERROR, "unexpected B-tree EOF");
		index_markpos(scan);
		if (!mark_test_next(scan, scratch, dir, NULL, second_key))
			elog(ERROR, "unexpected B-tree EOF after mark");
		second_tid = scan->xs_heaptid;
		index_restrpos(scan);
		if (!mark_test_next(scan, scratch, dir, &second_tid, second_key))
			elog(ERROR, "unexpected B-tree EOF after same-page restore");

		/* Cross several leaf pages, restore, and do it again after rescan. */
		for (int i = 0; i < 2000; i++)
			if (!mark_test_next(scan, scratch, dir, NULL, 0))
				elog(ERROR, "unexpected B-tree EOF before page-crossing restore");
		if (!BTScanPosIsValid(so->markPos) ||
			(scan->xs_want_itup && (!so->tupleWorkspace ||
			 GetMemoryChunkContext(so->tupleWorkspace) != GetMemoryChunkContext(so))))
			elog(ERROR, "B-tree marked workspace has the wrong lifetime");
		index_restrpos(scan);
		if (!mark_test_next(scan, scratch, dir, &second_tid, second_key))
			elog(ERROR, "unexpected B-tree EOF after page-crossing restore");
	}
	/* Exercise transaction-abort cleanup while the restored page is pinned. */
	if (PG_GETARG_BOOL(4))
		elog(ERROR, "B-tree mark diagnostic requested abort");
	index_endscan(scan);
	MemoryContextDelete(owner);
	index_close(index, AccessShareLock);
	table_close(heap, AccessShareLock);
	PG_RETURN_VOID();
}

/* Negative lease lifecycle probes explicitly close an outstanding lease,
 * including an ERROR raised with the AM context current. No expected error
 * is swallowed without checking its message and restoring the scope. */
static void
check_index_lease(const GgcAdjProviderV2 *provider, GgcAdjScanV2 *scan,
				  GgcOpenRequestV2 *request, int mode)
{
	const GgcIndexLeaseOps *ops = GgcGetIndexLease(provider);
	MemoryContext caller = CurrentMemoryContext;
	Snapshot snapshot = request->estate->es_snapshot;
	GgcIndexLease lease;
	GgcAdjItemV2 item;
	const char *expected = NULL;
	volatile bool caught = false;

	if (!ops)
		elog(ERROR, "missing graph index lease interface");
	switch (mode)
	{
		case 201: expected = "invalid B-tree graph index lease request"; break;
		case 202: expected = "outstanding index lease"; break;
		case 203: expected = "outstanding index lease"; break;
		case 204: expected = "invalid B-tree graph index lease return"; break;
		case 205: expected = "scope changed"; break;
		case 206: expected = "invalid B-tree graph index lease request"; break;
		case 208: expected = "diagnostic error inside leased AM context"; break;
		case 210: expected = "invalid graph index lease registration"; break;
	}
	PG_TRY();
	{
		if (mode == 210)
		{
			GgcIndexLeaseOps invalid = *ops;

			invalid.version++;
			GgcRegisterIndexLease(provider, &invalid);
		}
		if (!ops->begin(scan, mode == 206 ? 0 : 2, false, &lease))
			elog(ERROR, "unexpected EOF before lease diagnostic");
		switch (mode)
		{
			case 200:
				ops->finish(scan, &lease, 0, false);
				if (provider->getnext_batch(scan, &item, 1) != 1 ||
					item.original_ordinal != 0)
					elog(ERROR, "zero-consumption lease advanced the cursor");
				provider->rescan(scan, request);
				if (!ops->begin(scan, 1, false, &lease) || lease.ordinal != 0)
					elog(ERROR, "lease rescan did not reset the cursor");
				ops->finish(scan, &lease, 0, false);
				break;
			case 201: ops->begin(scan, 1, false, &lease); break;
			case 202: provider->getnext_batch(scan, &item, 1); break;
			case 203: provider->rescan(scan, request); break;
			case 204: ops->finish(scan, &lease, lease.capacity + 1, false); break;
			case 205:
				request->estate->es_snapshot = InvalidSnapshot;
				ops->finish(scan, &lease, 0, false);
				break;
			case 207: break; /* end with an outstanding, unconsumed lease */
			case 208:
				MemoryContextSwitchTo(lease.am_context);
				elog(ERROR, "diagnostic error inside leased AM context");
				break;
			case 209:
			{
				GgcAdjProviderV2 copy = *provider;

				if (GgcGetIndexLease(&copy))
					elog(ERROR, "copied provider inherited index lease");
				ops->finish(scan, &lease, 0, false);
				break;
			}
		}
	}
	PG_CATCH();
	{
		ErrorData *error;

		MemoryContextSwitchTo(caller);
		error = CopyErrorData();
		FlushErrorState();
		request->estate->es_snapshot = snapshot;
		provider->end(scan);
		scan = NULL;
		if (!expected || !strstr(error->message, expected))
			ReThrowError(error);
		FreeErrorData(error);
		caught = true;
	}
	PG_END_TRY();
	if (scan)
		provider->end(scan);
	if (expected && !caught)
		elog(ERROR, "index lease diagnostic failed to reject misuse");
}

/* Only the diagnostic materializes JSON, with a hard 10000-candidate limit. */
Datum
test_graph_btree_provider(PG_FUNCTION_ARGS)
{
	const GgcAdjProviderV2 *provider = GetGgcBtreeAdjProvider();
	GgcOpenRequestV2 request = {0};
	EState	   *estate = CreateExecutorState();
	GgcEndpointRequestV2 *endpoints;
	GgcAdjScanV2 *scan;
	GgcAdjItemV2 *items;
	GgcRejectReason reason;
	Datum	   *keys;
	bool	   *nulls;
	int			nkeys;
	int			capacity = PG_GETARG_INT32(4);
	int			mode = PG_GETARG_INT32(5);
	int			lease_mode = mode >= 200 && mode <= 210 ? mode : 0;
	bool		direct_begin = mode >= 100 && mode <= 123;
	int			pause_ms = PG_GETARG_INT32(6);
	int			byte_limit = PG_GETARG_INT32(7);
	int			count_limit = PG_GETARG_INT32(8);
	Oid			rescan_index = PG_GETARG_OID(9);
	Relation	edge;
	Relation	index;
	IndexFetchTableData *fetch;
	TupleTableSlot *slot;
	StringInfoData result;
	StringInfoData batches;
	bool		first = true;
	int			batch_count = 0;
	int			candidates = 0;
	int			phase = 0;
	uint32		snapshot_refs;
	bool		accepted;
	uint64		array_scans;

	/* Exercise begin's own validation, including calls with no preflight. */
	if (direct_begin)
		mode -= 100;
	if (lease_mode)
		mode = 0;
	if (capacity < 1 || capacity > 1024 || pause_ms < 0 || pause_ms > 15000 ||
		byte_limit < 0 || count_limit < 0)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("invalid B-tree diagnostic limits")));
	deconstruct_array(PG_GETARG_ARRAYTYPE_P(3), INT8OID, sizeof(int64),
					  FLOAT8PASSBYVAL, TYPALIGN_DOUBLE, &keys, &nulls, &nkeys);
	if (nkeys < 1 || nkeys > GGC_BTREE_MAX_ENDPOINTS)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("invalid B-tree diagnostic endpoint count")));
	endpoints = palloc0(sizeof(*endpoints) * nkeys);
	for (int i = 0; i < nkeys; i++)
	{
		if (nulls[i])
			ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
							errmsg("null endpoint is not supported")));
		endpoints[i].request_id = i + 1;
		endpoints[i].key = keys[i];
	}
	request.struct_size = sizeof(request);
	request.abi_version = GGC_ADJ_ABI_VERSION;
	request.estate = estate;
	request.scan_context = estate->es_query_cxt;
	request.resource_owner = CurrentResourceOwner;
	request.snapshot = RegisterSnapshot(GetActiveSnapshot());
	estate->es_snapshot = request.snapshot;
	snapshot_refs = request.snapshot->regd_count;
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
	request.budget.max_endpoints = GGC_BTREE_MAX_ENDPOINTS;
	request.budget.max_candidates = count_limit ? count_limit : capacity;
	request.budget.max_pins = 4;
	request.budget.max_inflight_pages = 4;
	request.budget.max_candidate_bytes = byte_limit ? byte_limit * sizeof(GgcAdjItemV2) :
		capacity * sizeof(GgcAdjItemV2);
	request.budget.max_restore_bytes = 1024 * 1024;
	request.budget.preserve_order = lease_mode == 0;
	request.requests = endpoints;
	request.request_count = nkeys;
	index = index_open(request.index_relid, AccessShareLock);
	request.endpoint_opfamily = index->rd_opfamily[0];
	index_close(index, AccessShareLock);
	if (mode >= 10)
	{
		switch (mode)
		{
			case 10: request.struct_size--; break;
			case 11: request.effective_role = InvalidOid; break;
			case 12: request.command_id++; break;
			case 13: request.required_caps |= GGC_CAP_HANDLE_OUTPUT; break;
			case 14: request.budget.max_candidate_bytes = 0; break;
			case 15: endpoints[0].handle.origin = GGC_HANDLE_PERSISTED_ADJACENCY; break;
			case 16: request.source_attnum = 3; break;
			case 17: request.build_generation = 1; break;
			case 18:
				if (nkeys < 2)
					elog(ERROR, "duplicate-ID diagnostic requires two requests");
				endpoints[1].request_id = endpoints[0].request_id;
				break;
			case 19: request.budget.max_restore_bytes = 16; break;
			case 20: request.scan_context = CurrentMemoryContext; break;
			case 21:
				for (int i = 0; i < nkeys; i++)
					endpoints[i].request_id = nkeys - i;
				break;
			case 22:
				if (nkeys < 3)
					elog(ERROR, "nonadjacent duplicate diagnostic requires three requests");
				endpoints[nkeys - 1].request_id = endpoints[0].request_id;
				break;
			case 23:
				for (int i = 0; i < nkeys; i++)
					endpoints[i].request_id = PG_UINT32_MAX - 2 * (nkeys - 1 - i);
				break;
			default: elog(ERROR, "unknown validation diagnostic");
		}
	}
	accepted = direct_begin || provider->validate(&request, &reason);
	if (!direct_begin && (!accepted || mode >= 10))
	{
		char *text = psprintf("{\"accepted\":%s,\"reason\":%d}",
							 accepted ? "true" : "false", reason);

		UnregisterSnapshot(request.snapshot);
		FreeExecutorState(estate);
		PG_RETURN_TEXT_P(cstring_to_text(text));
	}
	scan = provider->begin_batch(&request);
	if (lease_mode)
	{
		check_index_lease(provider, scan, &request, lease_mode);
		UnregisterSnapshot(request.snapshot);
		FreeExecutorState(estate);
		PG_RETURN_TEXT_P(cstring_to_text("{\"lease_contract\":\"PASS\"}"));
	}
	if (mode == 2)
	{
		/* The provider must not borrow the caller's mutable request array. */
		endpoints[0].request_id = 888;
		endpoints[0].key = Int64GetDatum(999999);
	}
	edge = table_open(request.edge_relid, AccessShareLock);
	fetch = table_index_fetch_begin(edge, SO_NONE);
	slot = table_slot_create(edge, NULL);
	items = palloc0(capacity * sizeof(*items));
	initStringInfo(&result);
	initStringInfo(&batches);
	appendStringInfoString(&result, "{\"accepted\":true,\"rows\":[");
	for (;;)
	{
		int count = provider->getnext_batch(scan, items, mode == 4 ? 0 : capacity);

		if (count == 0)
			break;
		if (batch_count++)
			appendStringInfoChar(&batches, ',');
		appendStringInfo(&batches, "%d", count);
		for (int i = 0; i < count; i++)
		{
			ItemPointerData tid = items[i].root_tid;
			bool call_again = false;
			bool all_dead = false;
			bool visible;

			if (++candidates > 10000)
				elog(ERROR, "B-tree diagnostic output limit exceeded");
			if (items[i].opposite_key_available ||
				items[i].opposite_handle.origin != GGC_HANDLE_NONE)
				elog(ERROR, "narrow B-tree fabricated opposite key/handle");
			visible = table_index_fetch_tuple(fetch, &tid, request.snapshot, slot,
											 &call_again, &all_dead);
			if (call_again)
				elog(ERROR, "MVCC heap diagnostic received multiple visible versions");
			if (!first)
				appendStringInfoChar(&result, ',');
			first = false;
			appendStringInfo(&result,
				"{\"phase\":%d,\"request\":%u,\"ordinal\":%llu,"
				"\"root_tid\":\"(%u,%u)\",\"visible\":%s,\"opposite_available\":false",
				phase, items[i].request_id, (unsigned long long) items[i].original_ordinal,
				ItemPointerGetBlockNumber(&items[i].root_tid),
				ItemPointerGetOffsetNumber(&items[i].root_tid), visible ? "true" : "false");
			if (visible)
			{
				int64 values[3];
				bool isnull;

				for (int j = 0; j < 3; j++)
				{
					values[j] = DatumGetInt64(slot_getattr(slot, j + 1, &isnull));
					if (isnull)
						elog(ERROR, "null value in diagnostic edge");
				}
				appendStringInfo(&result,
					",\"id\":%lld,\"src\":%lld,\"dst\":%lld,\"opposite\":%lld,"
					"\"visible_tid\":\"(%u,%u)\"",
					(long long) values[0], (long long) values[1], (long long) values[2],
					(long long) values[request.direction == GGC_OUT ? 2 : 1],
					ItemPointerGetBlockNumber(&slot->tts_tid),
					ItemPointerGetOffsetNumber(&slot->tts_tid));
			}
			appendStringInfoChar(&result, '}');
			ExecClearTuple(slot);
		}
		if (batch_count == 1 || (mode == 7 && phase == 1))
		{
			if (pause_ms)
			{
				/* A visible wait marker proves cancellation happens mid-scan. */
				pgstat_report_wait_start(PG_WAIT_EXTENSION);
				for (int t = 0; t < pause_ms; t += 10)
				{
					CHECK_FOR_INTERRUPTS();
					pg_usleep(10000L);
				}
				pgstat_report_wait_end();
			}
			if (mode == 3)
				ereport(ERROR, (errcode(ERRCODE_DATA_EXCEPTION),
								errmsg("diagnostic error after first candidate batch")));
			if (mode == 6)
				estate->es_snapshot = InvalidSnapshot;
			if (mode == 1)
			{
				if (!OidIsValid(rescan_index))
					elog(ERROR, "rescan diagnostic needs the other index");
				request.direction = request.direction == GGC_OUT ? GGC_IN : GGC_OUT;
				request.index_relid = rescan_index;
				request.request_count = 1;
				endpoints[0].key = Int64GetDatum(20);
				index = index_open(rescan_index, AccessShareLock);
				request.endpoint_opfamily = index->rd_opfamily[0];
				index_close(index, AccessShareLock);
				provider->rescan(scan, &request);
				phase = 1;
			}
			if (mode == 7 && phase < 2)
			{
				if (phase == 0)
				{
					request.request_count = 1;
					endpoints[0].key = Int64GetDatum(20);
				}
				else
				{
					request.request_count = nkeys;
					for (int i = 0; i < nkeys; i++)
						endpoints[i].key = keys[i];
				}
				provider->rescan(scan, &request);
				phase++;
			}
		}
	}
	array_scans = GgcBtreeGetArrayScans(scan);
	provider->end(scan);
	appendStringInfo(&result,
		"],\"batches\":[%s],\"raw_count\":%d,\"item_bytes\":%zu,"
		"\"snapshot_borrowed\":%s,\"capabilities\":%llu,\"array_scans\":%llu}",
		batches.data, candidates, sizeof(GgcAdjItemV2),
		request.snapshot->regd_count == snapshot_refs ? "true" : "false",
		(unsigned long long) provider->capabilities,
		(unsigned long long) array_scans);
	table_index_fetch_end(fetch);
	ExecDropSingleTupleTableSlot(slot);
	table_close(edge, NoLock);
	UnregisterSnapshot(request.snapshot);
	FreeExecutorState(estate);
	PG_RETURN_TEXT_P(cstring_to_text(result.data));
}
