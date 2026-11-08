/* Verify public request mapping independently of a concrete index provider. */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/pg_type_d.h"
#include "executor/executor.h"
#include "executor/graph_batch.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

PG_FUNCTION_INFO_V1(test_graph_batch_mapping);

typedef struct MappingCheck
{
	GgcEndpointRequestV2 *expected;
	uint32 count;
	uint32 seen;
} MappingCheck;

static MappingCheck *mapping_check;

static bool
mapping_validate(const GgcOpenRequestV2 *request, GgcRejectReason *reason)
{
	*reason = GGC_ACCEPT;
	return mapping_check != NULL && request->request_count > 0;
}

static void
mapping_rescan(GgcAdjScanV2 *scan, const GgcOpenRequestV2 *request)
{
	MappingCheck *check = (MappingCheck *) scan;

	for (uint32 i = 0; i < request->request_count; i++)
	{
		const GgcEndpointRequestV2 *actual = &request->requests[i];
		const GgcEndpointRequestV2 *expected;
		const GgcEndpointHandleV2 *a = &actual->handle;
		const GgcEndpointHandleV2 *e;

		if (check->seen >= check->count)
			elog(ERROR, "request mapping emitted an extra request");
		expected = &check->expected[check->seen++];
		e = &expected->handle;
		if (actual->request_id != expected->request_id ||
			DatumGetInt64(actual->key) != DatumGetInt64(expected->key) ||
			a->origin != e->origin || a->index_relid != e->index_relid ||
			a->build_generation != e->build_generation ||
			a->endpoint_domain != e->endpoint_domain || a->slot_id != e->slot_id ||
			a->slot_generation != e->slot_generation ||
			a->provider.struct_size != e->provider.struct_size ||
			a->provider.abi_version != e->provider.abi_version ||
			a->provider.owner_extension_oid != e->provider.owner_extension_oid ||
			a->provider.kind != e->provider.kind ||
			strncmp(NameStr(a->provider.name), NameStr(e->provider.name), NAMEDATALEN))
			elog(ERROR, "request mapping changed first occurrence identity or order");
	}
}

static GgcAdjScanV2 *
mapping_begin(const GgcOpenRequestV2 *request)
{
	mapping_rescan((GgcAdjScanV2 *) mapping_check, request);
	return (GgcAdjScanV2 *) mapping_check;
}

static int
mapping_next(GgcAdjScanV2 *scan, GgcAdjItemV2 *items, int capacity)
{
	return 0;
}

static void
mapping_end(GgcAdjScanV2 *scan)
{
}

Datum
test_graph_batch_mapping(PG_FUNCTION_ARGS)
{
	int count = PG_GETARG_INT32(0);
	int mode = PG_GETARG_INT32(1);
	bool ordered = PG_GETARG_BOOL(2);
	GgcBatchParent *parents;
	MappingCheck check = {0};
	int *classes;
	EState *estate;
	Snapshot snapshot;
	GgcOpenRequestV2 request = {0};
	GgcBatchOptions options = {0};
	GgcBatchInstrumentation instrumentation;
	GgcAdjProviderV2 provider = {0};
	GgcBatchState *state;
	GgcBatchResult row;

	if (count < 1 || count > 4096 || mode < 0 || mode > 13)
		elog(ERROR, "invalid request mapping diagnostic dimensions");
	parents = palloc0(sizeof(*parents) * count);
	check.expected = palloc0(sizeof(*check.expected) * count);
	classes = palloc(sizeof(*classes) * count);
	for (int i = 0; i < count; i++)
	{
		GgcEndpointHandleV2 *handle = &parents[i].handle;
		int group = mode == 2 ? i % 2 : mode >= 3 && mode <= 11 ? i % 3 :
			mode == 12 ? i : 0;
		uint32 j;

		parents[i].parent_id = 1000 + i;
		parents[i].parent_ordinal = ordered ? i : count - i;
		parents[i].key = Int64GetDatum((i % 11) * 13 - 65);
		handle->origin = mode == 0 ? GGC_HANDLE_NONE : GGC_HANDLE_QUERY_HINT;
		handle->provider.struct_size = sizeof(GgcProviderRef);
		handle->provider.abi_version = GGC_ADJ_ABI_VERSION;
		namestrcpy(&handle->provider.name, "mapping");
		switch (mode)
		{
			case 0: handle->slot_id = i; break; /* NONE ignores unused fields */
			case 2: handle->origin += group; break;
			case 3: handle->provider.abi_version += group; break;
			case 4: handle->provider.owner_extension_oid = group; break;
			case 5: handle->provider.kind = group; break;
			case 6: NameStr(handle->provider.name)[0] += group; break;
			case 7: handle->index_relid = group; break;
			case 8: handle->build_generation = group; break;
			case 9: handle->endpoint_domain = group; break;
			case 10: handle->slot_id = group; break;
			case 11: handle->slot_generation = group; break;
			case 12: handle->provider.struct_size--; break;
			case 13: NameStr(handle->provider.name)[20] = i % 127; break;
		}
		/* The expected equivalence classes are explicit test inputs, not the
		 * production comparator. In particular invalid handles never share. */
		for (j = 0; j < check.count; j++)
			if (DatumGetInt64(check.expected[j].key) == DatumGetInt64(parents[i].key) &&
				classes[j] == group)
				break;
		if (j == check.count)
		{
			classes[j] = group;
			check.expected[j].request_id = j + 1;
			check.expected[j].key = parents[i].key;
			check.expected[j].handle = *handle;
			check.count++;
		}
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
	request.endpoint_type = INT8OID;
	request.budget = (GgcBatchBudgetV2) {16, 16, 4, 4, 65536, 2097152, ordered};
	options.struct_size = sizeof(options);
	options.version = GGC_BATCH_EXEC_VERSION;
	provider.struct_size = sizeof(provider);
	provider.abi_version = GGC_ADJ_ABI_VERSION;
	provider.capabilities = GGC_CAP_BATCH | GGC_CAP_ROOT_TID |
		GGC_CAP_COMPLETE_CANDIDATES | GGC_CAP_RESCAN;
	provider.validate = mapping_validate;
	provider.begin_batch = mapping_begin;
	provider.getnext_batch = mapping_next;
	provider.rescan = mapping_rescan;
	provider.end = mapping_end;
	mapping_check = &check;
	PG_TRY();
	{
		state = GgcBatchBegin(&provider, &request, parents, count, &options);
		for (int phase = 0; phase < 2; phase++)
		{
			if (phase)
			{
				check.seen = 0;
				GgcBatchRescan(state, &request, parents, count, &options);
			}
			if (GgcBatchNext(state, &row) || check.seen != check.count)
				elog(ERROR, "request mapping lost a request or invented a result");
		}
		GgcBatchGetInstrumentation(state, &instrumentation);
		GgcBatchEnd(state);
		if (instrumentation.endpoint_requests != check.count * 2 ||
			instrumentation.parent_bindings != count * 2 ||
			instrumentation.shared_parent_requests != (count - check.count) * 2 ||
			instrumentation.rescan_count != 1 ||
			instrumentation.peak_restore_bytes > request.budget.max_restore_bytes)
			elog(ERROR, "request mapping counters or budget are inconsistent");
	}
	PG_FINALLY();
	{
		mapping_check = NULL;
		UnregisterSnapshot(snapshot);
		FreeExecutorState(estate);
	}
	PG_END_TRY();
	PG_RETURN_BOOL(true);
}
