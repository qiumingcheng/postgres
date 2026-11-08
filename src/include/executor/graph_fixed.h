/*-------------------------------------------------------------------------
 * Generic fixed-N graph expansion runtime.
 *-------------------------------------------------------------------------
 */
#ifndef GRAPH_FIXED_H
#define GRAPH_FIXED_H

#include "executor/graph_batch.h"
#include "nodes/execnodes.h"
#include "utils/array.h"

#define GGC_FIXED_EXEC_VERSION 3U

typedef struct GgcFixedAccessPlan
{
	GgcProviderRef provider;	/* serialized identity; never a callback pointer */
	Oid			index_relid;
	uint64		build_generation;
} GgcFixedAccessPlan;

typedef struct GgcFixedStagePlan
{
	GgcFixedAccessPlan primary;
	GgcFixedAccessPlan fallback;
	bool		has_fallback;
	Oid			graph_oid;
	Oid			edge_relid;
	AttrNumber	edge_identity_attnum;
	AttrNumber	source_attnum;
	AttrNumber	destination_attnum;
	AttrNumber	payload_attnum;	/* InvalidAttrNumber means no payload */
	GgcDirection direction;
	uint32		endpoint_domain;
	uint64		catalog_generation;
	uint64		statistics_generation;
	/* Optional destination vertex visibility check, independent of edge MVCC. */
	Oid			vertex_relid;
	Oid			vertex_index_relid;
	AttrNumber	vertex_key_attnum;
} GgcFixedStagePlan;

typedef struct GgcFixedExecOptions
{
	uint32		struct_size;
	uint32		version;
	AttrNumber	anchor_attnum;
	uint32		anchor_batch_size;
	uint32		max_parent_batch;
	/* Accepted extensions in bag mode, path-weighted arcs in factor mode,
	 * visible physical candidates in endpoint-set mode. Not a raw scan cap:
	 * rejected tuples and invisible index entries remain provider work. */
	uint64		max_expanded_edges;
	uint64		max_path_bindings;
	GgcBatchBudgetV2 batch_budget;
	GgcBatchOptions batch_options;
	bool		column_acl;		/* explicit, finite read-set authorization */
} GgcFixedExecOptions;

typedef struct GgcFixedStageInstrumentation
{
	uint64		executions;
	uint64		input_bindings;
	uint64		output_bindings;
	uint64		handle_inputs;
	uint64		handle_scope_fallbacks;
	uint64		vertex_probes;
	uint64		vertex_rejections;
	uint64		vertex_index_only;
	uint64		vertex_heap_fetches;
	uint64		vertex_scan_starts;
	uint64		edge_array_batches;
	uint64		vertex_array_batches;
	uint64		edge_range_batches;
	uint64		vertex_range_batches;
	uint64		vertex_pointer_sorts;
	uint64		endpoint_chunks;
	uint64		chunk_rows;
	uint64		direct_calls;
	uint64		lease_acquisitions;
	uint64		memory_checks;
	uint64		edge_index_only;
	uint64		edge_heap_fetches;
	double		binding_extend_us;
	bool		startup_fallback;
	GgcBatchInstrumentation batch;
} GgcFixedStageInstrumentation;

typedef struct GgcFixedInstrumentation
{
	uint64		anchor_batches;
	uint64		anchor_bindings;
	uint64		expanded_edges;
	uint64		created_bindings;
	uint64		output_bindings;
	uint64		rescan_count;
	uint64		startup_revalidations;
	uint64		startup_fallbacks;
	uint64		rescan_revalidations;
	uint64		runtime_retries;	/* invariant: execution errors are never retried */
	uint32		stage_count;
	uint32		peak_frontier_bindings;
	const GgcFixedStageInstrumentation *stages;	/* borrowed */
} GgcFixedInstrumentation;

/* All arrays and pass-by-reference Datums are read-only and borrowed until
 * the next Next/Rescan/End call. Consumers retaining results must copy them;
 * they must never pfree, repalloc or detoast in place into these arrays. */
typedef struct GgcFixedResult
{
	uint64		binding_ordinal;
	/* Identity of the immutable prefix (all but the final edge), valid only
	 * within this execution/rescan. Consumers must invalidate caches on rescan. */
	uint64		prefix_ordinal;
	uint32		hop_count;
	const int64 *vertices;		/* hop_count + 1, borrowed */
	const int64 *edges;			/* hop_count, borrowed */
	const Datum *payloads;		/* hop_count, borrowed */
	const bool *payload_nulls;	/* hop_count, borrowed */
} GgcFixedResult;

