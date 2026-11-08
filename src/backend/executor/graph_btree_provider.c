/*-------------------------------------------------------------------------
 * graph_btree_provider.c
 *
 * Public index-AM scans for a bounded batch of endpoint equality probes.
 * This provider does not fetch heap tuples, infer opposite keys, deduplicate
 * bindings, or manufacture stable adjacency handles.  Public Table AM fetch
 * and frontend permission/qual checks remain the caller's responsibility.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/nbtree.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_opfamily_d.h"
#include "catalog/pg_type_d.h"
#include "executor/graph_btree_provider.h"
#include "executor/graph_access.h"
#include "executor/graph_index_capabilities.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

#define BTREE_CAPS (GGC_CAP_OUT | GGC_CAP_IN | GGC_CAP_COMPLETE_CANDIDATES | \
					GGC_CAP_ROOT_TID | GGC_CAP_RESCAN | GGC_CAP_SSI | \
					GGC_CAP_BATCH | GGC_CAP_PAUSE_RESUME | GGC_CAP_STARTUP_REUSE | \
					GGC_CAP_PATH_INDEPENDENT | GGC_CAP_COLUMN_ACL)

struct GgcAdjScanV2
{
	MemoryContext context;
	MemoryContext am_context;
	MemoryContext request_context;
	GgcOpenRequestV2 request;
	Relation	edge;
	Relation	index;
	IndexScanDesc index_scan;
	Oid			equality_function;
	uint32		endpoint;
	uint64		ordinal;
	bool		primed;
	bool		array_probe;
	bool		range_probe;
	bool		want_index_tuple;
	uint64		array_scans;
	bool		lease_active;
	uint32		lease_capacity;
};

static bool
reject(GgcRejectReason *reason, GgcRejectReason value)
{
	if (reason)
		*reason = value;
	return false;
}

/* A borrowed query context/owner/snapshot must not outlive its executor. */
static bool
scope_valid(const GgcOpenRequestV2 *r, GgcRejectReason *reason)
{
	Oid			role;
	int			security;
	MemoryContext ancestor;

	if (!r || r->struct_size != sizeof(*r) ||
		r->abi_version != GGC_ADJ_ABI_VERSION)
		return reject(reason, GGC_REJECT_ABI);
	if (!r->estate || !r->scan_context || !r->resource_owner ||
		r->resource_owner != CurrentResourceOwner)
		return reject(reason, GGC_REJECT_RESOURCE);
	for (ancestor = r->scan_context; ancestor;
		 ancestor = MemoryContextGetParent(ancestor))
		if (ancestor == r->estate->es_query_cxt)
			break;
	if (!ancestor)
		return reject(reason, GGC_REJECT_RESOURCE);
	if (!r->snapshot || !IsMVCCSnapshot(r->snapshot) ||
		r->snapshot->regd_count == 0 ||
		r->estate->es_snapshot != r->snapshot ||
		r->snapshot->curcid != r->command_id ||
		GetCurrentCommandId(false) != r->command_id)
		return reject(reason, GGC_REJECT_SNAPSHOT);
	GetUserIdAndSecContext(&role, &security);
	if (r->effective_role != role || r->security_context != security)
		return reject(reason, GGC_REJECT_SECURITY);
	return true;
}

