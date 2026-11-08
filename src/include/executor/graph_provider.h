/*-------------------------------------------------------------------------
 * Versioned adjacency-provider ABI.  These runtime structs are not Nodes.
 *-------------------------------------------------------------------------
 */
#ifndef GRAPH_PROVIDER_H
#define GRAPH_PROVIDER_H

#include "executor/executor.h"
#include "nodes/graphnodes.h"
#include "storage/itemptr.h"
#include "utils/resowner.h"
#include "utils/snapshot.h"

#define GGC_ADJ_ABI_VERSION 2U
#define GGC_CATALOG_ABI_VERSION 1U
#define GGC_RESULT_ABI_VERSION 1U

#define GGC_CAP_OUT (UINT64CONST(1) << 0)
#define GGC_CAP_IN (UINT64CONST(1) << 1)
#define GGC_CAP_COMPLETE_CANDIDATES (UINT64CONST(1) << 2)
#define GGC_CAP_ROOT_TID (UINT64CONST(1) << 3)
#define GGC_CAP_RESCAN (UINT64CONST(1) << 4)
#define GGC_CAP_SSI (UINT64CONST(1) << 5)
#define GGC_CAP_SORTED (UINT64CONST(1) << 6)
#define GGC_CAP_PARALLEL (UINT64CONST(1) << 7)
#define GGC_CAP_BATCH (UINT64CONST(1) << 8)
#define GGC_CAP_PAUSE_RESUME (UINT64CONST(1) << 9)
#define GGC_CAP_HANDLE_INPUT (UINT64CONST(1) << 10)
#define GGC_CAP_HANDLE_OUTPUT (UINT64CONST(1) << 11)
#define GGC_CAP_BUILD_GENERATION (UINT64CONST(1) << 12)
/* Positive startup validation may be reused for consecutive identical stages
 * within one ExecutorStart, with the same execution scope, retained relation
 * locks and no processed invalidation. No runtime/Rescan validation is waived.
 * Providers with mutable non-catalog validation state must not advertise it. */
#define GGC_CAP_STARTUP_REUSE (UINT64CONST(1) << 13)
/* In a fixed snapshot/scope, candidate membership and values depend only on
 * the endpoint request, never its logical path history or multiplicity. */
#define GGC_CAP_PATH_INDEPENDENT (UINT64CONST(1) << 14)
/* Optional fixed-WALK delivery: bounded candidates may be consumed with
 * ordinary heap TableAM fetches, one borrowed slot at a time. The provider
 * retains SSI protection and validates live scope on each batch/rescan.
 * This is not an index-only scan or permission to bypass MVCC. */
#define GGC_CAP_HEAP_STREAM (UINT64CONST(1) << 15)
/* Explicit consumer contract: provider checks endpoint column privileges;
 * the consumer checks every additional read column. Never a waiver of RLS.
 * Requests without this bit retain whole-table SELECT authorization. */
#define GGC_CAP_COLUMN_ACL (UINT64CONST(1) << 16)

typedef enum GgcRejectReason
{
	GGC_ACCEPT = 0,
	GGC_REJECT_ABI,
	GGC_REJECT_TYPE,
	GGC_REJECT_SHAPE,
	GGC_REJECT_SECURITY,
	GGC_REJECT_SNAPSHOT,
	GGC_REJECT_CAPABILITY,
	GGC_REJECT_GENERATION,
	GGC_REJECT_RESOURCE
} GgcRejectReason;

typedef struct GgcProviderRef
{
	uint32		struct_size;
	uint32		abi_version;
	Oid			owner_extension_oid;
	uint32		kind;
	NameData	name;
} GgcProviderRef;

typedef enum GgcHandleOrigin
{
	GGC_HANDLE_NONE = 0,
	GGC_HANDLE_QUERY_HINT,
	GGC_HANDLE_PERSISTED_ADJACENCY
} GgcHandleOrigin;

typedef struct GgcEndpointHandleV2
{
	GgcProviderRef provider;
	Oid			index_relid;
	uint64		build_generation;
	uint32		endpoint_domain;
	uint64		slot_id;
	uint64		slot_generation;
	GgcHandleOrigin origin;
} GgcEndpointHandleV2;

typedef struct GgcEndpointRequestV2
{
	uint32		request_id;
	Datum		key;
	GgcEndpointHandleV2 handle;
} GgcEndpointRequestV2;

typedef struct GgcBatchBudgetV2
{
	uint32		max_endpoints;
	uint32		max_candidates;
	uint32		max_inflight_pages;
	uint32		max_pins;
	Size		max_candidate_bytes;
	Size		max_restore_bytes;
	bool		preserve_order;
} GgcBatchBudgetV2;