/* Owned text[] output and immutable prefix shared by array consumers.  The
 * caller supplies hop_count scratch Datums and an allocation context, and
 * clears array_prefix_valid on rescan.  Detoasting uses the current per-tuple
 * context; only copied array bytes are reused across results. */
typedef struct GgcFixedTextArrayState
{
	ArrayType  *array_payload_buffer;
	Size		array_payload_capacity;
	Size		array_prefix_bytes;
	Size		array_prefix_overhead;
	uint64		array_prefix_ordinal;
	bool		array_prefix_hasnulls;
	bool		array_prefix_valid;
	MemoryContext array_context;
	Datum	   *array_payloads;
} GgcFixedTextArrayState;

extern ArrayType *GgcFixedBuildPayloadArray(GgcFixedTextArrayState *state,
										  const GgcFixedResult *result);

typedef struct GgcFixedState GgcFixedState;

/* Runtime-only, stage-local predicates. Arguments borrow MVCC-visible data
 * only for the callback duration. Never retain a tuple/Datum pointer, mutate
 * the input or recurse into this executor. The frontend owns expression
 * admission, permissions and the callback contexts (which must outlive the
 * fixed state). NULL callbacks accept all rows. No callback is serialized. */
typedef struct GgcFixedLocalFilter
{
	bool		(*edge) (void *arg, const GgcBatchTupleView *row);
	void	   *edge_arg;
	bool		(*vertex) (void *arg, TupleTableSlot *slot);
	void	   *vertex_arg;
	/* Exact callback read columns; NULL retains full-table SELECT checks.
	 * Positive base attnums, validated and consumed by the setter only. */
	Bitmapset  *edge_columns;
	Bitmapset  *vertex_columns;
} GgcFixedLocalFilter;

/* Cross-variable predicate evaluated at the first stage containing all of
 * its dependencies.  The frontend is responsible for that proof and for
 * expression/permission admission.  Arrays and candidate are borrowed only
 * during accept(): binding has hop_count=stage+1, prefix_ordinal identifies
 * its retained parent, and binding_ordinal is zero (no child exists yet).
 * Returning false allocates no retained child or payload.  Contexts belong
 * to the caller and must outlive the fixed state.  Bag mode only. */
typedef struct GgcFixedBindingFilter
{
	bool (*accept) (void *arg, const GgcFixedResult *binding,
				   const GgcBatchTupleView *candidate);
	void *arg;
} GgcFixedBindingFilter;

extern GgcFixedState *GgcFixedBegin(PlanState *anchor_plan, EState *estate,
									const GgcFixedStagePlan *stages,
									uint32 stage_count,
									const GgcFixedExecOptions *options);
/* Materialized bag bindings and frontier arrays default to work_mem.  This
 * limit is independent of the provider's batch/restore budgets and the
 * explicitly supplied factor/endpoint-set byte limit.  Checkpoints measure
 * actual memory-context allocation, including allocator overhead; one growth
 * can transiently cross the limit before ERROR.  No result truncation, spill
 * or execution-time route switch is performed.  Install before first Next. */
extern void GgcFixedSetMemoryBudget(GgcFixedState *state, Size bytes);
extern Size GgcFixedBindingPeakBytes(const GgcFixedState *state);
/* Install before the first Next, with exactly one entry per stage. Copies
 * the array, not its contexts. Filtered execution is currently bag-only;
 * factorized/endpoint-set modes cannot be combined with these callbacks. */
extern void GgcFixedSetLocalFilters(GgcFixedState *state,
								   const GgcFixedLocalFilter *filters,
								   uint32 stage_count);
extern void GgcFixedSetBindingFilters(GgcFixedState *state,
									 const GgcFixedBindingFilter *filters,
									 uint32 stage_count);
/* Optional reverse access for undirected bag stages. Primary must be OUT;
 * the supplied access is resolved as IN with the same row/vertex mapping.
 * Invalid index entries leave a stage directed. Install once before Next,
 * never with factor/endpoint sets. Self-loops are emitted only by OUT. */
extern void GgcFixedSetUndirectedStages(GgcFixedState *state,
									  const GgcFixedAccessPlan *reverse_access,
									  uint32 stage_count);
/* Bag execution may expand an earlier binding, not only the last endpoint.
 * Position 0 is the anchor; stage i creates position i+1. Inputs and nonnegative
 * match positions must already exist (<= i). -1 means no bound-key check.
 * Copies arrays before first Next. Incompatible with factor/endpoint sets. */
extern void GgcFixedSetBindingPositions(GgcFixedState *state,
									 const uint32 *inputs, const int32 *matches,
									 uint32 stage_count);
extern uint64 GgcFixedGetBoundKeyRejections(const GgcFixedState *state,
											 uint32 stage);