static bool
relation_valid(const GgcOpenRequestV2 *r, Relation edge, Relation index,
			   GgcRejectReason *reason)
{
	TupleDesc	desc = RelationGetDescr(edge);
	AttrNumber	endpoint = r->direction == GGC_OUT ?
		r->source_attnum : r->destination_attnum;
	bool		table_select = pg_class_aclcheck(r->edge_relid,
												r->effective_role, ACL_SELECT) == ACLCHECK_OK;

	/* RLS needs a frontend/common-layer proof, not an unchecked raw scan. */
	if (edge->rd_rel->relrowsecurity ||
		(!table_select &&
		 (!(r->required_caps & GGC_CAP_COLUMN_ACL) ||
		  r->source_attnum <= 0 || r->destination_attnum <= 0 ||
		  pg_attribute_aclcheck(r->edge_relid, r->source_attnum,
								r->effective_role, ACL_SELECT) != ACLCHECK_OK ||
		  pg_attribute_aclcheck(r->edge_relid, r->destination_attnum,
								r->effective_role, ACL_SELECT) != ACLCHECK_OK)))
		return reject(reason, GGC_REJECT_SECURITY);
	if (edge->rd_rel->relkind != RELKIND_RELATION ||
		edge->rd_rel->relam != HEAP_TABLE_AM_OID || RELATION_IS_OTHER_TEMP(edge) ||
		index->rd_rel->relkind != RELKIND_INDEX || index->rd_rel->relam != BTREE_AM_OID ||
		!index->rd_index || index->rd_index->indrelid != r->edge_relid ||
		!index->rd_index->indisvalid || !index->rd_index->indisready ||
		!index->rd_index->indislive || index->rd_index->indcheckxmin ||
		index->rd_index->indnkeyatts < 1 ||
		index->rd_index->indkey.values[0] != endpoint ||
		RelationGetIndexExpressions(index) != NIL ||
		RelationGetIndexPredicate(index) != NIL)
		return reject(reason, GGC_REJECT_SHAPE);
	if (r->source_attnum <= 0 || r->source_attnum > desc->natts ||
		r->destination_attnum <= 0 || r->destination_attnum > desc->natts ||
		r->source_attnum == r->destination_attnum)
		return reject(reason, GGC_REJECT_SHAPE);
	for (int i = 0; i < 2; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(desc,
			(i == 0 ? r->source_attnum : r->destination_attnum) - 1);

		if (attr->attisdropped || !GgcIntegerKeyType(attr->atttypid) || !attr->attnotnull)
			return reject(reason, GGC_REJECT_TYPE);
	}
	if (r->endpoint_type != INT8OID || OidIsValid(r->endpoint_collation) ||
		r->endpoint_opfamily != INTEGER_BTREE_FAM_OID ||
		r->endpoint_opfamily != index->rd_opfamily[0] ||
		!GgcIntegerKeyType(index->rd_opcintype[0]) ||
		!OidIsValid(get_opfamily_member(r->endpoint_opfamily, index->rd_opcintype[0], INT8OID,
									BTEqualStrategyNumber)))
		return reject(reason, GGC_REJECT_TYPE);
	return true;
}

static bool
btree_request_valid(const GgcOpenRequestV2 *r, GgcRejectReason *reason)
{
	bool increasing = true;

	if (reason)
		*reason = GGC_ACCEPT;
	if (!scope_valid(r, reason))
		return false;
	if ((r->required_caps & ~BTREE_CAPS) ||
		(r->direction != GGC_OUT && r->direction != GGC_IN))
		return reject(reason, GGC_REJECT_CAPABILITY);
	if (r->build_generation != 0)
		return reject(reason, GGC_REJECT_GENERATION);
	if (!GgcBatchBudgetIsBounded(&r->budget) || !r->requests ||
		r->request_count == 0 || r->request_count > GGC_BTREE_MAX_ENDPOINTS ||
		r->request_count > r->budget.max_endpoints ||
		r->budget.max_pins < 4 || r->budget.max_inflight_pages < 4 ||
		r->budget.max_candidate_bytes < sizeof(GgcAdjItemV2) ||
		r->budget.max_restore_bytes < GGC_BTREE_MIN_WORK_BYTES +
			(Size) r->request_count * (sizeof(GgcEndpointRequestV2) + sizeof(int64)))
		return reject(reason, GGC_REJECT_RESOURCE);
	for (uint32 i = 0; i < r->request_count; i++)
	{
		if (r->requests[i].handle.origin != GGC_HANDLE_NONE)
			return reject(reason, GGC_REJECT_CAPABILITY);
		if (i && r->requests[i - 1].request_id >= r->requests[i].request_id)
			increasing = false;
	}
	/* Prove uniqueness; unordered external IDs retain the exact fallback. */
	if (increasing)
		return true;
	for (uint32 i = 0; i < r->request_count; i++)
	{
		for (uint32 j = 0; j < i; j++)
			if (r->requests[i].request_id == r->requests[j].request_id)
				return reject(reason, GGC_REJECT_SHAPE);
	}
	return true;
}

static bool
btree_validate_open(const GgcOpenRequestV2 *r, Relation edge, Relation index,
					GgcRejectReason *reason)
{
	if (!btree_request_valid(r, reason))
		return false;
	if (!edge || !index || RelationGetRelid(edge) != r->edge_relid ||
		RelationGetRelid(index) != r->index_relid)
		return reject(reason, GGC_REJECT_SHAPE);
	return relation_valid(r, edge, index, reason);
}