typedef struct GgcOpenRequestV2
{
	uint32		struct_size;
	uint32		abi_version;
	EState	   *estate;
	MemoryContext scan_context;
	ResourceOwner resource_owner;
	Snapshot	snapshot;
	CommandId	command_id;
	Oid			effective_role;
	int			security_context;
	Oid			graph_oid;
	Oid			edge_relid;
	Oid			index_relid;
	AttrNumber	source_attnum;
	AttrNumber	destination_attnum;
	Oid			endpoint_type;
	Oid			endpoint_collation;
	Oid			endpoint_opfamily;
	uint32		endpoint_domain;
	GgcDirection direction;
	uint64		required_caps;
	uint64		catalog_generation;
	uint64		build_generation;
	GgcBatchBudgetV2 budget;
	uint32		request_count;
	const GgcEndpointRequestV2 *requests;
} GgcOpenRequestV2;

typedef struct GgcAdjItemV2
{
	uint32		request_id;
	uint64		original_ordinal;
	Oid			edge_relid;
	ItemPointerData root_tid;
	Datum		opposite_key;
	bool		opposite_key_available;
	bool		opposite_isnull;
	GgcEndpointHandleV2 opposite_handle;
	uint64		descriptor_generation;
} GgcAdjItemV2;

typedef struct GgcEstimateRequestV2
{
	uint32		struct_size;
	uint32		abi_version;
	MemoryContext planner_context;
	Oid			graph_oid;
	Oid			edge_relid;
	Oid			index_relid;
	Oid			effective_role;
	uint64		catalog_generation;
	uint64		semantic_fingerprint;
	double		distinct_endpoints;
	double		bindings;
	double		shared_requests;
	double		key_requests;
	double		ref_requests;
	GgcDirection direction;
	GgcBatchBudgetV2 budget;
	const void *statistics;
} GgcEstimateRequestV2;

typedef struct GgcEstimateV2
{
	Cost		startup_cost;
	Cost		total_cost;
	double		logical_edges;
	double		physical_entries;
	double		edge_heap_fetches;
	double		vertex_heap_fetches;
	double		adjacency_pages;
	double		heap_page_groups;
	Size		transient_memory_bytes;
	Size		resident_memory_bytes;
	uint64		statistics_generation;
	uint32		cost_profile_version;
	bool		calibrated;
	bool		uncertainty_unknown;
} GgcEstimateV2;

typedef struct GgcAdjScanV2 GgcAdjScanV2;

/* Provider-owned physical counters exported through the common batch layer. */
typedef struct GgcProviderInstrumentation
{
	uint64		directory_lookups;
	uint64		ref_direct_lookups;
	uint64		ref_fallbacks;
	uint64		slot_dereferences;
	uint64		physical_candidates;
} GgcProviderInstrumentation;

typedef struct GgcAdjProviderV2
{
	uint32		struct_size;
	uint32		abi_version;
	GgcProviderRef identity;
	uint64		capabilities;
	bool		(*validate) (const GgcOpenRequestV2 *, GgcRejectReason *);
	bool		(*estimate) (const GgcEstimateRequestV2 *, GgcEstimateV2 *);
	GgcAdjScanV2 *(*begin_batch) (const GgcOpenRequestV2 *);
	int			(*getnext_batch) (GgcAdjScanV2 *, GgcAdjItemV2 *, int capacity);
	void		(*rescan) (GgcAdjScanV2 *, const GgcOpenRequestV2 *);
	void		(*end) (GgcAdjScanV2 *);
	uint64		(*generation) (Oid graph_oid, Oid edge_relid);
	void		(*get_instrumentation) (GgcAdjScanV2 *,
									GgcProviderInstrumentation *);
} GgcAdjProviderV2;

/* Optional, separately versioned public IndexScan lease. No private AM API.
 * The caller must advance only through index_getnext_tid/index_fetch_heap,
 * in am_context, and finish before any provider call or external callback.
 * A provider end must also release a lease interrupted by ERROR/cancel.
 * Pointers are backend-local and never serialized in plans. */
/* Version 3: begin explicitly requests index tuples before the first rescan.
 * Returned values remain borrowed until the next AM call. Visibility and
 * heap-page predicate locking remain the consumer's responsibility. */
#define GGC_INDEX_LEASE_VERSION 4U
typedef struct GgcIndexLease
{
	struct IndexScanDescData *scan;
	MemoryContext am_context;
	/* Zero denotes a sorted multi-key B-tree lease. The consumer must map
	 * xs_itup's first key to its bounded request array before fetching heap. */
	uint32 request_id;
	uint32 capacity;
	uint64 ordinal;
} GgcIndexLease;