extern bool GgcFixedNext(GgcFixedState *state, GgcFixedResult *result);
extern void GgcFixedRescan(GgcFixedState *state);
extern void GgcFixedGetInstrumentation(const GgcFixedState *state,
										 GgcFixedInstrumentation *instrumentation);
extern uint64 GgcFixedGetStartupMetadataReuses(const GgcFixedState *state);
/* Number of endpoint batches opting into leases, not individual acquisitions. */
extern uint64 GgcFixedGetIndexLeaseBatches(const GgcFixedState *state, uint32 stage);
/* Singleton factor batches bypassing the generic restore-group representation. */
extern uint64 GgcFixedGetDirectIndexBatches(const GgcFixedState *state, uint32 stage);
/* Opt-in before the first Next. Only unordered, path-independent WALK stages
 * without vertex callbacks/fallbacks qualify. False leaves the legacy route.
 * The arena byte limit is additional to the existing bounded batch budget. */
extern bool GgcFixedEnableFactorized(GgcFixedState *state, Size arena_bytes);
extern bool GgcFixedIsFactorized(const GgcFixedState *state);
/* Explicit exact-k WALK endpoint-set contract, NOT a path/bag rewrite.
 * Each anchor binding independently emits its distinct terminal keys. No
 * cross-depth visited set, TRAIL/SIMPLE semantics or path-dependent filters.
 * Only the current and next layer's nodes/handles survive a depth transition;
 * each uses a growing PG hash table, charged to the shared byte limit with
 * allocator overhead.  No arcs/payloads are saved.  This route does not spill;
 * exhaustion raises ERROR, never returns a partial set as a complete answer.
 * Optional destination-vertex checks use the same query snapshot, once per
 * distinct visible key at each depth. Missing vertices never enter a frontier.
 * Both work budgets charge visible physical candidates, not logical paths.
 * Call NextEndpoint instead of Next after enabling; Rescan/End are shared. */
extern bool GgcFixedEnableEndpointSet(GgcFixedState *state, Size arena_bytes);
/* Experimental opt-in; errors unless the complete plan/provider qualifies. */
extern void GgcFixedUseVisibleAdj(GgcFixedState *state);
extern const char *GgcFixedVisibleAdjName(const GgcFixedState *state);
/* Query-local controls for bounded endpoint-set probes, before first Next.
 * They do not authorize bag/order rewrites or weaken access checks. */
extern void GgcFixedSetProbeBatching(GgcFixedState *state, bool edges, bool vertices);
/* Separate execution policy, never raises the caller's endpoint/byte budgets.
 * Limits 1..128; batching is only used by qualified unordered endpoint sets. */
extern void GgcFixedSetEndpointPipeline(GgcFixedState *state, bool enabled,
									  uint32 edge_limit, uint32 vertex_limit);
/* Diagnostic ablation; only covered, payload-free endpoint sets qualify. */
extern void GgcFixedSetEdgeIndexOnly(GgcFixedState *state, bool enabled);
extern bool GgcFixedNextEndpoint(GgcFixedState *state, int64 *anchor, int64 *endpoint);
extern void GgcFixedSetRangeProbes(GgcFixedState *state, bool edges, bool vertices);
/* Optional pointer sorting; scratch remains charged to the factor arena. */
extern void GgcFixedSetVertexPointerSort(GgcFixedState *state, bool enabled);
extern void GgcFixedSetBtreeSeekHint(GgcFixedState *state, bool enabled);
extern void GgcFixedSetBtreeBatchOptions(GgcFixedState *state, bool routes, bool int8cmp);
/* Optional SQL array sink, selected once before the first Next. Requires a
 * virtual slot (int8, int8[], int8[], text[]) and eligible factorized stages.
 * False leaves ordinary result production unchanged. After true, each
 * successful Next also fills this slot and its ExprContext's scantuple; a
 * false Next does not promise a slot value. The caller must keep both alive
 * until End and must copy retained results before Next/Rescan/End. Buffers
 * remain owned by the fixed state; no borrowed heap lifetime is extended. */
extern bool GgcFixedEnableArrayOutput(GgcFixedState *state, TupleTableSlot *slot,
									  ExprContext *econtext);
extern void GgcFixedGetArrayOutputStats(const GgcFixedState *state,
									  uint64 *updates, uint64 *rebuilds);
extern Size GgcFixedFactorizedPeakBytes(const GgcFixedState *state);
extern uint64 GgcFixedFactorizedArcs(const GgcFixedState *state);
extern void GgcFixedEnd(GgcFixedState *state);

#endif							/* GRAPH_FIXED_H */