static bool
btree_validate(const GgcOpenRequestV2 *r, GgcRejectReason *reason)
{
	Relation	edge;
	Relation	index;
	bool		valid;

	if (!btree_request_valid(r, reason))
		return false;
	/* Holding both relation locks again at begin closes the validation gap. */
	edge = table_open(r->edge_relid, AccessShareLock);
	index = index_open(r->index_relid, AccessShareLock);
	valid = relation_valid(r, edge, index, reason);
	index_close(index, AccessShareLock);
	table_close(edge, AccessShareLock);
	return valid;
}

static void
check_memory(GgcAdjScanV2 *scan)
{
	if (MemoryContextMemAllocated(scan->context, true) >
		scan->request.budget.max_restore_bytes)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("B-tree graph provider working memory budget exceeded")));
}

static void
copy_request(GgcAdjScanV2 *scan, const GgcOpenRequestV2 *r)
{
	MemoryContext old;
	GgcEndpointRequestV2 *requests;

	scan->request = *r;
	old = MemoryContextSwitchTo(scan->request_context);
	requests = palloc0(sizeof(*requests) * r->request_count);
	for (uint32 i = 0; i < r->request_count; i++)
	{
		requests[i].request_id = r->requests[i].request_id;
		requests[i].key = Int64GetDatum(DatumGetInt64(r->requests[i].key));
	}
	scan->request.requests = requests;
	MemoryContextSwitchTo(old);
	scan->endpoint = 0;
	scan->ordinal = 0;
	scan->primed = false;
	scan->array_probe = false;
	scan->range_probe = false;
	scan->want_index_tuple = false;
}

/* An array scan emits keys in index order. Only merge requests already in
 * that order, preserving request-major delivery and duplicate multiplicity.
 * INCLUDE columns do not change the order of the single bigint search key. */
static bool
array_probe_compatible(const GgcAdjScanV2 *scan)
{
	if (scan->request.request_count < 2 ||
		!GgcBtreeArrayIndexEligible(scan->index->rd_index->indnkeyatts,
			(scan->index->rd_indoption[0] & INDOPTION_DESC) != 0))
		return false;
	for (uint32 i = 1; i < scan->request.request_count; i++)
		if (DatumGetInt64(scan->request.requests[i - 1].key) >=
			DatumGetInt64(scan->request.requests[i].key))
			return false;
	return true;
}

static void
prime_scan(GgcAdjScanV2 *scan)
{
	ScanKeyData scan_keys[2];
	int nkeys = scan->range_probe ? 2 : 1;

	if (scan->endpoint == 0)
		scan->array_probe = array_probe_compatible(scan);
	/* Public index_rescan requires exactly the original key count. */
	if (scan->index_scan->numberOfKeys != nkeys)
	{
		index_endscan(scan->index_scan);
		scan->index_scan = NULL;
		scan->index_scan = index_beginscan(scan->edge, scan->index,
			scan->request.snapshot, NULL, nkeys, 0, SO_NONE);
	}
	scan->index_scan->xs_want_itup = scan->array_probe || scan->want_index_tuple;
	if (scan->range_probe)
	{
		Assert(scan->array_probe);
		GgcIntegerScanKey(&scan_keys[0], scan->index->rd_opcintype[0], BTGreaterEqualStrategyNumber,
			scan->request.requests[0].key);
		GgcIntegerScanKey(&scan_keys[1], scan->index->rd_opcintype[0], BTLessEqualStrategyNumber,
			scan->request.requests[scan->request.request_count - 1].key);
	}
	else if (scan->array_probe)
	{
		MemoryContext old = MemoryContextSwitchTo(scan->request_context);
		Datum	   *keys = palloc(sizeof(Datum) * scan->request.request_count);
		ArrayType  *array;

		for (uint32 i = 0; i < scan->request.request_count; i++)
			keys[i] = scan->request.requests[i].key;
		array = construct_array(keys, scan->request.request_count, INT8OID,
								sizeof(int64), FLOAT8PASSBYVAL, TYPALIGN_DOUBLE);
		pfree(keys);
		MemoryContextSwitchTo(old);
		GgcIntegerScanKey(&scan_keys[0], scan->index->rd_opcintype[0],
			BTEqualStrategyNumber, PointerGetDatum(array));
		scan_keys[0].sk_flags |= SK_SEARCHARRAY;
	}
	else
		GgcIntegerScanKey(&scan_keys[0], scan->index->rd_opcintype[0], BTEqualStrategyNumber,
					scan->request.requests[scan->endpoint].key);
	/* Public index APIs retain normal B-tree SSI and AM accounting. */
	index_rescan(scan->index_scan, scan_keys, nkeys, NULL, 0);
	if (scan->array_probe)
		scan->array_scans++;
	scan->primed = true;
}