typedef struct GgcIndexLeaseOps
{
	uint32 struct_size;
	uint32 version;
	bool (*begin) (GgcAdjScanV2 *, uint32, bool, GgcIndexLease *);
	void (*finish) (GgcAdjScanV2 *, const GgcIndexLease *, uint32, bool);
	/* Explicit opt-in to ordinary bigint B-tree index-tuple consumption.
	 * Registrations are provider-pointer scoped; wrappers do not inherit it.
	 * Keep the common executor independent of concrete provider factories. */
	bool btree_values;
	bool (*set_range_probe) (GgcAdjScanV2 *, bool);
} GgcIndexLeaseOps;

extern void GgcRegisterIndexLease(const GgcAdjProviderV2 *provider,
								  const GgcIndexLeaseOps *ops);
extern const GgcIndexLeaseOps *GgcGetIndexLease(const GgcAdjProviderV2 *provider);

/* Optional startup hooks, separate from the V2 provider ABI. validate_open
 * borrows matching caller-opened relations with AccessShareLock held; it must
 * perform the same request/permission/shape checks as validate, without
 * closing or retaining the borrowed references. No validation result crosses
 * a callback or snapshot boundary. A self-validating begin must independently
 * check the entire live request under its own retained locks before reading
 * any candidate; only the immediately preceding duplicate validate is waived.
 * Callback tables have backend-static lifetime and are never plan data. */
#define GGC_PROVIDER_SETUP_VERSION 1U
typedef struct GgcProviderSetupOps
{
	uint32 struct_size;
	uint32 version;
	bool (*validate_open) (const GgcOpenRequestV2 *, Relation, Relation,
						  GgcRejectReason *);
	bool self_validating_begin;
} GgcProviderSetupOps;

extern void GgcRegisterProviderSetup(const GgcAdjProviderV2 *provider,
									const GgcProviderSetupOps *ops);
extern const GgcProviderSetupOps *GgcGetProviderSetup(const GgcAdjProviderV2 *provider);

/* Separately versioned, read-only index metadata. The caller holds
 * AccessShareLock on the borrowed relation. A provider advertising build
 * generations must supply an authoritative getter, not an edge-table guess.
 * This does not change the V2 scan ABI or the index lease contract. */
#define GGC_INDEX_METADATA_VERSION 1U
typedef struct GgcIndexMetadataOps
{
	uint32 struct_size;
	uint32 version;
	uint64 (*build_generation) (Relation index);
} GgcIndexMetadataOps;

extern void GgcRegisterIndexMetadata(const GgcAdjProviderV2 *provider,
									 const GgcIndexMetadataOps *ops);
extern bool GgcGetIndexBuildGeneration(Relation index, uint64 *generation);

/*
 * Runtime registry entry points.  Cached plans store only GgcProviderRef;
 * callback addresses are resolved again in ExecutorStart.  Built-in
 * providers are obtained from the index AM, without linking the common
 * executor to a concrete provider. Extensions can register backend-local
 * static callback tables during _PG_init and unregister during _PG_fini.
 */
extern void GgcRegisterAdjProvider(const GgcAdjProviderV2 *provider);
extern void GgcUnregisterAdjProvider(const GgcAdjProviderV2 *provider);
extern const GgcAdjProviderV2 *GgcGetIndexAdjProvider(Relation index);
extern const GgcAdjProviderV2 *GgcResolveAdjProvider(
	Relation index, const GgcProviderRef *reference, GgcRejectReason *reason);

static inline bool
GgcBatchBudgetIsBounded(const GgcBatchBudgetV2 *budget)
{
	return budget != NULL && budget->max_endpoints > 0 &&
		budget->max_candidates > 0 && budget->max_inflight_pages > 0 &&
		budget->max_pins > 0 && budget->max_candidate_bytes > 0 &&
		budget->max_restore_bytes > 0;
}

static inline bool
GgcProviderRefEqual(const GgcProviderRef *left, const GgcProviderRef *right)
{
	return left->struct_size == sizeof(GgcProviderRef) &&
		right->struct_size == sizeof(GgcProviderRef) &&
		left->abi_version == right->abi_version &&
		left->owner_extension_oid == right->owner_extension_oid &&
		left->kind == right->kind &&
		strncmp(NameStr(left->name), NameStr(right->name), NAMEDATALEN) == 0;
}

static inline bool
GgcEndpointHandleScopeMatches(const GgcEndpointHandleV2 *handle,
							  const GgcProviderRef *provider,
							  Oid index_relid, uint64 build_generation,
							  uint32 endpoint_domain)
{
	return handle != NULL && handle->origin != GGC_HANDLE_NONE &&
		GgcProviderRefEqual(&handle->provider, provider) &&
		handle->index_relid == index_relid &&
		handle->build_generation == build_generation &&
		handle->endpoint_domain == endpoint_domain;
}

#endif							/* GRAPH_PROVIDER_H */