static void
open_scan(GgcAdjScanV2 *scan, const GgcOpenRequestV2 *r)
{
	MemoryContext old;
	GgcRejectReason reason;
	Oid			operator;

	copy_request(scan, r);
	old = MemoryContextSwitchTo(scan->am_context);
	scan->edge = table_open(r->edge_relid, AccessShareLock);
	scan->index = index_open(r->index_relid, AccessShareLock);
	if (!relation_valid(r, scan->edge, scan->index, &reason))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("B-tree graph provider rejected request (%d)", reason)));
	operator = get_opfamily_member(r->endpoint_opfamily, scan->index->rd_opcintype[0], INT8OID,
									   BTEqualStrategyNumber);
	scan->equality_function = get_opcode(operator);
	/* Use the public wrapper: it owns SSI/predicate locking and AM accounting. */
	scan->index_scan = index_beginscan(scan->edge, scan->index, r->snapshot,
									 NULL, 1, 0, SO_NONE);
	MemoryContextSwitchTo(old);
	check_memory(scan);
}

static GgcAdjScanV2 *
btree_begin(const GgcOpenRequestV2 *r)
{
	GgcAdjScanV2 *scan;
	GgcRejectReason reason;
	MemoryContext context;

	/*
	 * begin_batch must be safe without a preceding validate call.  Validate
	 * the request here, then let open_scan check the relations under the locks
	 * and references retained by this scan.  Calling btree_validate here would
	 * open, check and close the same relations immediately before open_scan
	 * opens and checks them again; it does not close any additional race.
	 */
	if (!btree_request_valid(r, &reason))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("B-tree graph provider rejected request (%d)", reason)));
	context = AllocSetContextCreate(r->scan_context, "B-tree graph provider",
									ALLOCSET_DEFAULT_SIZES);
	scan = MemoryContextAllocZero(context, sizeof(*scan));
	scan->context = context;
	scan->am_context = AllocSetContextCreate(context, "B-tree graph AM",
										   ALLOCSET_DEFAULT_SIZES);
	scan->request_context = AllocSetContextCreate(context, "B-tree graph requests",
													ALLOCSET_DEFAULT_SIZES);
	open_scan(scan, r);
	return scan;
}

static int
btree_getnext(GgcAdjScanV2 *scan, GgcAdjItemV2 *items, int capacity)
{
	MemoryContext old;
	GgcRejectReason reason;
	Size		limit;
	int			count = 0;

	if (scan->lease_active)
		elog(ERROR, "B-tree graph scan has an outstanding index lease");
	if (!items || capacity <= 0)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("B-tree graph batch capacity must be positive")));
	if (!scope_valid(&scan->request, &reason))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("B-tree graph provider scope changed during scan (%d)", reason)));
	limit = Min((Size) capacity, (Size) scan->request.budget.max_candidates);
	limit = Min(limit, scan->request.budget.max_candidate_bytes / sizeof(*items));
	old = MemoryContextSwitchTo(scan->am_context);
	while (count < limit && scan->endpoint < scan->request.request_count)
	{
		const GgcEndpointRequestV2 *request;
		ItemPointer tid;

		CHECK_FOR_INTERRUPTS();
		if (!scan->primed)
			prime_scan(scan);
		tid = index_getnext_tid(scan->index_scan, ForwardScanDirection);
		if (!tid)
		{
			if (scan->array_probe)
			{
				scan->endpoint = scan->request.request_count;
				break;
			}
			scan->endpoint++;
			scan->primed = false;
			continue;
		}
		if (scan->array_probe)
		{
			bool		isnull;
			int64		endpoint;

			if (!scan->index_scan->xs_itup)
				elog(ERROR, "B-tree array scan returned no index tuple");
			endpoint = GgcIntegerKeyValue(index_getattr(scan->index_scan->xs_itup, 1,
				scan->index_scan->xs_itupdesc, &isnull), scan->index->rd_opcintype[0]);
			if (isnull)
				elog(ERROR, "B-tree array scan returned a null endpoint");
			while (scan->endpoint < scan->request.request_count &&
				   DatumGetInt64(scan->request.requests[scan->endpoint].key) < endpoint)
				scan->endpoint++;
			if (scan->endpoint == scan->request.request_count ||
				DatumGetInt64(scan->request.requests[scan->endpoint].key) != endpoint)
				elog(ERROR, "B-tree array scan returned an unexpected endpoint");
		}
		request = &scan->request.requests[scan->endpoint];
		if (scan->ordinal == PG_UINT64_MAX)
			ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
							errmsg("B-tree graph candidate ordinal overflow")));
		MemSet(&items[count], 0, sizeof(items[count]));
		items[count].request_id = request->request_id;
		items[count].original_ordinal = scan->ordinal++;
		items[count].edge_relid = scan->request.edge_relid;
		items[count].root_tid = *tid;
		/* Explicitly unavailable: Datum zero is not a missing-key sentinel. */
		items[count].opposite_key_available = false;
		items[count].opposite_handle.origin = GGC_HANDLE_NONE;
		count++;
	}
	MemoryContextSwitchTo(old);
	check_memory(scan);
	return count;
}

static bool
btree_begin_lease(GgcAdjScanV2 *scan, uint32 capacity, bool want_index_tuple,
				  GgcIndexLease *lease)
{
	GgcRejectReason reason;
	MemoryContext old;
	Size limit;

	if (!lease || !capacity || scan->lease_active ||
		(scan->request.request_count != 1 && !array_probe_compatible(scan)) ||
		scan->request.budget.preserve_order)
		elog(ERROR, "invalid B-tree graph index lease request");
	if (!scope_valid(&scan->request, &reason))
		elog(ERROR, "B-tree graph index lease scope changed (%d)", reason);
	if (scan->endpoint == scan->request.request_count)
		return false;
	if (scan->primed && scan->want_index_tuple != want_index_tuple)
		elog(ERROR, "cannot change graph index tuple mode during a scan");
	scan->want_index_tuple = want_index_tuple;
	limit = Min(capacity, scan->request.budget.max_candidates);
	limit = Min(limit, scan->request.budget.max_candidate_bytes / sizeof(GgcAdjItemV2));
	limit = Min(limit, PG_UINT64_MAX - scan->ordinal);
	if (!limit)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						errmsg("B-tree graph index lease capacity exhausted")));
	old = MemoryContextSwitchTo(scan->am_context);
	if (!scan->primed)
		prime_scan(scan);
	MemoryContextSwitchTo(old);
	scan->lease_capacity = limit;
	scan->lease_active = true;
	*lease = (GgcIndexLease) {
		.scan = scan->index_scan, .am_context = scan->am_context,
		.request_id = scan->request.request_count == 1 ?
			scan->request.requests[0].request_id : 0,
		.capacity = limit, .ordinal = scan->ordinal
	};
	return true;
}

static void
btree_finish_lease(GgcAdjScanV2 *scan, const GgcIndexLease *lease,
				   uint32 consumed, bool eof)
{
	GgcRejectReason reason;

	if (!scan->lease_active || !lease || lease->scan != scan->index_scan ||
		lease->am_context != scan->am_context || lease->ordinal != scan->ordinal ||
		lease->request_id != (scan->request.request_count == 1 ?
			scan->request.requests[0].request_id : 0) ||
		lease->capacity != scan->lease_capacity || consumed > lease->capacity)
		elog(ERROR, "invalid B-tree graph index lease return");
	if (!scope_valid(&scan->request, &reason))
		elog(ERROR, "B-tree graph index lease scope changed (%d)", reason);
	scan->ordinal += consumed;
	if (eof)
	{
		scan->endpoint = scan->request.request_count;
		scan->primed = false;
	}
	scan->lease_active = false;
	check_memory(scan);
}

static void
close_scan(GgcAdjScanV2 *scan)
{
	if (scan->index_scan)
		index_endscan(scan->index_scan);
	index_close(scan->index, NoLock);
	table_close(scan->edge, NoLock);
	scan->index_scan = NULL;
	scan->index = NULL;
	scan->edge = NULL;
}

static void
btree_rescan(GgcAdjScanV2 *scan, const GgcOpenRequestV2 *r)
{
	GgcRejectReason reason;

	if (scan->lease_active)
		elog(ERROR, "cannot rescan B-tree graph with an outstanding index lease");
	if (!btree_request_valid(r, &reason))
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("B-tree graph provider rejected rescan (%d)", reason)));
	if (r->estate != scan->request.estate || r->snapshot != scan->request.snapshot ||
		r->resource_owner != scan->request.resource_owner ||
		r->scan_context != scan->request.scan_context ||
		r->edge_relid != scan->request.edge_relid ||
		r->index_relid != scan->request.index_relid ||
		r->endpoint_type != scan->request.endpoint_type ||
		r->endpoint_collation != scan->request.endpoint_collation ||
		r->endpoint_opfamily != scan->request.endpoint_opfamily ||
		r->command_id != scan->request.command_id ||
		r->effective_role != scan->request.effective_role ||
		r->security_context != scan->request.security_context ||
		r->graph_oid != scan->request.graph_oid ||
		r->catalog_generation != scan->request.catalog_generation ||
		r->endpoint_domain != scan->request.endpoint_domain ||
		r->source_attnum != scan->request.source_attnum ||
		r->destination_attnum != scan->request.destination_attnum ||
		r->direction != scan->request.direction ||
		r->required_caps != scan->request.required_caps ||
		r->build_generation != scan->request.build_generation)
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						errmsg("B-tree graph provider cannot change execution scope on rescan")));
	/* Held relation locks and the immutable scope make reopen unnecessary. */
	MemoryContextReset(scan->request_context);
	copy_request(scan, r);
	check_memory(scan);
}

static void
btree_end(GgcAdjScanV2 *scan)
{
	MemoryContext context = scan->context;

	close_scan(scan);
	MemoryContextDelete(context);
}

static bool
btree_estimate(const GgcEstimateRequestV2 *r, GgcEstimateV2 *estimate)
{
	/* No fabricated calibrated coefficients before G1-048. */
	MemSet(estimate, 0, sizeof(*estimate));
	estimate->uncertainty_unknown = true;
	return false;
}

/* Query-local diagnostic: actual array index_rescan calls, including rescans.
 * The caller must own a live scan from this provider. No global state. */
uint64
GgcBtreeGetArrayScans(const GgcAdjScanV2 *scan)
{
	return scan->array_scans;
}

/* Opt-in before a fresh request is read. Never fill holes in a key set. */
bool
GgcBtreeSetRangeProbe(GgcAdjScanV2 *scan, bool enabled)
{
	if (!scan || scan->primed || scan->lease_active || scan->ordinal)
		elog(ERROR, "graph range policy requires an unread request");
	scan->range_probe = false;
	if (!enabled || scan->request.budget.preserve_order || !array_probe_compatible(scan))
		return false;
	for (uint32 i = 1; i < scan->request.request_count; i++)
	{
		int64 prior = DatumGetInt64(scan->request.requests[i - 1].key);
		int64 key = DatumGetInt64(scan->request.requests[i].key);
		if (prior == PG_INT64_MAX || key != prior + 1)
			return false;
	}
	scan->range_probe = true;
	return true;
}

static uint64
btree_generation(Oid graph_oid, Oid edge_relid)
{
	/* No persistent handle or generation claims for ordinary B-tree. */
	return 0;
}

const GgcAdjProviderV2 *
GetGgcBtreeAdjProvider(void)
{
	static const GgcProviderSetupOps setup = {
		.struct_size = sizeof(GgcProviderSetupOps),
		.version = GGC_PROVIDER_SETUP_VERSION,
		.validate_open = btree_validate_open,
		.self_validating_begin = true
	};
	static const GgcIndexLeaseOps lease = {
		.struct_size = sizeof(GgcIndexLeaseOps),
		.version = GGC_INDEX_LEASE_VERSION,
		.begin = btree_begin_lease, .finish = btree_finish_lease,
		.btree_values = true, .set_range_probe = GgcBtreeSetRangeProbe
	};
	static const GgcAdjProviderV2 provider = {
		.struct_size = sizeof(GgcAdjProviderV2),
		.abi_version = GGC_ADJ_ABI_VERSION,
		.identity = {
			.struct_size = sizeof(GgcProviderRef),
			.abi_version = GGC_ADJ_ABI_VERSION,
			.kind = 4,
			.name = {"btree_adj"}
		},
		.capabilities = BTREE_CAPS,
		.validate = btree_validate,
		.estimate = btree_estimate,
		.begin_batch = btree_begin,
		.getnext_batch = btree_getnext,
		.rescan = btree_rescan,
		.end = btree_end,
		.generation = btree_generation
	};

	GgcRegisterIndexLease(&provider, &lease);
	GgcRegisterProviderSetup(&provider, &setup);
	return &provider;
}
