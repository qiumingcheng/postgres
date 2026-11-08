/*-------------------------------------------------------------------------
 * graph_fixed.c
 *
 * A generic fixed-N expansion state machine.  The stage count is data, not
 * a switch statement or a product limit.  Every stage uses the common
 * bounded endpoint/candidate batching layer. Intermediate stages materialize
 * a bounded next frontier; the final stage streams borrowed results.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/detoast.h"
#include "access/nbtree.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "access/visibilitymap.h"
#include "catalog/pg_type_d.h"
#include "catalog/pg_am_d.h"
#include "executor/executor.h"
#include "executor/graph_fixed.h"
#include "executor/graph_access.h"
#include "executor/graph_visible_adj.h"
#include "executor/graph_index_capabilities.h"
#include "fmgr.h"
#include "executor/instrument.h"
#include "miscadmin.h"
#include "storage/sinval.h"
#include "storage/bufmgr.h"
#include "storage/predicate.h"
#include "utils/datum.h"
#include "utils/array.h"
#include "utils/acl.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"

#define GGC_FIXED_VIEW_BATCH_SIZE 32U
#define GGC_ENDPOINT_BATCH_MAX 128U
#define GGC_FACTOR_BUCKETS 64U
#define GGC_ENDPOINT_NODES_PER_CHUNK 64U

/* Explicit opt-in; no AGE library is loaded or cache built by this GUC. */
bool enable_graph_age_adjacency = false;

static bool
btree_value_provider(const GgcAdjProviderV2 *provider)
{
	const GgcIndexLeaseOps *ops = GgcGetIndexLease(provider);

	return ops && ops->btree_values;
}

typedef struct GgcFactorNode GgcFactorNode;
typedef struct GgcFactorArc
{
	struct GgcFactorArc *next;
	GgcFactorNode *prior;
	uint64 offset;
	int64 edge;
	Datum payload;
	bool isnull;
} GgcFactorArc;

struct GgcFactorNode
{
	int64 endpoint;
	GgcFactorNode *hash_next;
	GgcFactorNode *next;
	GgcFactorArc *incoming;
	GgcFactorArc *last;
	uint64 paths;
	uint64 base_ordinal;
};

typedef struct GgcFactorLayer
{
	GgcFactorNode **buckets;
	uint32 nbuckets;
	uint32 nodes;
	/* Endpoint sets need no hash index while insertion order is increasing.
	 * On the first interior lookup or unordered insertion, index all existing
	 * nodes.  Their addresses and the traversal list never change. */
	MemoryContext context;
	char *node_next;
	uint32 nodes_available;
	GgcFactorNode *head;
	GgcFactorNode *last;
	uint64 paths;
	/* Avoid allocating a hash table for chains/singleton frontiers. */
	GgcFactorNode singleton;
	GgcEndpointHandleV2 singleton_handle;
} GgcFactorLayer;

StaticAssertDecl(offsetof(GgcFactorLayer, singleton_handle) ==
		offsetof(GgcFactorLayer, singleton) + MAXALIGN(sizeof(GgcFactorNode)),
		"singleton endpoint handle must immediately follow the aligned node");

typedef struct GgcFactorFrame
{
	GgcFactorNode *node;
	GgcFactorArc *next;
	GgcFactorArc *chosen;
	uint64 root_pos;
} GgcFactorFrame;

typedef struct GgcOwnedFixedStage
{
	GgcFixedStagePlan plan;
	GgcFixedLocalFilter filter;
	const GgcAdjProviderV2 *provider;
	Oid			index_relid;
	uint64		build_generation;
	Oid			endpoint_opfamily;
	Oid			identity_type;
	Oid			opposite_type;
	Oid			payload_type;
	int16		payload_typlen;
	bool		payload_typbyval;
	char		payload_typalign;
	uint64		startup_inval_count;
	uint64		index_lease_batches;
	uint64		direct_index_batches;
	Relation	vertex_relation;
	Relation	vertex_index;
	IndexScanDesc vertex_scan;
	TupleTableSlot *vertex_slot;
	struct GgcOwnedFixedStage *vertex_owner;
	Buffer		vertex_vm_buffer;
	bool		vertex_index_only;
	bool		array_index_eligible;
	AttrNumber cover_index[2]; /* opposite endpoint, edge identity; zero = absent */
} GgcOwnedFixedStage;

typedef struct GgcResolvedFixedAccess
{
	const GgcAdjProviderV2 *provider;
	Relation	index_relation;
	Oid			index_relid;
	uint64		build_generation;
	Oid			endpoint_opfamily;
	GgcRejectReason reject_reason;
	bool		valid;
} GgcResolvedFixedAccess;

typedef struct GgcFixedBinding
{
	uint64		ordinal;
	const struct GgcFixedBinding *parent;
	uint32		depth;
	int64		endpoint;
	int64		edge;
	Datum		payload;
	bool		payload_isnull;
	GgcEndpointHandleV2 endpoint_handle;
} GgcFixedBinding;

typedef struct GgcFixedFrontier
{
	GgcFixedBinding **items;
	uint32		count;
	uint32		capacity;
	MemoryContext context;
} GgcFixedFrontier;

typedef struct GgcFixedPayloadCopy
{
	Datum		value;
	bool		valid;
} GgcFixedPayloadCopy;

/* Private factor index reader. The Provider still owns its IndexScan and
 * validates every scope transition. A slot may borrow a pinned heap tuple;
 * this is deliberately NOT an owned GgcBatchTupleView. */
typedef struct GgcFixedDirect
{
	MemoryContext context;
	const GgcAdjProviderV2 *provider;
	const GgcIndexLeaseOps *ops;
	GgcAdjScanV2 *scan;
	GgcOpenRequestV2 request;
	GgcEndpointRequestV2 endpoint;
	GgcIndexLease lease;
	uint32 lease_consumed;
	bool lease_active;
	TupleTableSlot *slot;
	GgcFactorNode *parent;
	TupleTableSlot *cover_slot;
	Buffer cover_vm;
	bool cover_eligible;
	GgcFactorNode *parent_end;
	TimestampTz started_at;
	bool first_row;
	bool eof;
	/* Optional ordinary TableAM stream, separate from the B-tree lease. */
	Relation heap;
	IndexFetchTableData *fetch;
	GgcAdjItemV2 *candidates;
	uint32 candidate_capacity;
	uint32 candidate_count;
	uint32 candidate_pos;
	GgcProviderInstrumentation provider_counters;
	bool multi_key;
	uint32 array_count;
	uint32 array_capacity;
	uint32 array_pos;
	GgcEndpointRequestV2 *array_requests;
	GgcFactorNode **array_parents;
} GgcFixedDirect;

typedef struct GgcFactorRow
{
	GgcFactorNode *parent;
	int64 opposite;
	const GgcEndpointHandleV2 *opposite_handle;
	const GgcBatchTupleView *owned;
	TupleTableSlot *slot;
} GgcFactorRow;

typedef struct GgcPendingVertex
{
	int64 key;
	GgcEndpointHandleV2 handle;
} GgcPendingVertex;

/* Optional four-column SQL array sink. Buffers are owned by the fixed
 * execution context, never by a heap slot or a factor arena. */
typedef struct GgcFixedArrayOutput
{
	TupleTableSlot *slot;
	ExprContext *econtext;
	ArrayType *vertices;
	ArrayType *edges;
	GgcFixedTextArrayState payload_array;
	bool array_layout_valid;
	uint32 *array_element_sizes;
	Size *array_element_offsets;
	uint64 array_layout_updates;
	uint64 array_layout_rebuilds;
} GgcFixedArrayOutput;

struct GgcFixedState
{
	const GgcVisibleAdjOps *visible_ops;
	void *visible_context;
	MemoryContext context;
	MemoryContext binding_arena;
	Size binding_memory_limit;
	Size binding_memory_peak;
	MemoryContext arenas[2];
	PlanState  *anchor_plan;
	EState	   *estate;
	GgcOwnedFixedStage *stages;
	GgcOwnedFixedStage *reverse_stages; /* separate IN access, no vertex resources */
	uint32	   *input_positions;
	int32	   *match_positions;
	uint64	   *bound_key_rejections;
	uint32		stage_count;
	GgcFixedExecOptions options;
	GgcFixedFrontier frontier;
	int			frontier_arena;
	uint32		output_pos;
	uint32		final_parent_count;
	bool		final_batch_ready;
	bool		final_reverse;
	const GgcFixedBinding *output_parent;
	uint64		next_ordinal;
	uint64		budget_expanded_edges;
	uint64		budget_created_bindings;
	int64	   *output_vertices;
	int64	   *output_edges;
	Datum	   *output_payloads;
	bool	   *output_payload_nulls;
	GgcBatchState *active_batch;
	GgcFixedDirect *direct;
	uint32		active_stage;
	GgcBatchBindingView *batch_views;
	uint32		batch_view_count;
	uint32		batch_view_pos;
	GgcFixedPayloadCopy *payload_copies;
	uint32		payload_copy_capacity;
	uint64		payload_restore_generation;
	Snapshot	startup_snapshot;
	CommandId	startup_command_id;
	Oid			startup_role;
	int			startup_security_context;
	ResourceOwner startup_resource_owner;
	uint64		startup_metadata_reuses;
	bool		timing;
	bool		anchor_exhausted;
	bool		finished;
	bool		failed;
	bool		has_local_filters;
	GgcFixedBindingFilter *binding_filters;
	GgcFixedStageInstrumentation *stage_instrumentation;
	GgcFixedStageInstrumentation *report_stages;
	GgcFixedInstrumentation instrumentation;
	char *factor_arena;
	MemoryContext endpoint_context;
	bool factor_handles;
	Size factor_capacity;
	Size factor_used;
	Size factor_peak;
	uint64 factor_arcs;
	GgcFactorLayer *factor_layers;
	GgcFactorFrame *factor_frames;
	GgcFactorNode *factor_final_node;
	GgcFactorNode *factor_cursor;
	GgcBatchParent *factor_parents;
	GgcFactorNode **factor_parent_nodes;
	uint32 factor_parent_capacity;
	uint32 factor_parent_count;
	int factor_depth;
	bool factor_ready;
	bool factor_final_open;
	bool factor_enumerating;
	bool endpoint_set;
	bool edge_probe_batch;
	bool vertex_probe_batch;
	bool endpoint_pipeline;
	bool edge_range_probe;
	bool vertex_range_probe;
	bool vertex_pointer_sort;
	bool btree_seek_hint;
	bool btree_batch_route;
	bool btree_int8_compare;
	GgcPendingVertex **pending_order;
	uint32 endpoint_batch_limit;
	uint32 vertex_batch_limit;
	bool edge_index_only;
	int64 endpoint_anchor;
	GgcFixedArrayOutput *array_output;
};

static GgcEndpointHandleV2 *factor_node_handle(GgcFixedState *state,
											GgcFactorNode *node);
static void factor_charge(GgcFixedState *state, uint64 count);
static void materialize_output_prefix(GgcFixedState *state,
									  const GgcFixedBinding *binding);

static bool
binding_filter_accepts(GgcFixedState *state, uint32 stage_index,
					   const GgcFixedBinding *parent,
					   const GgcBatchTupleView *row)
{
	const GgcOwnedFixedStage *stage;
	GgcFixedResult binding;
	Datum identity;
	bool isnull;

	if (!state->binding_filters || !state->binding_filters[stage_index].accept)
		return true;
	stage = &state->stages[stage_index];
	materialize_output_prefix(state, parent);
	identity = GgcBatchTupleViewGetAttr(row, stage->plan.edge_identity_attnum, &isnull);
	if (isnull)
		ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
						errmsg("visible graph edge has a null identity")));
	state->output_vertices[stage_index + 1] = DatumGetInt64(row->candidate.opposite_key);
	state->output_edges[stage_index] = GgcIntegerKeyValue(identity, stage->identity_type);
	state->output_payloads[stage_index] = (Datum) 0;
	state->output_payload_nulls[stage_index] = true;
	if (stage->plan.payload_attnum != InvalidAttrNumber)
		state->output_payloads[stage_index] = GgcBatchTupleViewGetAttr(row,
			stage->plan.payload_attnum, &state->output_payload_nulls[stage_index]);
	binding = (GgcFixedResult) {
		.binding_ordinal = 0, .prefix_ordinal = parent->ordinal,
		.hop_count = stage_index + 1,
		.vertices = state->output_vertices, .edges = state->output_edges,
		.payloads = state->output_payloads, .payload_nulls = state->output_payload_nulls};
	return state->binding_filters[stage_index].accept(
		state->binding_filters[stage_index].arg, &binding, row);
}

/* These private arenas normally have no children.  Read the allocator's
 * exact block counter in that common case; retain recursive accounting if
 * an allocation path ever introduces a child.  This is not payload sizing
 * or a sampled check: allocator overhead is still charged on every check. */
static inline Size
fixed_context_bytes(MemoryContext context)
{
	return context->firstchild ? MemoryContextMemAllocated(context, true) :
		context->mem_allocated;
}

/* work_mem limits materialized bindings/frontiers, independently of the
 * provider's bounded candidate/restore workspace.  As with PG hash tables,
 * check actual context allocation at growth boundaries; one allocation may
 * transiently cross the limit, but no further rows are emitted after ERROR.
 * This implementation does not spill and never truncates or changes plans. */
static void
check_binding_memory(GgcFixedState *state)
{
	Size bytes = fixed_context_bytes(state->binding_arena);

	bytes = add_size(bytes, fixed_context_bytes(state->arenas[0]));
	bytes = add_size(bytes, fixed_context_bytes(state->arenas[1]));
	state->binding_memory_peak = Max(state->binding_memory_peak, bytes);
	if (bytes > state->binding_memory_limit)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("fixed graph binding memory limit exceeded"),
				 errdetail("Materialized bindings and frontiers use %zu bytes; limit is %zu bytes.",
						   bytes, state->binding_memory_limit),
				 errhint("Increase work_mem or explicitly select the relational route. Graph bindings do not spill to disk.")));
}

static void
check_endpoint_memory(GgcFixedState *state)
{
	MemoryContext context = state->endpoint_context;
	Size bytes = context->mem_allocated;

	/* Only the two live frontier contexts are normally children here.  Do
	 * not walk the context tree for every inserted endpoint, but still count
	 * any descendants should a hash implementation acquire them later. */
	for (MemoryContext child = context->firstchild; child; child = child->nextchild)
		bytes = add_size(bytes, fixed_context_bytes(child));

	state->factor_peak = Max(state->factor_peak, bytes);
	if (bytes > state->factor_capacity)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("fixed graph endpoint-set memory limit exceeded"),
				 errdetail("Live endpoint frontiers and hash tables use %zu bytes; limit is %zu bytes.",
						   bytes, state->factor_capacity),
				 errhint("Increase work_mem or explicitly select the relational route. Endpoint sets do not spill to disk.")));
}

static int
endpoint_parent_cmp(const void *a, const void *b)
{
	int64 x = (*(GgcFactorNode *const *) a)->endpoint;
	int64 y = (*(GgcFactorNode *const *) b)->endpoint;

	return (x > y) - (x < y);
}

ArrayType *
GgcFixedBuildPayloadArray(GgcFixedTextArrayState *state, const GgcFixedResult *result)
{
	uint32 last = result->hop_count - 1;
	bool new_prefix = !state->array_prefix_valid ||
		state->array_prefix_ordinal != result->prefix_ordinal;
	Size data_bytes = new_prefix ? 0 : state->array_prefix_bytes;
	Size overhead;
	Size total;
	bool hasnulls = new_prefix ? false : state->array_prefix_hasnulls;
	ArrayType *array;
	char *dest;
	uint8 *bitmap;

	for (uint32 i = new_prefix ? 0 : last; i < result->hop_count; i++)
	{
		struct varlena *value;

		if (i == last)
		{
			state->array_prefix_bytes = data_bytes;
			state->array_prefix_hasnulls = hasnulls;
		}
		if (result->payload_nulls[i])
		{
			hasnulls = true;
			continue;
		}
		value = (struct varlena *) DatumGetPointer(result->payloads[i]);
		if (VARATT_IS_EXTERNAL(value) || VARATT_IS_COMPRESSED(value))
			value = PG_DETOAST_DATUM_PACKED(result->payloads[i]);
		state->array_payloads[i] = PointerGetDatum(value);
		data_bytes = add_size(data_bytes, INTALIGN(VARHDRSZ + VARSIZE_ANY_EXHDR(value)));
	}
	overhead = hasnulls ? ARR_OVERHEAD_WITHNULLS(1, result->hop_count) :
		ARR_OVERHEAD_NONULLS(1);
	total = add_size(overhead, data_bytes);
	if (!AllocSizeIsValid(total))
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						errmsg("graph payload array exceeds allocation limit")));
	if (state->array_payload_capacity < total)
	{
		if (state->array_payload_buffer)
			state->array_payload_buffer = repalloc(state->array_payload_buffer, total);
		else
			state->array_payload_buffer = MemoryContextAlloc(state->array_context, total);
		state->array_payload_capacity = total;
	}
	array = state->array_payload_buffer;
	/* Changing the last value between NULL/non-NULL can change the bitmap
	 * overhead. Preserve the owned prefix before initializing the new header.
	 * A repalloc above preserves its bytes; no borrowed Datum is cached. */
	if (!new_prefix && overhead != state->array_prefix_overhead)
		memmove((char *) array + overhead,
				(char *) array + state->array_prefix_overhead,
				state->array_prefix_bytes);
	memset(array, 0, overhead);
	SET_VARSIZE(array, total);
	ARR_NDIM(array) = 1;
	array->dataoffset = hasnulls ? overhead : 0;
	ARR_ELEMTYPE(array) = TEXTOID;
	ARR_DIMS(array)[0] = result->hop_count;
	ARR_LBOUND(array)[0] = 1;
	bitmap = ARR_NULLBITMAP(array);
	dest = ARR_DATA_PTR(array) + (new_prefix ? 0 : state->array_prefix_bytes);
	for (uint32 i = 0; i < result->hop_count; i++)
	{
		struct varlena *value;
		Size length;

		if (result->payload_nulls[i])
			continue;
		if (bitmap)
			bitmap[i / 8] |= 1 << (i % 8);
		if (!new_prefix && i < last)
			continue;
		value = (struct varlena *) DatumGetPointer(state->array_payloads[i]);
		length = VARHDRSZ + VARSIZE_ANY_EXHDR(value);
		SET_VARSIZE(dest, length);
		memcpy(dest + VARHDRSZ, VARDATA_ANY(value), length - VARHDRSZ);
		memset(dest + length, 0, INTALIGN(length) - length);
		dest += INTALIGN(length);
	}
	state->array_prefix_valid = true;
	state->array_prefix_ordinal = result->prefix_ordinal;
	state->array_prefix_overhead = overhead;
	return array;
}


/* Update the owned output at the point the enumerator selects an arc.
 * This routine cannot allocate, detoast or throw. A rejected partial update
 * invalidates prefix reuse; the ordinary encoder rebuilds after Next succeeds. */
static inline void
fixed_array_update(GgcFixedState *state, uint32 index)
{
	GgcFixedArrayOutput *output = state->array_output;
	struct varlena *value;
	Size length;
	char *dest;

	if (!output || !output->array_layout_valid)
		return;
	if (state->output_payload_nulls[index])
		goto rebuild;
	value = (struct varlena *) DatumGetPointer(state->output_payloads[index]);
	if (VARATT_IS_EXTERNAL(value) || VARATT_IS_COMPRESSED(value))
		goto rebuild;
	length = VARSIZE_ANY_EXHDR(value);
	if (length + VARHDRSZ != output->array_element_sizes[index])
		goto rebuild;
	dest = ARR_DATA_PTR(output->payload_array.array_payload_buffer) +
		output->array_element_offsets[index];
	memcpy(dest + VARHDRSZ, VARDATA_ANY(value), length);
	return;

rebuild:
	output->array_layout_valid = false;
	output->payload_array.array_prefix_valid = false;
}

static void
fixed_array_store(GgcFixedState *state, const GgcFixedResult *result)
{
	GgcFixedArrayOutput *output = state->array_output;
	TupleTableSlot *slot = output->slot;
	ArrayType *array;
	MemoryContext old;

	/* Match the existing consumer: reset after a successful Next, not before
	 * reading the next edge or checking its logical budget. */
	ResetExprContext(output->econtext);
	old = MemoryContextSwitchTo(output->econtext->ecxt_per_tuple_memory);
	ExecClearTuple(slot);
	if (output->array_layout_valid)
	{
		array = output->payload_array.array_payload_buffer;
		output->payload_array.array_prefix_valid = true;
		output->payload_array.array_prefix_ordinal = result->prefix_ordinal;
		output->array_layout_updates++;
	}
	else
	{
		char *dest;

		array = GgcFixedBuildPayloadArray(&output->payload_array, result);
		output->array_layout_rebuilds++;
		if (!ARR_HASNULL(array))
		{
			if (!output->array_element_sizes)
			{
				output->array_element_sizes = MemoryContextAlloc(state->context,
					mul_size(state->stage_count, sizeof(uint32)));
				output->array_element_offsets = MemoryContextAlloc(state->context,
					mul_size(state->stage_count, sizeof(Size)));
			}
			dest = ARR_DATA_PTR(array);
			for (uint32 i = 0; i < state->stage_count; i++)
			{
				output->array_element_sizes[i] = VARSIZE(dest);
				output->array_element_offsets[i] = dest - ARR_DATA_PTR(array);
				dest += INTALIGN(output->array_element_sizes[i]);
			}
			output->array_layout_valid = true;
		}
	}
	slot->tts_values[0] = Int64GetDatum(result->vertices[0]);
	slot->tts_values[1] = PointerGetDatum(output->vertices);
	slot->tts_values[2] = PointerGetDatum(output->edges);
	slot->tts_values[3] = PointerGetDatum(array);
	memset(slot->tts_isnull, 0, 4 * sizeof(bool));
	ExecStoreVirtualTuple(slot);
	output->econtext->ecxt_scantuple = slot;
	MemoryContextSwitchTo(old);
}

static void
validate_options(PlanState *anchor_plan, EState *estate,
				 const GgcFixedStagePlan *stages, uint32 stage_count,
				 const GgcFixedExecOptions *options)
{
	TupleDesc	anchor_desc;
	Form_pg_attribute anchor_attribute;

	if (!anchor_plan || !estate || !estate->es_snapshot || !stages ||
		stage_count == 0 || !options || options->struct_size != sizeof(*options) ||
		options->version != GGC_FIXED_EXEC_VERSION ||
		options->anchor_attnum <= 0 || options->anchor_batch_size == 0 ||
		options->max_parent_batch == 0 || options->max_expanded_edges == 0 ||
		options->max_path_bindings == 0 ||
		!GgcBatchBudgetIsBounded(&options->batch_budget) ||
		options->batch_options.struct_size != sizeof(GgcBatchOptions) ||
		options->batch_options.version != GGC_BATCH_EXEC_VERSION)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid fixed graph execution request")));
	if (stage_count > PG_INT16_MAX ||
		!AllocSizeIsValid((Size) stage_count * sizeof(GgcOwnedFixedStage)))
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("fixed graph stage list exceeds allocation limits")));
	anchor_desc = ExecGetResultType(anchor_plan);
	if (!anchor_desc || options->anchor_attnum > anchor_desc->natts)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("fixed graph anchor attribute is out of range")));
	anchor_attribute = TupleDescAttr(anchor_desc, options->anchor_attnum - 1);
	if (anchor_attribute->attisdropped || !GgcIntegerKeyType(anchor_attribute->atttypid))
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("fixed graph anchor endpoint must be an integer type")));
}

static void
initialize_access_request(GgcFixedState *state,
					  const GgcFixedStagePlan *stage,
					  Oid index_relid, uint64 build_generation,
					  Oid endpoint_opfamily,
					  GgcOpenRequestV2 *request)
{
	MemSet(request, 0, sizeof(*request));
	request->struct_size = sizeof(*request);
	request->abi_version = GGC_ADJ_ABI_VERSION;
	request->estate = state->estate;
	request->scan_context = state->context;
	request->resource_owner = CurrentResourceOwner;
	request->snapshot = state->startup_snapshot;
	request->command_id = state->startup_command_id;
	request->effective_role = state->startup_role;
	request->security_context = state->startup_security_context;
	request->graph_oid = stage->graph_oid;
	request->edge_relid = stage->edge_relid;
	request->index_relid = index_relid;
	request->source_attnum = stage->source_attnum;
	request->destination_attnum = stage->destination_attnum;
	request->endpoint_type = INT8OID;
	request->endpoint_opfamily = endpoint_opfamily;
	request->endpoint_domain = stage->endpoint_domain;
	request->direction = stage->direction;
	request->required_caps = GGC_CAP_ROOT_TID | GGC_CAP_COMPLETE_CANDIDATES |
		GGC_CAP_BATCH | GGC_CAP_PAUSE_RESUME | GGC_CAP_SSI;
	if (state->options.column_acl)
		request->required_caps |= GGC_CAP_COLUMN_ACL;
	request->catalog_generation = stage->catalog_generation;
	request->build_generation = build_generation;
	request->budget = state->options.batch_budget;
}

static GgcResolvedFixedAccess
resolve_access(GgcFixedState *state, const GgcFixedStagePlan *stage,
			   const GgcFixedAccessPlan *access, Relation edge)
{
	GgcResolvedFixedAccess resolved = {0};
	GgcEndpointRequestV2 dummy = {0};
	GgcOpenRequestV2 request;
	const GgcProviderSetupOps *setup;
	Relation	index;
	AttrNumber endpoint_attnum;

	resolved.reject_reason = GGC_REJECT_SHAPE;
	if (!OidIsValid(access->index_relid))
		return resolved;

	index = try_index_open(access->index_relid, AccessShareLock);
	if (index == NULL)
		return resolved;
	resolved.provider = GgcResolveAdjProvider(index, &access->provider,
		&resolved.reject_reason);
	if (!resolved.provider)
	{
		index_close(index, AccessShareLock);
		return resolved;
	}
	endpoint_attnum = stage->direction == GGC_OUT ?
		stage->source_attnum : stage->destination_attnum;
	if (index->rd_index != NULL)
	{
		for (int i = 0; i < index->rd_index->indnkeyatts; i++)
		{
			if (index->rd_index->indkey.values[i] == endpoint_attnum)
			{
				resolved.endpoint_opfamily = index->rd_opfamily[i];
				break;
			}
		}
	}
	initialize_access_request(state, stage, access->index_relid,
		access->build_generation, resolved.endpoint_opfamily, &request);
	dummy.request_id = 1;
	dummy.key = Int64GetDatum(0);
	dummy.handle.origin = GGC_HANDLE_NONE;
	request.request_count = 1;
	request.requests = &dummy;
	setup = GgcGetProviderSetup(resolved.provider);
	resolved.valid = setup ?
		setup->validate_open(&request, edge, index, &resolved.reject_reason) :
		resolved.provider->validate(&request, &resolved.reject_reason);
	if (resolved.valid)
	{
		resolved.index_relation = index;
		resolved.index_relid = access->index_relid;
		resolved.build_generation = access->build_generation;
	}
	else
		index_close(index, AccessShareLock);
	return resolved;
}

/* Open before consuming the anchor: FKs alone do not prove MVCC visibility. */
static void
open_vertex_check(GgcFixedState *state, GgcOwnedFixedStage *owned)
{
	const GgcFixedStagePlan *plan = &owned->plan;
	Relation rel;
	Relation idx;
	Form_pg_attribute attr;
	MemoryContext old;

	if (!OidIsValid(plan->vertex_relid))
		return;
	/* Share only immutable access metadata and a resettable scan, never a
	 * cached visibility answer. All stages use the same query snapshot/role. */
	for (GgcOwnedFixedStage *prior = state->stages; prior < owned; prior++)
	{
		if (prior->plan.vertex_relid == plan->vertex_relid &&
			prior->plan.vertex_index_relid == plan->vertex_index_relid &&
			prior->plan.vertex_key_attnum == plan->vertex_key_attnum)
		{
			owned->vertex_owner = prior->vertex_owner;
			return;
		}
	}
	owned->vertex_owner = owned;
	owned->vertex_vm_buffer = InvalidBuffer;
	rel = table_open(plan->vertex_relid, AccessShareLock);
	owned->vertex_relation = rel;
	idx = index_open(plan->vertex_index_relid, AccessShareLock);
	owned->vertex_index = idx;
	if (rel->rd_rel->relkind != RELKIND_RELATION ||
		rel->rd_rel->relrowsecurity || rel->rd_rel->relhassubclass ||
		plan->vertex_key_attnum <= 0 ||
		plan->vertex_key_attnum > RelationGetDescr(rel)->natts ||
		idx->rd_rel->relam != BTREE_AM_OID ||
		idx->rd_index->indrelid != plan->vertex_relid ||
		!idx->rd_index->indisunique || !idx->rd_index->indimmediate ||
		!idx->rd_index->indisvalid || !idx->rd_index->indisready ||
		idx->rd_index->indnkeyatts != 1 ||
		idx->rd_index->indkey.values[0] != plan->vertex_key_attnum ||
		RelationGetIndexPredicate(idx) != NIL ||
		RelationGetIndexExpressions(idx) != NIL ||
		get_opfamily_proc(idx->rd_opfamily[0], INT8OID, INT8OID,
			BTORDER_PROC) != F_BTINT8CMP)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("unsupported fixed graph vertex visibility check")));
	attr = TupleDescAttr(RelationGetDescr(rel), plan->vertex_key_attnum - 1);
	if (attr->attisdropped || !GgcIntegerKeyType(attr->atttypid) || !attr->attnotnull ||
		idx->rd_opcintype[0] != attr->atttypid)
		ereport(ERROR, (errcode(ERRCODE_DATATYPE_MISMATCH),
						errmsg("fixed graph vertex key must be an integer type NOT NULL")));
	if (pg_class_aclcheck(plan->vertex_relid, GetUserId(), ACL_SELECT) != ACLCHECK_OK &&
		pg_attribute_aclcheck(plan->vertex_relid, plan->vertex_key_attnum,
			GetUserId(), ACL_SELECT) != ACLCHECK_OK)
		aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLE, RelationGetRelationName(rel));
	old = MemoryContextSwitchTo(state->context);
	owned->vertex_slot = table_slot_create(rel, NULL);
	/* VM is a heap contract. Other TableAMs and non-MVCC snapshots retain
	 * the ordinary table fetch path. This probe returns existence, no data. */
	owned->vertex_index_only = rel->rd_tableam == GetHeapamTableAmRoutine() &&
		IsMVCCSnapshot(state->startup_snapshot);
	MemoryContextSwitchTo(old);
}

/* Scalar, array and range probes share scan ownership and the VM pin contract.
 * Reusing an owner never reuses the preceding stage's index-only decision. */
static void
rescan_vertex(GgcFixedState *state, uint32 stage_index, ScanKey keys,
			  int nkeys, bool index_only)
{
	GgcOwnedFixedStage *owned = state->stages[stage_index].vertex_owner;
	GgcFixedStageInstrumentation *instr = &state->stage_instrumentation[stage_index];
	MemoryContext old;

	Assert(owned != NULL);
	if (owned->vertex_scan && owned->vertex_scan->numberOfKeys != nkeys)
	{
		ExecClearTuple(owned->vertex_slot);
		index_endscan(owned->vertex_scan);
		owned->vertex_scan = NULL;
	}
	if (!owned->vertex_scan)
	{
		old = MemoryContextSwitchTo(state->context);
		owned->vertex_scan = index_beginscan(owned->vertex_relation,
			owned->vertex_index, state->startup_snapshot, NULL, nkeys, 0, 0);
		instr->vertex_scan_starts++;
		MemoryContextSwitchTo(old);
	}
	/* Like IndexOnlyNext, a VM-only existence check must retain the index
	 * leaf pin until after observing the VM.  An MVCC snapshot alone cannot
	 * prevent VACUUM from recycling a dead TID and setting its page all-visible.
	 * Set this before every rescan: a shared owner may last have served a
	 * filtered stage or a batch that reset xs_want_itup on completion. */
	owned->vertex_scan->xs_want_itup = index_only;
	index_rescan(owned->vertex_scan, keys, nkeys, NULL, 0);
}

static void
release_vertex_scan(GgcOwnedFixedStage *owned)
{
	/* Retain the allocation, but never a heap/index pin across a result or
	 * edge batch. Invalidate the old position before changing scan mode. */
	ExecClearTuple(owned->vertex_slot);
	index_rescan(owned->vertex_scan, NULL, owned->vertex_scan->numberOfKeys, NULL, 0);
	owned->vertex_scan->xs_want_itup = false;
}

static bool
vertex_visible(GgcFixedState *state, uint32 stage_index, Datum key)
{
	GgcOwnedFixedStage *owned = state->stages[stage_index].vertex_owner;
	GgcFixedLocalFilter *filter = &state->stages[stage_index].filter;
	GgcFixedStageInstrumentation *instr = &state->stage_instrumentation[stage_index];
	ScanKeyData scan_key;
	bool found = false;
	bool index_only;

	if (!owned)
		return true;
	instr->vertex_probes++;
	GgcIntegerScanKey(&scan_key, owned->vertex_index->rd_opcintype[0],
		BTEqualStrategyNumber, key);
	/* VM proves existence, not values; a predicate requires the real row. */
	index_only = owned->vertex_index_only && filter->vertex == NULL;
	rescan_vertex(state, stage_index, &scan_key, 1, index_only);
	if (index_only)
	{
		ItemPointer tid;

		while ((tid = index_getnext_tid(owned->vertex_scan, ForwardScanDirection)))
		{
			CHECK_FOR_INTERRUPTS();
			/* Built-in bigint B-tree equality is exact. Never accept a lossy
			 * answer without rechecking should the access contract change. */
			if (owned->vertex_scan->xs_recheck)
				elog(ERROR, "lossy fixed graph vertex index probe");
			if (VM_ALL_VISIBLE(owned->vertex_relation,
					ItemPointerGetBlockNumber(tid), &owned->vertex_vm_buffer))
			{
				/* Same ordering and SSI contract as IndexOnlyNext: acquire the
				 * TID from the index BEFORE observing the VM bit. */
				PredicateLockPage(owned->vertex_relation,
					ItemPointerGetBlockNumber(tid), state->startup_snapshot);
				instr->vertex_index_only++;
				found = true;
				break;
			}
			instr->vertex_heap_fetches++;
			if (index_fetch_heap(owned->vertex_scan, owned->vertex_slot))
			{
				found = true;
				break;
			}
		}
	}
	else
	{
		instr->vertex_heap_fetches++;
		found = index_getnext_slot(owned->vertex_scan, ForwardScanDirection,
			owned->vertex_slot);
	}
	if (found && filter->vertex)
		found = filter->vertex(filter->vertex_arg, owned->vertex_slot);
	release_vertex_scan(owned);
	if (!found)
		state->stage_instrumentation[stage_index].vertex_rejections++;
	return found;
}

static void
close_vertex_checks(GgcFixedState *state)
{
	for (uint32 i = 0; i < state->stage_count; i++)
	{
		GgcOwnedFixedStage *owned = &state->stages[i];

		if (owned->vertex_scan)
			index_endscan(owned->vertex_scan);
		if (BufferIsValid(owned->vertex_vm_buffer))
			ReleaseBuffer(owned->vertex_vm_buffer);
		if (owned->vertex_slot)
			ExecDropSingleTupleTableSlot(owned->vertex_slot);
		if (owned->vertex_index)
			index_close(owned->vertex_index, NoLock);
		if (owned->vertex_relation)
			table_close(owned->vertex_relation, NoLock);
	}
}

static void
resolve_stage(GgcFixedState *state, GgcOwnedFixedStage *owned,
			  const GgcFixedStagePlan *plan, bool *used_fallback)
{
	Relation	edge;
	TupleDesc	desc;
	Form_pg_attribute identity;
	Form_pg_attribute source;
	Form_pg_attribute destination;
	GgcResolvedFixedAccess primary;
	GgcResolvedFixedAccess fallback = {0};
	const GgcResolvedFixedAccess *selected;

	if (!OidIsValid(plan->edge_relid) ||
		!OidIsValid(plan->primary.index_relid) ||
		(plan->has_fallback && !OidIsValid(plan->fallback.index_relid)) ||
		plan->edge_identity_attnum <= 0 ||
		plan->source_attnum <= 0 || plan->destination_attnum <= 0 ||
		(plan->direction != GGC_OUT && plan->direction != GGC_IN))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid fixed graph stage")));
	*owned = (GgcOwnedFixedStage) {.plan = *plan};
	/* If processing an invalidation interrupts resolution, do not reuse this
	 * stage's metadata. A later full resolution can establish a fresh epoch. */
	owned->startup_inval_count = SharedInvalidMessageCounter;
	edge = table_open(plan->edge_relid, AccessShareLock);
	desc = RelationGetDescr(edge);
	if (plan->edge_identity_attnum > desc->natts ||
		plan->source_attnum > desc->natts ||
		plan->destination_attnum > desc->natts ||
		(plan->payload_attnum != InvalidAttrNumber &&
		 plan->payload_attnum > desc->natts))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
				 errmsg("fixed graph stage attribute mapping is out of range")));
	identity = TupleDescAttr(desc, plan->edge_identity_attnum - 1);
	source = TupleDescAttr(desc, plan->source_attnum - 1);
	destination = TupleDescAttr(desc, plan->destination_attnum - 1);
	if (identity->attisdropped || !GgcIntegerKeyType(identity->atttypid) ||
		!identity->attnotnull)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("fixed graph edge identity must be an integer type NOT NULL")));
	if (source->attisdropped || destination->attisdropped ||
		!GgcIntegerKeyType(source->atttypid) || !GgcIntegerKeyType(destination->atttypid) ||
		!source->attnotnull || !destination->attnotnull)
		ereport(ERROR,
				(errcode(ERRCODE_DATATYPE_MISMATCH),
				 errmsg("fixed graph edge endpoints must be integer types NOT NULL")));
	owned->identity_type = identity->atttypid;
	owned->opposite_type = plan->direction == GGC_OUT ?
		destination->atttypid : source->atttypid;
	if (!GgcSelectColumnAllowed(plan->edge_relid, plan->edge_identity_attnum, GetUserId()) ||
		(plan->payload_attnum != InvalidAttrNumber &&
		 !GgcSelectColumnAllowed(plan->edge_relid, plan->payload_attnum, GetUserId())))
		aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLE, RelationGetRelationName(edge));
	if (plan->payload_attnum != InvalidAttrNumber)
	{
		Form_pg_attribute payload = TupleDescAttr(desc,
			plan->payload_attnum - 1);

		if (payload->attisdropped)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_COLUMN_REFERENCE),
					 errmsg("fixed graph payload attribute is dropped")));
		owned->payload_type = payload->atttypid;
		get_typlenbyvalalign(payload->atttypid, &owned->payload_typlen,
			&owned->payload_typbyval, &owned->payload_typalign);
	}
	primary = resolve_access(state, plan, &plan->primary, edge);
	if (plan->has_fallback)
		fallback = resolve_access(state, plan, &plan->fallback, edge);
	if (primary.valid)
	{
		selected = &primary;
		*used_fallback = false;
		if (fallback.valid)
			index_close(fallback.index_relation, AccessShareLock);
	}
	else if (plan->has_fallback && fallback.valid)
	{
		selected = &fallback;
		*used_fallback = true;
	}
	else
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("fixed graph stage has no valid startup provider"),
				 errdetail("Primary rejection: %d; fallback rejection: %d.",
						   primary.reject_reason,
						   plan->has_fallback ? fallback.reject_reason : -1)));
	owned->provider = selected->provider;
	owned->index_relid = selected->index_relid;
	owned->build_generation = selected->build_generation;
	owned->endpoint_opfamily = selected->endpoint_opfamily;
	owned->array_index_eligible = btree_value_provider(selected->provider) &&
		selected->index_relation->rd_rel->relam == BTREE_AM_OID &&
		GgcBtreeArrayIndexEligible(selected->index_relation->rd_index->indnkeyatts,
			(selected->index_relation->rd_indoption[0] & INDOPTION_DESC) != 0);
	/* Narrow opt-in: endpoint sets only consume these two plain bigint
	 * attributes. No quals, payloads or arbitrary projections may use the
	 * partial virtual slot built below. Existing security validation stands. */
	if (btree_value_provider(selected->provider) &&
		selected->index_relation->rd_rel->relam == BTREE_AM_OID &&
		plan->payload_attnum == InvalidAttrNumber)
	{
		Relation index = selected->index_relation;
		AttrNumber wanted[2] = {
			plan->direction == GGC_OUT ? plan->destination_attnum : plan->source_attnum,
			plan->edge_identity_attnum};

		for (int i = 0; i < 2; i++)
			for (int j = 0; j < index->rd_index->indnatts; j++)
				if (wanted[i] > 0 && index->rd_index->indkey.values[j] == wanted[i] &&
					GgcIntegerKeyType(TupleDescAttr(RelationGetDescr(index), j)->atttypid) &&
					index_can_return(index, j + 1))
					owned->cover_index[i] = j + 1;
	}
	/* Keep only the selected index's DDL lock through query execution. */
	index_close(selected->index_relation, NoLock);
	table_close(edge, NoLock);
}

/* Reuse only positive, opt-in metadata in this uninterrupted startup loop.
 * The first resolution retains edge/index DDL locks. Do not cache fallback
 * choices or vertex-check resources.  Vertex checks are installed separately
 * by open_vertex_check() for every stage, including a reused edge access. */
static bool
reuse_startup_metadata(GgcFixedState *state, GgcOwnedFixedStage *owned,
					   const GgcOwnedFixedStage *previous,
					   const GgcFixedStagePlan *plan)
{
	const GgcFixedStagePlan *before = &previous->plan;
	Oid			role;
	int			security_context;

	if (!(previous->provider->capabilities & GGC_CAP_STARTUP_REUSE) ||
		before->has_fallback || plan->has_fallback ||
		!GgcProviderRefEqual(&before->primary.provider, &plan->primary.provider) ||
		before->primary.index_relid != plan->primary.index_relid ||
		before->primary.build_generation != plan->primary.build_generation)
		return false;
#define SAME(f) (before->f == plan->f)
	if (!(SAME(graph_oid) && SAME(edge_relid) && SAME(edge_identity_attnum) &&
		  SAME(source_attnum) && SAME(destination_attnum) && SAME(payload_attnum) &&
		  SAME(direction) && SAME(endpoint_domain) && SAME(catalog_generation) &&
		  SAME(statistics_generation) && SAME(vertex_relid) &&
		  SAME(vertex_index_relid) && SAME(vertex_key_attnum)))
		return false;
#undef SAME
	/* Includes ACL, relcache and syscache invalidations. Accept pending
	 * messages before checking the epoch, just as relation-lock acquisition
	 * would; a change forces the ordinary, fully validated path. */
	AcceptInvalidationMessages();
	GetUserIdAndSecContext(&role, &security_context);
	if (previous->startup_inval_count != SharedInvalidMessageCounter ||
		state->estate->es_snapshot != state->startup_snapshot ||
		state->startup_snapshot->regd_count == 0 ||
		state->startup_snapshot->curcid != state->startup_command_id ||
		GetCurrentCommandId(false) != state->startup_command_id ||
		role != state->startup_role ||
		security_context != state->startup_security_context ||
		CurrentResourceOwner != state->startup_resource_owner ||
		MemoryContextGetParent(state->context) != state->estate->es_query_cxt)
		return false;
	*owned = (GgcOwnedFixedStage) {.plan = *plan};
	owned->provider = previous->provider;
	owned->index_relid = previous->index_relid;
	owned->build_generation = previous->build_generation;
	owned->endpoint_opfamily = previous->endpoint_opfamily;
	owned->identity_type = previous->identity_type;
	owned->opposite_type = previous->opposite_type;
	owned->array_index_eligible = previous->array_index_eligible;
	memcpy(owned->cover_index, previous->cover_index, sizeof(owned->cover_index));
	owned->payload_type = previous->payload_type;
	owned->payload_typlen = previous->payload_typlen;
	owned->payload_typbyval = previous->payload_typbyval;
	owned->payload_typalign = previous->payload_typalign;
	owned->startup_inval_count = previous->startup_inval_count;
	return true;
}

static uint64
allocate_binding_ordinal(GgcFixedState *state)
{
	if (state->next_ordinal == PG_UINT64_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("fixed graph binding ordinal overflow")));
	return state->next_ordinal++;
}

static void
frontier_append(GgcFixedFrontier *frontier, GgcFixedBinding *binding)
{
	MemoryContext old;

	if (frontier->count == frontier->capacity)
	{
		uint32		capacity = frontier->capacity ? frontier->capacity * 2 : 32;

		if (capacity < frontier->capacity ||
			!AllocSizeIsValid((Size) capacity * sizeof(*frontier->items)))
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("fixed graph frontier allocation overflow")));
		old = MemoryContextSwitchTo(frontier->context);
		if (frontier->items)
			frontier->items = repalloc(frontier->items,
				(Size) capacity * sizeof(*frontier->items));
		else
			frontier->items = palloc((Size) capacity * sizeof(*frontier->items));
		MemoryContextSwitchTo(old);
		frontier->capacity = capacity;
	}
	frontier->items[frontier->count++] = binding;
}

static GgcFixedBinding *
make_anchor_binding(GgcFixedState *state, int64 anchor)
{
	MemoryContext old = MemoryContextSwitchTo(state->binding_arena);
	GgcFixedBinding *binding = palloc0(sizeof(*binding));

	binding->ordinal = allocate_binding_ordinal(state);
	binding->endpoint = anchor;
	binding->payload_isnull = true;
	MemoryContextSwitchTo(old);
	return binding;
}

static Datum
copy_payload(const GgcOwnedFixedStage *stage, Datum value, bool isnull)
{
	if (isnull || stage->plan.payload_attnum == InvalidAttrNumber)
		return (Datum) 0;
	return datumCopy(value, stage->payload_typbyval, stage->payload_typlen);
}

static Datum
copy_binding_payload(GgcFixedState *state, const GgcOwnedFixedStage *stage,
					 const GgcBatchTupleView *row, Datum value, bool isnull)
{
	GgcFixedPayloadCopy *copy;

	if (isnull || stage->payload_typbyval || row->candidate_parent_count <= 1)
		return copy_payload(stage, value, isnull);
	if (row->restore_generation == 0 || row->fetched_count == 0 ||
		row->fetched_index >= row->fetched_count ||
		row->fetched_count > state->options.batch_budget.max_candidates ||
		(Size) row->fetched_count > state->options.batch_budget.max_candidate_bytes /
			sizeof(GgcAdjItemV2))
		elog(ERROR, "invalid graph payload restore identity");
	if (row->fetched_count > state->payload_copy_capacity)
	{
		if (state->payload_copies)
			pfree(state->payload_copies);
		state->payload_copies = MemoryContextAllocZero(state->context,
			mul_size(sizeof(*state->payload_copies), row->fetched_count));
		state->payload_copy_capacity = row->fetched_count;
		state->payload_restore_generation = 0;
	}
	if (state->payload_restore_generation != row->restore_generation)
	{
		memset(state->payload_copies, 0,
			   sizeof(*state->payload_copies) * row->fetched_count);
		state->payload_restore_generation = row->restore_generation;
	}
	copy = &state->payload_copies[row->fetched_index];
	if (!copy->valid)
	{
		/* One immutable copy in the anchor's binding arena, shared by all
		 * parents of this already-visible candidate. The cache itself is a
		 * dense, bounded vector, not a TID hash or cross-snapshot cache. */
		copy->value = copy_payload(stage, value, false);
		copy->valid = true;
	}
	return copy->value;
}

static GgcFixedBinding *
extend_binding(GgcFixedState *state, const GgcFixedBinding *parent,
			   uint32 stage_index,
			   const GgcBatchTupleView *row)
{
	const GgcOwnedFixedStage *stage = &state->stages[stage_index];
	MemoryContext old;
	GgcFixedBinding *child;
	bool		isnull;
	Datum		identity;
	Datum		payload = (Datum) 0;

	if (state->budget_expanded_edges >= state->options.max_expanded_edges ||
		state->budget_created_bindings >= state->options.max_path_bindings)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("fixed graph execution resource limit exceeded"),
				 errdetail("expanded edges: %llu, created bindings: %llu",
						   (unsigned long long) state->budget_expanded_edges,
						   (unsigned long long) state->budget_created_bindings)));
	identity = GgcBatchTupleViewGetAttr(row, stage->plan.edge_identity_attnum, &isnull);
	if (isnull)
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("visible graph edge has a null identity")));
	if (stage->plan.payload_attnum != InvalidAttrNumber)
		payload = GgcBatchTupleViewGetAttr(row, stage->plan.payload_attnum, &isnull);
	else
		isnull = true;
	if (!isnull && !stage->payload_typbyval &&
		datumGetSize(payload, false, stage->payload_typlen) > state->binding_memory_limit)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("fixed graph binding payload exceeds memory limit"),
				 errhint("Increase work_mem or explicitly select the relational route.")));

	old = MemoryContextSwitchTo(state->binding_arena);
	child = palloc0(sizeof(*child));
	child->ordinal = allocate_binding_ordinal(state);
	child->parent = parent;
	child->depth = stage_index + 1;
	child->endpoint = DatumGetInt64(row->candidate.opposite_key);
	child->edge = GgcIntegerKeyValue(identity, stage->identity_type);
	child->payload_isnull = isnull;
	child->payload = copy_binding_payload(state, stage, row, payload, isnull);
	child->endpoint_handle = row->candidate.opposite_handle;
	MemoryContextSwitchTo(old);

	state->budget_expanded_edges++;
	state->budget_created_bindings++;
	state->instrumentation.expanded_edges++;
	state->instrumentation.created_bindings++;
	return child;
}

static void
accumulate_batch(GgcBatchInstrumentation *target,
				 const GgcBatchInstrumentation *source)
{
#define ADD_FIELD(f) target->f += source->f
	ADD_FIELD(parent_bindings);
	ADD_FIELD(endpoint_requests);
	ADD_FIELD(shared_parent_requests);
	ADD_FIELD(endpoint_batches);
	ADD_FIELD(candidate_batches);
	ADD_FIELD(candidates_received);
	ADD_FIELD(candidate_bytes);
	ADD_FIELD(heap_page_groups);
	ADD_FIELD(prefetch_submitted);
	ADD_FIELD(prefetch_already_cached);
	ADD_FIELD(prefetch_io_started);
	ADD_FIELD(tableam_fetches);
	ADD_FIELD(visibility_rejects);
	ADD_FIELD(visibility_reuses);
	ADD_FIELD(hot_redirects);
	ADD_FIELD(restored_bindings);
	ADD_FIELD(shared_blks_hit);
	ADD_FIELD(shared_blks_read);
	ADD_FIELD(rescan_count);
	ADD_FIELD(small_batches);
	ADD_FIELD(first_row_batches);
	ADD_FIELD(provider_directory_lookups);
	ADD_FIELD(provider_ref_direct_lookups);
	ADD_FIELD(provider_ref_fallbacks);
	ADD_FIELD(provider_slot_dereferences);
	ADD_FIELD(provider_physical_candidates);
	ADD_FIELD(batch_setup_us);
	ADD_FIELD(provider_getnext_us);
	ADD_FIELD(heap_restore_us);
#undef ADD_FIELD
	target->first_row_us += source->first_row_us;
	target->peak_candidate_count = Max(target->peak_candidate_count,
		source->peak_candidate_count);
	target->peak_candidate_bytes = Max(target->peak_candidate_bytes,
		source->peak_candidate_bytes);
	target->peak_inflight_pages = Max(target->peak_inflight_pages,
		source->peak_inflight_pages);
	target->peak_pins = Max(target->peak_pins, source->peak_pins);
	target->peak_restore_bytes = Max(target->peak_restore_bytes,
		source->peak_restore_bytes);
	target->last_reason = source->last_reason;
}

static void
safe_batch_rescan(GgcFixedState *state,
				  const GgcOpenRequestV2 *request,
				  const GgcBatchParent *parents, uint32 parent_count)
{
	PG_TRY();
	{
		GgcBatchRescan(state->active_batch, request, parents, parent_count,
			&state->options.batch_options);
	}
	PG_CATCH();
	{
		/* GgcBatchRescan owns and destroys its state on every ERROR. */
		state->active_batch = NULL;
		PG_RE_THROW();
	}
	PG_END_TRY();
}

static void
finish_active_batch(GgcFixedState *state)
{
	GgcBatchInstrumentation batch;

	if (!state->active_batch)
		return;
	GgcBatchGetInstrumentation(state->active_batch, &batch);
	accumulate_batch(&state->stage_instrumentation[state->active_stage].batch, &batch);
	GgcBatchEnd(state->active_batch);
	state->active_batch = NULL;
	state->batch_view_count = state->batch_view_pos = 0;
}

static void
initialize_request(GgcFixedState *state, uint32 stage_index,
				   GgcOpenRequestV2 *request)
{
	const GgcOwnedFixedStage *stage = &state->stages[stage_index];

	initialize_access_request(state, &stage->plan, stage->index_relid,
		stage->build_generation, stage->endpoint_opfamily, request);
}

static void
finish_direct_reader(GgcFixedState *state)
{
	GgcFixedDirect *direct = state->direct;

	if (!direct)
		return;
	state->direct = NULL;
	if (direct->cover_slot)
		ExecDropSingleTupleTableSlot(direct->cover_slot);
	if (BufferIsValid(direct->cover_vm))
		ReleaseBuffer(direct->cover_vm);
	/* The slot owns a buffer pin independently of the index fetch state. */
	if (direct->slot)
		ExecDropSingleTupleTableSlot(direct->slot);
	if (direct->fetch)
		table_index_fetch_end(direct->fetch);
	if (direct->heap)
		table_close(direct->heap, NoLock);
	if (direct->scan)
		direct->provider->end(direct->scan);
	MemoryContextDelete(direct->context);
}

static bool
direct_scope_equal(const GgcFixedDirect *direct,
				   const GgcAdjProviderV2 *provider,
				   const GgcOpenRequestV2 *next)
{
	const GgcOpenRequestV2 *previous = &direct->request;

	if (direct->provider != provider ||
		MemoryContextGetParent(direct->context) != next->scan_context ||
		previous->scan_context != direct->context)
		return false;
#define SAME(f) (previous->f == next->f)
	return SAME(struct_size) && SAME(abi_version) && SAME(estate) &&
		SAME(resource_owner) && SAME(snapshot) && SAME(command_id) &&
		SAME(effective_role) && SAME(security_context) && SAME(graph_oid) &&
		SAME(edge_relid) && SAME(index_relid) && SAME(source_attnum) &&
		SAME(destination_attnum) && SAME(endpoint_type) &&
		SAME(endpoint_collation) && SAME(endpoint_opfamily) &&
		SAME(endpoint_domain) && SAME(direction) && SAME(required_caps) &&
		SAME(catalog_generation) && SAME(build_generation);
#undef SAME
}

static void
direct_set_endpoint(GgcFixedState *state, GgcFixedDirect *direct,
					uint32 stage, GgcFactorNode *parent)
{
	GgcEndpointHandleV2 *handle = factor_node_handle(state, parent);
	const GgcOwnedFixedStage *owned = &state->stages[stage];

	direct->endpoint = (GgcEndpointRequestV2) {
		.request_id = 1, .key = Int64GetDatum(parent->endpoint)};
	if (handle && GgcEndpointHandleScopeMatches(handle, &owned->provider->identity,
			owned->index_relid, owned->build_generation, owned->plan.endpoint_domain))
	{
		direct->endpoint.handle = *handle;
		state->stage_instrumentation[stage].handle_inputs++;
	}
	else if (handle && handle->origin != GGC_HANDLE_NONE)
		state->stage_instrumentation[stage].handle_scope_fallbacks++;
}

/* Only already-qualified factor stages reach here. No model-specific shape,
 * cardinality estimate or index name is used to select the singleton path. */
static bool
start_direct_reader(GgcFixedState *state, uint32 stage, GgcFactorNode *parent,
					uint32 count, GgcOpenRequestV2 *request)
{
	const GgcAdjProviderV2 *provider = state->stages[stage].provider;
	const GgcIndexLeaseOps *ops = GgcGetIndexLease(provider);
	GgcFixedDirect *direct;
	GgcBatchInstrumentation *stats = &state->stage_instrumentation[stage].batch;
	GgcRejectReason reason;
	MemoryContext old = CurrentMemoryContext;
	instr_time started;
	bool reuse;
	GgcFactorNode *node;
	uint64 inputs = 0;

	if ((!ops && !(provider->capabilities & GGC_CAP_HEAP_STREAM)) ||
		(ops && provider->get_instrumentation) ||
		state->options.batch_budget.preserve_order ||
		state->options.batch_options.first_row)
	{
		finish_direct_reader(state);
		return false;
	}
	if (state->timing)
		INSTR_TIME_SET_CURRENT_FAST(started);
	finish_active_batch(state);
	reuse = state->direct && direct_scope_equal(state->direct, provider, request);
	if (!reuse)
	{
		MemoryContext context;

		finish_direct_reader(state);
		context = AllocSetContextCreate(state->context, "fixed factor index reader",
			ALLOCSET_DEFAULT_SIZES);
		direct = MemoryContextAllocZero(context, sizeof(*direct));
		direct->context = context;
		direct->provider = provider;
		direct->ops = ops;
		/* Publish before begin so the outer fixed error guard owns partial setup. */
		state->direct = direct;
	}
	direct = state->direct;
	if (direct->lease_active)
		elog(ERROR, "cannot change factor stage with an outstanding index lease");
	if (direct->slot)
		ExecClearTuple(direct->slot);
	direct_set_endpoint(state, direct, stage, parent);
	direct->request = *request;
	direct->request.scan_context = direct->context;
	direct->request.requests = &direct->endpoint;
	direct->request.request_count = 1;
	direct->parent = parent;
	node = parent;
	for (uint32 i = 0; i < count; i++, node = node->next)
		inputs += node->paths;
	direct->parent_end = node;
	direct->cover_eligible = state->endpoint_set && state->edge_index_only &&
		state->stages[stage].cover_index[0] && state->stages[stage].cover_index[1];
	direct->multi_key = state->endpoint_set && state->edge_probe_batch &&
		state->stages[stage].array_index_eligible && count > 1 &&
		count <= state->endpoint_batch_limit && count <= request->budget.max_endpoints;
	if (direct->multi_key)
	{
		if (count > direct->array_capacity)
		{
			uint32 capacity = Max(count, GGC_FIXED_VIEW_BATCH_SIZE);

			if (direct->array_requests)
			{
				direct->array_requests = repalloc(direct->array_requests,
					capacity * sizeof(GgcEndpointRequestV2));
				direct->array_parents = repalloc(direct->array_parents,
					capacity * sizeof(GgcFactorNode *));
			}
			else
			{
				direct->array_requests = MemoryContextAlloc(direct->context,
					capacity * sizeof(GgcEndpointRequestV2));
				direct->array_parents = MemoryContextAlloc(direct->context,
					capacity * sizeof(GgcFactorNode *));
			}
			direct->array_capacity = capacity;
		}
		node = parent;
		for (uint32 i = 0; i < count; i++, node = node->next)
			direct->array_parents[i] = node;
		/* Small bounded insertion sort; singleton requests allocate nothing. */
		if (count > GGC_FIXED_VIEW_BATCH_SIZE)
			qsort(direct->array_parents, count, sizeof(GgcFactorNode *), endpoint_parent_cmp);
		else for (uint32 i = 1; i < count; i++)
		{
			GgcFactorNode *item = direct->array_parents[i];
			uint32 j = i;
			while (j && direct->array_parents[j - 1]->endpoint > item->endpoint)
			{
				direct->array_parents[j] = direct->array_parents[j - 1];
				j--;
			}
			direct->array_parents[j] = item;
		}
		for (uint32 i = 0; i < count; i++)
			direct->array_requests[i] = (GgcEndpointRequestV2) {
				.request_id = i + 1, .key = Int64GetDatum(direct->array_parents[i]->endpoint)};
		direct->array_count = count;
		direct->array_pos = 0;
		direct->request.requests = direct->array_requests;
		direct->request.request_count = count;
		state->stage_instrumentation[stage].edge_array_batches++;
	}
	direct->eof = direct->first_row = false;
	direct->candidate_count = direct->candidate_pos = 0;
	direct->started_at = GetCurrentTimestamp();
	MemoryContextSwitchTo(direct->context);
	if (reuse)
	{
		direct->provider->rescan(direct->scan, &direct->request);
		stats->rescan_count++;
	}
	else
	{
		const GgcProviderSetupOps *setup = GgcGetProviderSetup(provider);

		/* Not a cached authorization: an opted-in begin validates the live
		 * request again under the references it will actually retain. */
		if ((!setup || !setup->self_validating_begin) &&
			!provider->validate(&direct->request, &reason))
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("graph provider rejected singleton factor request (%d)", reason)));
		direct->scan = provider->begin_batch(&direct->request);
		if (!ops)
		{
			direct->heap = table_open(request->edge_relid, AccessShareLock);
			if (direct->heap->rd_rel->relkind != RELKIND_RELATION ||
				direct->heap->rd_rel->relam != HEAP_TABLE_AM_OID)
				elog(ERROR, "graph heap stream requires an ordinary heap relation");
			direct->fetch = table_index_fetch_begin(direct->heap, SO_NONE);
			direct->slot = table_slot_create(direct->heap, NULL);
			direct->candidate_capacity = Min(GGC_FIXED_VIEW_BATCH_SIZE,
				Min(request->budget.max_candidates,
					request->budget.max_candidate_bytes / sizeof(GgcAdjItemV2)));
			direct->candidates = palloc(direct->candidate_capacity * sizeof(GgcAdjItemV2));
		}
	}
	if (direct->multi_key && ops && ops->btree_values && ops->set_range_probe &&
		ops->set_range_probe(direct->scan, state->edge_range_probe))
		state->stage_instrumentation[stage].edge_range_batches++;
	MemoryContextSwitchTo(old);
	if (state->timing)
	{
		instr_time finished;

		INSTR_TIME_SET_CURRENT_FAST(finished);
		INSTR_TIME_SUBTRACT(finished, started);
		stats->batch_setup_us += INSTR_TIME_GET_MILLISEC(finished) * 1000.0;
	}
	stats->parent_bindings += count;
	stats->endpoint_requests += direct->multi_key ? count : 1;
	stats->endpoint_batches++;
	if (ops)
		state->stages[stage].direct_index_batches++;
	state->active_stage = stage;
	state->stage_instrumentation[stage].executions++;
	state->stage_instrumentation[stage].input_bindings += inputs;
	return true;
}

/* Unlike a restore group, this cursor owns no heap tuple copies or parent
 * sorting arrays. TableAM retains the HOT/MVCC/SSI behavior. The slot pins its
 * page until the next read; the fixed consumer copies retained values into
 * its budgeted arena. Provider batches contain values, never page pointers. */
static bool
next_heap_stream_row(GgcFixedState *state, GgcFactorRow *row)
{
	GgcFixedDirect *direct = state->direct;
	GgcBatchInstrumentation *stats = &state->stage_instrumentation[state->active_stage].batch;
	MemoryContext caller = CurrentMemoryContext;
	BufferUsage before = pgBufferUsage;
	BufferUsage delta = {0};
	instr_time started;
	bool found = false;
	Size bytes;

	if (direct->eof && direct->parent->next == direct->parent_end)
		return false;
	if (state->timing)
		INSTR_TIME_SET_CURRENT_FAST(started);
	ExecClearTuple(direct->slot);
	MemoryContextSwitchTo(direct->context);
	while (!found)
	{
		GgcAdjItemV2 *candidate;
		ItemPointerData visible_tid;
		bool call_again = false;
		bool all_dead = false;
		bool isnull;
		Datum opposite;

		CHECK_FOR_INTERRUPTS();
		if (direct->eof)
		{
			if (direct->parent->next == direct->parent_end)
				break;
			direct->parent = direct->parent->next;
			direct_set_endpoint(state, direct, state->active_stage, direct->parent);
			direct->provider->rescan(direct->scan, &direct->request);
			direct->first_row = direct->eof = false;
			direct->started_at = GetCurrentTimestamp();
			stats->rescan_count++;
			stats->endpoint_requests++;
			stats->endpoint_batches++;
		}
		if (direct->candidate_pos == direct->candidate_count)
		{
			int count = direct->provider->getnext_batch(direct->scan,
				direct->candidates, direct->candidate_capacity);

			if (count < 0 || count > direct->candidate_capacity)
				elog(ERROR, "graph heap stream received an invalid candidate batch");
			direct->candidate_count = count;
			direct->candidate_pos = 0;
			if (direct->provider->get_instrumentation)
			{
				GgcProviderInstrumentation current;

				direct->provider->get_instrumentation(direct->scan, &current);
#define ADD_COUNTER(f) stats->provider_##f += current.f - direct->provider_counters.f
				ADD_COUNTER(directory_lookups);
				ADD_COUNTER(ref_direct_lookups);
				ADD_COUNTER(ref_fallbacks);
				ADD_COUNTER(slot_dereferences);
				ADD_COUNTER(physical_candidates);
#undef ADD_COUNTER
				direct->provider_counters = current;
			}
			if (!count)
			{
				direct->eof = true;
				continue;
			}
			stats->candidate_batches++;
			stats->candidates_received += count;
			stats->candidate_bytes += count * sizeof(GgcAdjItemV2);
			stats->peak_candidate_count = Max(stats->peak_candidate_count, count);
			stats->peak_candidate_bytes = Max(stats->peak_candidate_bytes,
				direct->candidate_capacity * sizeof(GgcAdjItemV2));
		}
		candidate = &direct->candidates[direct->candidate_pos++];
		if (candidate->request_id != 1 ||
			candidate->edge_relid != direct->request.edge_relid ||
			!ItemPointerIsValid(&candidate->root_tid))
			elog(ERROR, "graph heap stream received an invalid heap candidate");
		visible_tid = candidate->root_tid;
		stats->tableam_fetches++;
		stats->heap_page_groups++;
		found = table_index_fetch_tuple(direct->fetch, &visible_tid,
			direct->request.snapshot, direct->slot, &call_again, &all_dead);
		if (call_again)
			elog(ERROR, "graph heap stream requires one MVCC tuple per TID");
		if (!found)
		{
			stats->visibility_rejects++;
			ExecClearTuple(direct->slot);
			continue;
		}
		opposite = slot_getattr(direct->slot, direct->request.direction == GGC_OUT ?
			direct->request.destination_attnum : direct->request.source_attnum, &isnull);
		if (isnull)
			elog(ERROR, "visible graph edge has a null opposite endpoint");
		*row = (GgcFactorRow) {.parent = direct->parent,
			.opposite = GgcIntegerKeyValue(opposite, state->stages[state->active_stage].opposite_type), .slot = direct->slot,
			.opposite_handle = &candidate->opposite_handle};
		if (!ItemPointerEquals(&candidate->root_tid, &direct->slot->tts_tid))
			stats->hot_redirects++;
		stats->restored_bindings++;
	}
	bytes = MemoryContextMemAllocated(direct->context, true);
	if (bytes > direct->request.budget.max_restore_bytes)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						errmsg("graph heap stream exceeds restore byte budget")));
	stats->peak_restore_bytes = Max(stats->peak_restore_bytes, bytes);
	/* A heap fetch state and slot, plus the provider's Slot/data pair. */
	stats->peak_pins = Max(stats->peak_pins, 4U);
	stats->peak_inflight_pages = Max(stats->peak_inflight_pages, 3U);
	MemoryContextSwitchTo(caller);
	BufferUsageAccumDiff(&delta, &pgBufferUsage, &before);
	stats->shared_blks_hit += delta.shared_blks_hit;
	stats->shared_blks_read += delta.shared_blks_read;
	if (state->timing)
	{
		instr_time finished;

		INSTR_TIME_SET_CURRENT_FAST(finished);
		INSTR_TIME_SUBTRACT(finished, started);
		stats->heap_restore_us += INSTR_TIME_GET_MILLISEC(finished) * 1000.0;
	}
	if (found && !direct->first_row)
	{
		stats->first_row_us += TimestampDifferenceMicroseconds(direct->started_at, GetCurrentTimestamp());
		direct->first_row = true;
	}
	stats->last_reason = direct->eof ? GGC_BATCH_REASON_PROVIDER_EOF : GGC_BATCH_REASON_CANDIDATE_LIMIT;
	return found;
}

/* Return one borrowed slot, without allocating a candidate/restore group.
 * Prefix construction consumes all its rows inside one GgcFixedNext call,
 * so it may share a bounded lease across internal calls here. No external
 * callback runs between those reads: prefix charging and arena copies remain
 * in the fixed executor. Final rows always return the lease before delivery.
 * EOF/capacity ends a lease before a Provider rescan; ERROR cleanup owns it. */
/* endpoints!=NULL consumes a bounded value chunk entirely inside one call.
 * Only builtin B-tree unordered endpoint sets opt in: copy bigint values,
 * never slots/heap pointers, and charge every visible candidate immediately.
 * AM memory is still checked after EVERY read, including invisible tuples;
 * only redundant pre-read checks and context/buffer-accounting transitions
 * are amortized. No security, visibility or resource check is skipped. */
static uint32
read_direct_rows(GgcFixedState *state, GgcFactorRow *row,
				 int64 *endpoints, uint32 max_values)
{
	GgcFixedDirect *direct = state->direct;
	GgcOwnedFixedStage *stage = &state->stages[state->active_stage];
	GgcFixedStageInstrumentation *instr = &state->stage_instrumentation[state->active_stage];
	GgcBatchInstrumentation *stats = &instr->batch;
	MemoryContext old = CurrentMemoryContext;
	BufferUsage started_usage = pgBufferUsage;
	BufferUsage delta_usage = {0};
	instr_time started;
	bool found = false;
	uint32 delivered = 0;
	/* Stage mapping and the request budget do not change during these
	 * synchronous reads. Parent rescans change only the endpoint key. */
	const AttrNumber opposite_attnum = direct->request.direction == GGC_OUT ?
		direct->request.destination_attnum : direct->request.source_attnum;
	const Oid opposite_type = stage->opposite_type;
	const uint32 capacity = endpoints || state->active_stage + 1 < state->stage_count ?
		Min(direct->request.budget.max_candidates, GGC_FIXED_VIEW_BATCH_SIZE) : 1;

	if (!direct->ops)
		return next_heap_stream_row(state, row);
	if (direct->eof && (direct->multi_key || direct->parent->next == direct->parent_end))
		return false;
	if (state->timing)
		INSTR_TIME_SET_CURRENT_FAST(started);
	if (endpoints && (!state->endpoint_set || !direct->multi_key ||
		!btree_value_provider(direct->provider) || !max_values))
		elog(ERROR, "invalid graph endpoint chunk request");
	instr->direct_calls++;
	if (endpoints)
		instr->endpoint_chunks++;
	if (direct->slot)
		ExecClearTuple(direct->slot);
	while (!found || (endpoints && delivered < max_values))
	{
		GgcIndexLease *lease = &direct->lease;
		IndexScanDesc scan;
		ItemPointer tid;
		uint32 consumed = 0;
		bool eof = false;
		bool new_lease = false;
		Size bytes;

		found = false;
		CHECK_FOR_INTERRUPTS();
		if (direct->eof)
		{
			if (direct->multi_key || direct->parent->next == direct->parent_end)
				break;
			/* Advance the logical parent window using the same Provider-owned
			 * scan. Only one endpoint request and one scan hold resources;
			 * there is no singleton/multi-endpoint owner handoff. */
			direct->parent = direct->parent->next;
			direct->endpoint.key = Int64GetDatum(direct->parent->endpoint);
			direct->first_row = direct->eof = false;
			direct->started_at = GetCurrentTimestamp();
			MemoryContextSwitchTo(direct->context);
			direct->provider->rescan(direct->scan, &direct->request);
			MemoryContextSwitchTo(old);
			stats->rescan_count++;
			stats->endpoint_requests++;
			stats->endpoint_batches++;
		}
		if (!direct->lease_active)
		{
			MemoryContext ancestor;

			if (!direct->ops->begin(direct->scan, capacity, direct->cover_eligible, lease))
			{
				direct->eof = true;
				continue;
			}
			direct->lease_active = true;
			new_lease = true;
			instr->lease_acquisitions++;
			direct->lease_consumed = 0;
			scan = lease->scan;
			for (ancestor = lease->am_context; ancestor;
				 ancestor = MemoryContextGetParent(ancestor))
				if (ancestor == direct->context)
					break;
			if (!scan || !lease->am_context || !ancestor ||
				lease->capacity == 0 || lease->capacity > capacity ||
				lease->capacity > PG_UINT64_MAX - lease->ordinal ||
				lease->request_id != (direct->multi_key ? 0 : 1) ||
				!scan->heapRelation || !scan->indexRelation ||
				RelationGetRelid(scan->heapRelation) != direct->request.edge_relid ||
				RelationGetRelid(scan->indexRelation) != direct->request.index_relid ||
				scan->xs_snapshot != direct->request.snapshot ||
				scan->heapRelation->rd_rel->relam != HEAP_TABLE_AM_OID ||
				scan->heapRelation->rd_rel->relkind != RELKIND_RELATION)
				elog(ERROR, "invalid bounded factor index lease");
			if (state->btree_seek_hint && direct->multi_key &&
				btree_value_provider(direct->provider))
			{
				MemoryContextSwitchTo(lease->am_context);
				btgraphseekhint(scan);
				MemoryContextSwitchTo(old);
			}
			if ((state->btree_batch_route || state->btree_int8_compare) &&
				btree_value_provider(direct->provider))
			{
				MemoryContextSwitchTo(lease->am_context);
				btgraphbatchopts(scan, state->btree_batch_route, state->btree_int8_compare);
				MemoryContextSwitchTo(old);
			}
		}
		scan = lease->scan;
		if (direct->cover_eligible && !direct->cover_slot)
		{
			MemoryContextSwitchTo(direct->context);
			direct->cover_slot = MakeSingleTupleTableSlot(
				RelationGetDescr(scan->heapRelation), &TTSOpsVirtual);
		}
		if (!direct->slot)
		{
			MemoryContextSwitchTo(direct->context);
			direct->slot = table_slot_create(scan->heapRelation, NULL);
		}
		if (!endpoints || new_lease)
		{
			bytes = MemoryContextMemAllocated(direct->context, true);
			instr->memory_checks++;
			if (bytes > direct->request.budget.max_restore_bytes)
				ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
								errmsg("singleton factor reader exceeds restore byte budget")));
		}
		MemoryContextSwitchTo(lease->am_context);
		tid = index_getnext_tid(scan, ForwardScanDirection);
		if (!tid)
			eof = true;
		else
		{
			ItemPointerData root = *tid;
			bool isnull;
			Datum opposite;
			TupleTableSlot *visible = direct->slot;

			if (direct->multi_key)
			{
				int64 key;

				if (!scan->xs_itup)
					elog(ERROR, "multi-key graph lease returned no index tuple");
				key = GgcIntegerKeyValue(index_getattr(scan->xs_itup, 1,
					scan->xs_itupdesc, &isnull), scan->indexRelation->rd_opcintype[0]);
				if (isnull)
					elog(ERROR, "multi-key graph lease returned a null key");
				while (direct->array_pos < direct->array_count &&
					direct->array_parents[direct->array_pos]->endpoint < key)
					direct->array_pos++;
				if (direct->array_pos == direct->array_count ||
					direct->array_parents[direct->array_pos]->endpoint != key)
					elog(ERROR, "multi-key graph lease returned an unexpected key");
				direct->parent = direct->array_parents[direct->array_pos];
			}
			if (!ItemPointerIsValid(&root) || scan->xs_recheck)
				elog(ERROR, "singleton factor reader requires an exact heap TID");
			consumed = 1;
			if (direct->cover_eligible && VM_ALL_VISIBLE(scan->heapRelation,
					ItemPointerGetBlockNumber(&root), &direct->cover_vm))
			{
				AttrNumber wanted[2] = {
					opposite_attnum, stage->plan.edge_identity_attnum};

				if (!scan->xs_itup || !scan->xs_itupdesc)
					elog(ERROR, "covering graph lease returned no index tuple");
				visible = direct->cover_slot;
				ExecClearTuple(visible);
				memset(visible->tts_isnull, true,
					visible->tts_tupleDescriptor->natts * sizeof(bool));
				for (int i = 0; i < 2; i++)
					visible->tts_values[wanted[i] - 1] = index_getattr(scan->xs_itup,
						stage->cover_index[i], scan->xs_itupdesc,
						&visible->tts_isnull[wanted[i] - 1]);
				visible->tts_tid = root;
				visible->tts_tableOid = RelationGetRelid(scan->heapRelation);
				ExecStoreVirtualTuple(visible);
				/* Index TID first, VM second, matching IndexOnlyNext. Updates
				 * clear VM before publishing their index changes. SSI still
				 * needs the heap-page predicate lock when no heap is read. */
				PredicateLockPage(scan->heapRelation, ItemPointerGetBlockNumber(&root),
					scan->xs_snapshot);
				instr->edge_index_only++;
				found = true;
			}
			else
			{
				/* Do not add a retained VM pin to the heap fallback budget. */
				if (BufferIsValid(direct->cover_vm))
				{
					ReleaseBuffer(direct->cover_vm);
					direct->cover_vm = InvalidBuffer;
				}
				stats->tableam_fetches++;
				instr->edge_heap_fetches++;
				stats->heap_page_groups++;
				found = index_fetch_heap(scan, direct->slot);
			}
			if (scan->xs_heap_continue)
				elog(ERROR, "singleton factor reader requires one MVCC tuple per TID");
			if (found)
			{
				opposite = slot_getattr(visible, opposite_attnum, &isnull);
				if (isnull)
					elog(ERROR, "visible graph edge has a null opposite endpoint");
				*row = (GgcFactorRow) {.parent = direct->parent,
					.opposite = GgcIntegerKeyValue(opposite, opposite_type), .slot = visible};
				if (!ItemPointerEquals(&root, &visible->tts_tid))
					stats->hot_redirects++;
				stats->restored_bindings++;
			}
			else
			{
				stats->visibility_rejects++;
				ExecClearTuple(direct->slot);
			}
		}
		if (!endpoints)
			MemoryContextSwitchTo(old);
		direct->lease_consumed += consumed;
		if (eof || direct->lease_consumed == lease->capacity)
		{
			direct->ops->finish(direct->scan, lease, direct->lease_consumed, eof);
			direct->lease_active = false;
		}
		direct->eof = eof;
		bytes = MemoryContextMemAllocated(direct->context, true);
		instr->memory_checks++;
		if (bytes > direct->request.budget.max_restore_bytes)
			ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
							errmsg("singleton factor reader exceeds restore byte budget")));
		stats->peak_restore_bytes = Max(stats->peak_restore_bytes, bytes);
		if (consumed)
		{
			stats->candidate_batches++;
			stats->candidates_received++;
			stats->candidate_bytes += sizeof(GgcAdjItemV2);
			stats->peak_candidate_count = Max(stats->peak_candidate_count, 1U);
			stats->peak_candidate_bytes = Max(stats->peak_candidate_bytes, sizeof(GgcAdjItemV2));
			stats->peak_inflight_pages = Max(stats->peak_inflight_pages, 1U);
			/* Same public B-tree traversal + heap fetch conservative bound. */
			stats->peak_pins = Max(stats->peak_pins, 4U);
		}
		if (found)
		{
			if (endpoints)
			{
				bool isnull;

				factor_charge(state, 1);
				(void) slot_getattr(row->slot,
					stage->plan.edge_identity_attnum, &isnull);
				if (isnull)
					ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
									errmsg("visible graph edge has a null identity")));
				endpoints[delivered] = row->opposite;
				ExecClearTuple(row->slot);
				instr->chunk_rows++;
			}
			delivered++;
		}
	}
	/* A chunk exposes only copied keys, never an outstanding lease. */
	if (endpoints && direct->lease_active)
	{
		direct->ops->finish(direct->scan, &direct->lease, direct->lease_consumed, false);
		direct->lease_active = false;
	}
	MemoryContextSwitchTo(old);
	BufferUsageAccumDiff(&delta_usage, &pgBufferUsage, &started_usage);
	stats->shared_blks_hit += delta_usage.shared_blks_hit;
	stats->shared_blks_read += delta_usage.shared_blks_read;
	if (state->timing)
	{
		instr_time finished;

		INSTR_TIME_SET_CURRENT_FAST(finished);
		INSTR_TIME_SUBTRACT(finished, started);
		stats->heap_restore_us += INSTR_TIME_GET_MILLISEC(finished) * 1000.0;
	}
	if (delivered && !direct->first_row)
	{
		stats->first_row_us += TimestampDifferenceMicroseconds(direct->started_at, GetCurrentTimestamp());
		direct->first_row = true;
	}
	stats->last_reason = direct->eof ? GGC_BATCH_REASON_PROVIDER_EOF : GGC_BATCH_REASON_CANDIDATE_LIMIT;
	return delivered;
}

static bool
next_direct_row(GgcFixedState *state, GgcFactorRow *row)
{
	return read_direct_rows(state, row, NULL, 1) != 0;
}

static const GgcFixedBinding *
binding_at_position(const GgcFixedBinding *binding, uint32 position)
{
	while (binding && binding->depth > position)
		binding = binding->parent;
	if (!binding || binding->depth != position)
		elog(ERROR, "invalid fixed graph binding position");
	return binding;
}

static const GgcFixedBinding *
stage_input_binding(const GgcFixedState *state, uint32 stage,
					const GgcFixedBinding *parent)
{
	return state->input_positions ?
		binding_at_position(parent, state->input_positions[stage]) : parent;
}

static bool
stage_matches_bound_key(GgcFixedState *state, uint32 stage,
						const GgcFixedBinding *parent, int64 opposite)
{
	if (state->match_positions && state->match_positions[stage] >= 0 &&
		binding_at_position(parent, state->match_positions[stage])->endpoint != opposite)
	{
		state->bound_key_rejections[stage]++;
		return false;
	}
	return true;
}

static void
start_parent_batch(GgcFixedState *state, uint32 stage_index,
				   uint32 start, uint32 count, bool reverse)
{
	GgcBatchParent *parents;
	GgcOpenRequestV2 request;
	GgcFixedStageInstrumentation *instrumentation =
		&state->stage_instrumentation[stage_index];
	MemoryContext old = MemoryContextSwitchTo(state->context);
	GgcOwnedFixedStage *access = reverse ?
		&state->reverse_stages[stage_index] : &state->stages[stage_index];

	/* A new stage/provider instance can reuse restore generation numbers.
	 * Invalidating at every parent-batch start also covers anchor-arena reset. */
	state->payload_restore_generation = 0;
	state->batch_view_count = state->batch_view_pos = 0;
	if (!state->batch_views)
		state->batch_views = palloc(sizeof(*state->batch_views) * GGC_FIXED_VIEW_BATCH_SIZE);
	parents = palloc0(sizeof(*parents) * count);
	MemoryContextSwitchTo(old);
	for (uint32 i = 0; i < count; i++)
	{
		GgcFixedBinding *binding = state->frontier.items[start + i];
		const GgcFixedBinding *input = stage_input_binding(state, stage_index, binding);

		parents[i].parent_id = i + 1;
		parents[i].parent_ordinal = binding->ordinal;
		Assert(binding->depth == stage_index);
		parents[i].key = Int64GetDatum(input->endpoint);
		if (GgcEndpointHandleScopeMatches(&input->endpoint_handle,
				&access->provider->identity, access->index_relid,
				access->build_generation, access->plan.endpoint_domain))
		{
			parents[i].handle = input->endpoint_handle;
			instrumentation->handle_inputs++;
		}
		else if (input->endpoint_handle.origin != GGC_HANDLE_NONE)
			instrumentation->handle_scope_fallbacks++;
	}
	initialize_access_request(state, &access->plan, access->index_relid,
		access->build_generation, access->endpoint_opfamily, &request);
	/* Keep only one batch. Cross-hop reuse requires identical immutable scope;
	 * a different index/direction/provider closes it and uses a fresh begin. */
	if (state->active_batch && !GgcBatchCanRescan(state->active_batch,
			access->provider, &request))
		finish_active_batch(state);
	if (state->active_batch)
	{
		if (state->active_stage != stage_index)
		{
			GgcBatchInstrumentation batch;

			GgcBatchGetInstrumentation(state->active_batch, &batch);
			accumulate_batch(&state->stage_instrumentation[state->active_stage].batch,
				&batch);
			GgcBatchResetInstrumentation(state->active_batch);
		}
		safe_batch_rescan(state, &request, parents, count);
	}
	else
		state->active_batch = GgcBatchBegin(
			access->provider, &request, parents, count,
			&state->options.batch_options);
	state->active_stage = stage_index;
	pfree(parents);
	instrumentation->executions++;
	instrumentation->input_bindings += count;
}

/* All views in one call borrow the same restore group. Do not fetch the next
 * group until the last view has been consumed. The caller owns cleanup if a
 * consumer fails; NextBindings itself destroys the batch if fetching fails. */
static inline const GgcBatchBindingView *
next_fixed_view(GgcFixedState *state)
{
	if (state->batch_view_pos >= state->batch_view_count)
	{
		state->batch_view_count = GgcBatchNextBindings(state->active_batch,
			state->batch_views, GGC_FIXED_VIEW_BATCH_SIZE);
		state->batch_view_pos = 0;
		if (state->batch_view_count == 0)
			return NULL;
	}
	return &state->batch_views[state->batch_view_pos++];
}

static void
expand_stage(GgcFixedState *state, uint32 stage_index)
{
	GgcFixedFrontier input = state->frontier;
	GgcFixedFrontier output = {0};
	MemoryContext output_context = state->arenas[1 - state->frontier_arena];
	GgcFixedStageInstrumentation *instrumentation =
		&state->stage_instrumentation[stage_index];
	bool reverse = false;
	bool undirected = state->reverse_stages &&
		OidIsValid(state->reverse_stages[stage_index].index_relid);

	MemoryContextReset(output_context);
	output.context = output_context;
	for (uint32 start = 0; start < input.count;)
	{
		uint32		count = Min(state->options.max_parent_batch,
			input.count - start);
		const GgcBatchBindingView *row;

		start_parent_batch(state, stage_index, start, count, reverse);
		/* One exception frame per parent batch, not per restored binding.
		 * GgcBatchNextBindings destroys its state if its own call raises; distinguish
		 * that case from errors in the consumer, which the outer fixed-state
		 * cleanup must still close. */
		{
			volatile bool in_batch_next = false;

			PG_TRY();
			{
				for (;;)
				{
					uint32		parent_index;
					GgcFixedBinding *child;
					instr_time	extend_started;

					in_batch_next = true;
					if ((row = next_fixed_view(state)) == NULL)
					{
						in_batch_next = false;
						break;
					}
					in_batch_next = false;

					if (row->parent_id == 0 || row->parent_id > count)
						ereport(ERROR,
								(errcode(ERRCODE_DATA_CORRUPTED),
								 errmsg("fixed graph batch returned an unknown parent")));
					parent_index = row->parent_id - 1;
					/* Both directions match a self-loop in native OR semantics,
					 * but it is one edge binding, not two. Parallel ids survive. */
					if (reverse && DatumGetInt64(row->tuple_view->candidate.opposite_key) ==
						stage_input_binding(state, stage_index,
							input.items[start + parent_index])->endpoint)
						continue;
					if (!stage_matches_bound_key(state, stage_index,
							input.items[start + parent_index],
							DatumGetInt64(row->tuple_view->candidate.opposite_key)))
						continue;
					if (state->stages[stage_index].filter.edge &&
						!state->stages[stage_index].filter.edge(
							state->stages[stage_index].filter.edge_arg, row->tuple_view))
						continue;
					if (!vertex_visible(state, stage_index,
										   row->tuple_view->candidate.opposite_key))
						continue;
					if (state->binding_filters && !binding_filter_accepts(state, stage_index,
							input.items[start + parent_index], row->tuple_view))
						continue;
					if (state->timing)
						INSTR_TIME_SET_CURRENT_FAST(extend_started);
					child = extend_binding(state,
						input.items[start + parent_index], stage_index, row->tuple_view);
					if (state->timing)
					{
						instr_time	finished;

						INSTR_TIME_SET_CURRENT_FAST(finished);
						INSTR_TIME_SUBTRACT(finished, extend_started);
						instrumentation->binding_extend_us +=
							INSTR_TIME_GET_MILLISEC(finished) * 1000.0;
					}
					frontier_append(&output, child);
					check_binding_memory(state);
					instrumentation->output_bindings++;
					CHECK_FOR_INTERRUPTS();
				}
			}
			PG_CATCH();
			{
				if (in_batch_next)
					state->active_batch = NULL;
				PG_RE_THROW();
			}
			PG_END_TRY();
		}
		if (undirected && !reverse)
			reverse = true;
		else
		{
			reverse = false;
			start += count;
		}
	}
	/* All views are consumed and binding payloads are owned copies. Leave the
	 * drained batch alive for the next compatible stage/anchor batch. */
	state->frontier = output;
	state->frontier_arena = 1 - state->frontier_arena;
	state->instrumentation.peak_frontier_bindings = Max(
		state->instrumentation.peak_frontier_bindings, output.count);
}

static bool
load_anchor_batch(GgcFixedState *state)
{
	GgcFixedFrontier anchors = {0};
	MemoryContext context = state->arenas[0];

	/* A child scan need not keep returning EOF without an explicit rescan. */
	if (state->anchor_exhausted)
		return false;
	MemoryContextReset(state->arenas[0]);
	MemoryContextReset(state->arenas[1]);
	MemoryContextReset(state->binding_arena);
	state->output_parent = NULL;
	anchors.context = context;
	for (uint32 i = 0; i < state->options.anchor_batch_size; i++)
	{
		TupleTableSlot *slot = ExecProcNode(state->anchor_plan);
		bool		isnull;
		Datum		value;

		if (TupIsNull(slot))
		{
			state->anchor_exhausted = true;
			break;
		}
		value = slot_getattr(slot, state->options.anchor_attnum, &isnull);
		if (isnull)
			ereport(ERROR,
					(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
					 errmsg("fixed graph anchor endpoint is null")));
		frontier_append(&anchors, make_anchor_binding(state,
			GgcIntegerKeyValue(value, TupleDescAttr(slot->tts_tupleDescriptor,
				state->options.anchor_attnum - 1)->atttypid)));
		check_binding_memory(state);
	}
	if (anchors.count == 0)
		return false;
	state->instrumentation.anchor_batches++;
	state->instrumentation.anchor_bindings += anchors.count;
	state->instrumentation.peak_frontier_bindings = Max(
		state->instrumentation.peak_frontier_bindings, anchors.count);
	state->frontier = anchors;
	state->frontier_arena = 0;
	state->output_pos = 0;
	/* The final stage streams into the caller.  There is no next traversal
	 * frontier to justify allocating a binding for every final result. */
	for (uint32 stage = 0; stage + 1 < state->stage_count; stage++)
	{
		expand_stage(state, stage);
		if (state->frontier.count == 0)
			break;
	}
	return true;
}

/* DAG nodes/arcs/payloads use the compact arena; alignment and superseded
 * bucket storage count against its limit.  Endpoint sets instead allocate
 * scratch here and nodes in reclaimable layer contexts.  Their shared limit
 * counts actual context allocation, including hash-table overhead. */
static void *
factor_alloc(GgcFixedState *state, Size bytes, bool zero)
{
	void *result;

	bytes = MAXALIGN(bytes);
	if (state->endpoint_context)
	{
		result = zero ? MemoryContextAllocZero(state->endpoint_context, bytes) :
			MemoryContextAlloc(state->endpoint_context, bytes);
		check_endpoint_memory(state);
		return result;
	}
	if (bytes > state->factor_capacity - state->factor_used)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						errmsg("fixed graph factorized arena limit exceeded")));
	result = state->factor_arena + state->factor_used;
	state->factor_used += bytes;
	state->factor_peak = Max(state->factor_peak, state->factor_used);
	if (zero)
		memset(result, 0, bytes);
	return result;
}

static uint32
factor_bucket(int64 endpoint)
{
	uint64 hash = (uint64) endpoint;

	hash ^= hash >> 33;
	hash *= UINT64CONST(0xff51afd7ed558ccd);
	hash ^= hash >> 33;
	return (uint32) hash;
}

/* Both representations index the stable nodes through their embedded links.
 * Endpoint sets own the index in the reclaimable layer context; DAGs retain
 * it in their arena, like the nodes and incoming arcs.  Charge a growth peak
 * before freeing the old endpoint index, including both arrays and allocator
 * overhead.  No node or endpoint handle moves when the bucket array grows. */
static void
factor_grow_buckets(GgcFixedState *state, GgcFactorLayer *layer, uint32 nbuckets)
{
	GgcFactorNode **old = layer->buckets;
	GgcFactorNode **buckets;
	Size bytes = mul_size(nbuckets, sizeof(*buckets));

	Assert(nbuckets > layer->nbuckets && (nbuckets & (nbuckets - 1)) == 0);
	if (state->endpoint_set)
	{
		if (!layer->context)
			layer->context = AllocSetContextCreate(state->endpoint_context,
				"graph endpoint frontier", ALLOCSET_SMALL_SIZES);
		buckets = MemoryContextAllocExtended(layer->context, bytes,
			MCXT_ALLOC_HUGE | MCXT_ALLOC_ZERO);
		check_endpoint_memory(state);
	}
	else
		buckets = factor_alloc(state, bytes, true);
	for (GgcFactorNode *node = layer->head; node; node = node->next)
	{
		uint32 bucket = factor_bucket(node->endpoint) & (nbuckets - 1);

		node->hash_next = buckets[bucket];
		buckets[bucket] = node;
		CHECK_FOR_INTERRUPTS();
	}
	layer->buckets = buckets;
	layer->nbuckets = nbuckets;
	if (state->endpoint_set && old)
		pfree(old);
}

/* Ordered endpoint insertion needs no index.  An interior lookup or an
 * unordered insertion makes the one-way transition to the shared node index.
 * Size it from nodes already present; do not reserve a maximum-size frontier. */
static void
endpoint_build_hash(GgcFixedState *state, GgcFactorLayer *layer)
{
	uint32 nbuckets = GGC_FACTOR_BUCKETS;

	Assert(state->endpoint_set && layer->nodes && !layer->nbuckets);
	while (nbuckets < layer->nodes)
	{
		if (nbuckets > PG_UINT32_MAX / 2)
			ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
							errmsg("graph factor hash size overflow")));
		nbuckets *= 2;
	}
	factor_grow_buckets(state, layer, nbuckets);
}

static GgcFactorNode *
factor_find_node(GgcFixedState *state, GgcFactorLayer *layer, int64 endpoint)
{
	uint32 bucket;
	GgcFactorNode *node;

	if (state->endpoint_set && !layer->nbuckets)
	{
		if (!layer->nodes || endpoint < layer->head->endpoint ||
			endpoint > layer->last->endpoint)
			return NULL;
		if (endpoint == layer->last->endpoint)
			return layer->last;
		if (endpoint == layer->head->endpoint)
			return layer->head;
		endpoint_build_hash(state, layer);
	}
	if (!layer->nbuckets)
		return NULL;
	bucket = factor_bucket(endpoint) & (layer->nbuckets - 1);
	for (node = layer->buckets[bucket]; node; node = node->hash_next)
	{
		if (node->endpoint == endpoint)
			return node;
		CHECK_FOR_INTERRUPTS();
	}
	return NULL;
}

static GgcFactorNode *
factor_node(GgcFixedState *state, GgcFactorLayer *layer, int64 endpoint)
{
	uint32 bucket;
	GgcFactorNode *node;
	Size entrysize = MAXALIGN(MAXALIGN(sizeof(*node)) +
		(state->factor_handles ? sizeof(GgcEndpointHandleV2) : 0));

	if (state->endpoint_set)
	{
		if (!layer->nodes)
		{
			node = &layer->singleton;
			node->endpoint = endpoint;
			goto link_node;
		}
		if (!layer->nbuckets && endpoint >= layer->last->endpoint)
		{
			if (endpoint == layer->last->endpoint)
				return layer->last;
			/* Every existing key is smaller: append without hashing. */
		}
		else
		{
			if (!layer->nbuckets)
				endpoint_build_hash(state, layer);
			node = factor_find_node(state, layer, endpoint);
			if (node)
				return node;
		}
	}
	else
	{
		node = factor_find_node(state, layer, endpoint);
		if (node)
			return node;
	}
	/* Keep indexed layers at at most one node per bucket on average.
	 * An ordered endpoint frontier remains unindexed until it needs lookup. */
	if ((!state->endpoint_set || layer->nbuckets) &&
		(!layer->nbuckets || layer->nodes >= layer->nbuckets))
	{
		uint32 nbuckets = layer->nbuckets ? layer->nbuckets * 2 : GGC_FACTOR_BUCKETS;

		if (nbuckets < layer->nbuckets)
			ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
							errmsg("graph factor hash size overflow")));
		factor_grow_buckets(state, layer, nbuckets);
	}
	if (state->endpoint_set)
	{
		/* Nodes die together with their layer and never move.  Allocate small
		 * zeroed chunks instead of a separate allocator chunk per node.  The
		 * entire reserved block (including unused nodes) is charged now; no
		 * allocation or unaccounted memory growth occurs between checks. */
		if (!layer->nodes_available)
		{
			if (!layer->context)
				layer->context = AllocSetContextCreate(state->endpoint_context,
					"graph endpoint frontier", ALLOCSET_SMALL_SIZES);
			layer->node_next = MemoryContextAllocZero(layer->context,
				mul_size(entrysize, GGC_ENDPOINT_NODES_PER_CHUNK));
			check_endpoint_memory(state);
			layer->nodes_available = GGC_ENDPOINT_NODES_PER_CHUNK;
		}
		node = (GgcFactorNode *) layer->node_next;
		layer->node_next += entrysize;
		layer->nodes_available--;
	}
	else
	{
		/* Only handle-producing plans pay for stable-reference storage.  Keep
		 * it in the same bounded arena after the aligned node. */
		node = factor_alloc(state, entrysize, true);
	}
	node->endpoint = endpoint;
	if (layer->nbuckets)
	{
		bucket = factor_bucket(endpoint) & (layer->nbuckets - 1);
		node->hash_next = layer->buckets[bucket];
		layer->buckets[bucket] = node;
	}
link_node:
	layer->nodes++;
	if (layer->last)
		layer->last->next = node;
	else
		layer->head = node;
	layer->last = node;
	return node;
}

/* Decode an actually consumed text value once, using only spare DAG arena.
 * This runs during final-stage enumeration, never while building prefixes.
 * A qualified direct final reader cannot later allocate parent arrays in the
 * arena: its scope/capabilities/options stay fixed until the next anchor reset.
 * Thus an optional decoded copy cannot crowd out mandatory DAG allocations.
 * If there is no room, retain the original Datum and ordinary output decoding.
 * No cross-query cache, visibility result or unbounded side allocation lives
 * here. The ordinary output detoast workspace is freed immediately after copy. */
static inline Datum
factor_flatten_payload(GgcFixedState *state, Datum payload, bool isnull)
{
	struct varlena *value;
	struct varlena *flat;
	Size bytes;
	void *copy;
	MemoryContext old;

	if (isnull || !state->array_output || !state->direct)
		return payload;
	value = (struct varlena *) DatumGetPointer(payload);
	if (!VARATT_IS_EXTERNAL(value) && !VARATT_IS_COMPRESSED(value))
		return payload;
	bytes = toast_raw_datum_size(payload);
	if (!AllocSizeIsValid(bytes) ||
		MAXALIGN(bytes) > state->factor_capacity - state->factor_used)
		return payload;
	old = MemoryContextSwitchTo(state->context);
	flat = PG_DETOAST_DATUM(payload);
	if (VARSIZE(flat) != bytes)
		elog(ERROR, "graph payload detoast size changed");
	copy = factor_alloc(state, bytes, false);
	memcpy(copy, flat, bytes);
	if (flat != value)
		pfree(flat);
	MemoryContextSwitchTo(old);
	return PointerGetDatum(copy);
}

static GgcEndpointHandleV2 *
factor_node_handle(GgcFixedState *state, GgcFactorNode *node)
{
	return state->factor_handles ?
		(GgcEndpointHandleV2 *) ((char *) node + MAXALIGN(sizeof(*node))) : NULL;
}

/* Sharing a logical endpoint must not silently hide contradictory persisted
 * references. Hints remain non-authoritative; a persisted reference wins over
 * a hint, and incompatible hints are discarded. All values are copied before
 * the bounded restore view expires. Next-stage scope checking is separate. */
static void
factor_merge_handle(GgcFixedState *state, GgcFactorNode *node,
					const GgcEndpointHandleV2 *incoming)
{
	GgcEndpointHandleV2 *saved;
	bool same_scope;
	bool same_slot;

	if (!incoming || incoming->origin == GGC_HANDLE_NONE)
		return;
	saved = factor_node_handle(state, node);
	if (!saved)
		elog(ERROR, "factor provider returned an undeclared endpoint handle");
	if (saved->origin == GGC_HANDLE_NONE)
	{
		*saved = *incoming;
		return;
	}
	same_scope = GgcEndpointHandleScopeMatches(incoming, &saved->provider,
		saved->index_relid, saved->build_generation, saved->endpoint_domain);
	same_slot = same_scope && saved->slot_id == incoming->slot_id &&
		saved->slot_generation == incoming->slot_generation;
	if (same_slot)
	{
		if (incoming->origin == GGC_HANDLE_PERSISTED_ADJACENCY)
			*saved = *incoming;
		return;
	}
	if (saved->origin == GGC_HANDLE_PERSISTED_ADJACENCY &&
		incoming->origin == GGC_HANDLE_PERSISTED_ADJACENCY)
		ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
						errmsg("factorized endpoint has conflicting persistent references")));
	if (incoming->origin == GGC_HANDLE_PERSISTED_ADJACENCY)
		*saved = *incoming;
	else if (saved->origin != GGC_HANDLE_PERSISTED_ADJACENCY)
		MemSet(saved, 0, sizeof(*saved));
}

/* Charge all logical paths, including dead prefixes. On exhaustion report
 * the same boundary counters as incremental expansion, before returning any
 * row of an intermediate stage. Final rows are charged one at a time. */
static void
factor_charge(GgcFixedState *state, uint64 count)
{
	uint64 allowed = Min(state->options.max_expanded_edges - state->budget_expanded_edges,
		state->options.max_path_bindings - state->budget_created_bindings);
	uint64 charged = Min(count, allowed);

	state->budget_expanded_edges += charged;
	state->budget_created_bindings += charged;
	state->instrumentation.expanded_edges += charged;
	state->instrumentation.created_bindings += charged;
	if (count > allowed)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("fixed graph execution resource limit exceeded"),
				 errdetail("expanded edges: %llu, created bindings: %llu",
						   (unsigned long long) state->budget_expanded_edges,
						   (unsigned long long) state->budget_created_bindings)));
}

static const GgcBatchBindingView *
factor_next_view(GgcFixedState *state)
{
	const GgcBatchBindingView *row = NULL;

	PG_TRY();
	{
		row = next_fixed_view(state);
	}
	PG_CATCH();
	{
		/* The batch Next implementation has already destroyed its state. */
		state->active_batch = NULL;
		PG_RE_THROW();
	}
	PG_END_TRY();
	return row;
}

/* Batch distinct layer nodes, not their logical prefix multiplicities. The
 * provider/batch layer still owns endpoint splitting, page grouping and MVCC.
 * Parent IDs map a returned physical edge back to its weighted DAG node.
 * Both mapping arrays (including superseded growth) count against the arena. */
static GgcFactorNode *
factor_start_batch(GgcFixedState *state, uint32 stage, GgcFactorNode *first)
{
	GgcFactorNode *node = first;
	GgcOpenRequestV2 request;
	uint32 count = 0;
	uint64 inputs = 0;
	uint32 limit = state->options.max_parent_batch;

	if (state->endpoint_set && state->edge_probe_batch &&
		state->stages[stage].array_index_eligible)
		limit = Min(limit, Min(state->endpoint_batch_limit,
			state->options.batch_budget.max_endpoints));
	while (node && count < limit)
	{
		count++;
		node = node->next;
		CHECK_FOR_INTERRUPTS();
	}
	Assert(count > 0);
	initialize_request(state, stage, &request);
	if (start_direct_reader(state, stage, first, count, &request))
		return node;
	if (count > state->factor_parent_capacity)
	{
		state->factor_parents = factor_alloc(state,
			mul_size(count, sizeof(*state->factor_parents)), false);
		state->factor_parent_nodes = factor_alloc(state,
			mul_size(count, sizeof(*state->factor_parent_nodes)), false);
		state->factor_parent_capacity = count;
	}
	memset(state->factor_parents, 0, sizeof(*state->factor_parents) * count);
	node = first;
	for (uint32 i = 0; i < count; i++, node = node->next)
	{
		GgcEndpointHandleV2 *handle = factor_node_handle(state, node);
		const GgcOwnedFixedStage *owned = &state->stages[stage];

		state->factor_parents[i].parent_id = i + 1;
		state->factor_parents[i].parent_ordinal = node->base_ordinal;
		state->factor_parents[i].key = Int64GetDatum(node->endpoint);
		if (handle && GgcEndpointHandleScopeMatches(handle, &owned->provider->identity,
				owned->index_relid, owned->build_generation, owned->plan.endpoint_domain))
		{
			state->factor_parents[i].handle = *handle;
			state->stage_instrumentation[stage].handle_inputs++;
		}
		else if (handle && handle->origin != GGC_HANDLE_NONE)
			state->stage_instrumentation[stage].handle_scope_fallbacks++;
		state->factor_parent_nodes[i] = node;
		inputs += node->paths;
	}
	state->factor_parent_count = count;
	state->batch_view_count = state->batch_view_pos = 0;
	if (!state->batch_views)
		state->batch_views = MemoryContextAlloc(state->context,
			sizeof(*state->batch_views) * GGC_FIXED_VIEW_BATCH_SIZE);
	if (state->active_batch && !GgcBatchCanRescan(state->active_batch,
			state->stages[stage].provider, &request))
		finish_active_batch(state);
	if (state->active_batch)
	{
		if (state->active_stage != stage)
		{
			GgcBatchInstrumentation batch;

			GgcBatchGetInstrumentation(state->active_batch, &batch);
			accumulate_batch(&state->stage_instrumentation[state->active_stage].batch, &batch);
			GgcBatchResetInstrumentation(state->active_batch);
		}
		safe_batch_rescan(state, &request, state->factor_parents, count);
	}
	else
		state->active_batch = GgcBatchBegin(state->stages[stage].provider,
			&request, state->factor_parents, count, &state->options.batch_options);
	if (GgcBatchEnableIndexLease(state->active_batch))
		state->stages[stage].index_lease_batches++;
	state->active_stage = stage;
	state->stage_instrumentation[stage].executions++;
	state->stage_instrumentation[stage].input_bindings += inputs;
	return node;
}

static GgcFactorNode *
factor_row_node(GgcFixedState *state, const GgcBatchBindingView *row)
{
	if (row->parent_id == 0 || row->parent_id > state->factor_parent_count)
		elog(ERROR, "factorized graph batch returned an unknown parent");
	return state->factor_parent_nodes[row->parent_id - 1];
}

static bool
factor_read_row(GgcFixedState *state, GgcFactorRow *result)
{
	const GgcBatchBindingView *row;

	if (state->direct)
		return next_direct_row(state, result);
	row = factor_next_view(state);
	if (!row)
		return false;
	*result = (GgcFactorRow) {
		.parent = factor_row_node(state, row),
		.opposite = DatumGetInt64(row->tuple_view->candidate.opposite_key),
		.opposite_handle = &row->tuple_view->candidate.opposite_handle,
		.owned = row->tuple_view};
	return true;
}

static inline Datum
factor_row_attr(const GgcFactorRow *row, AttrNumber attnum, bool *isnull)
{
	if (row->slot)
		return slot_getattr(row->slot, attnum, isnull);
	return GgcBatchTupleViewGetAttr(row->owned, attnum, isnull);
}

static bool
factor_load_anchors(GgcFixedState *state)
{
	GgcFactorLayer *root;

	if (state->anchor_exhausted)
		return false;
	state->factor_used = 0;
	state->factor_parent_capacity = state->factor_parent_count = 0;
	state->factor_parents = NULL;
	state->factor_parent_nodes = NULL;
	state->factor_layers = factor_alloc(state,
		mul_size(sizeof(GgcFactorLayer), state->stage_count), true);
	state->factor_frames = factor_alloc(state,
		mul_size(sizeof(GgcFactorFrame), state->stage_count), true);
	root = &state->factor_layers[0];
	for (uint32 i = 0; i < state->options.anchor_batch_size; i++)
	{
		TupleTableSlot *slot = ExecProcNode(state->anchor_plan);
		Datum value;
		bool isnull;
		GgcFactorNode *node;

		if (TupIsNull(slot))
		{
			state->anchor_exhausted = true;
			break;
		}
		value = slot_getattr(slot, state->options.anchor_attnum, &isnull);
		if (isnull)
			ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
							errmsg("fixed graph anchor endpoint is null")));
		node = factor_node(state, root, GgcIntegerKeyValue(value,
			TupleDescAttr(slot->tts_tupleDescriptor, state->options.anchor_attnum - 1)->atttypid));
		node->paths++;
		root->paths++;
	}
	if (!root->paths)
		return false;
	state->instrumentation.anchor_batches++;
	state->instrumentation.anchor_bindings += root->paths;
	for (uint32 depth = 0; depth < state->stage_count; depth++)
	{
		GgcFactorLayer *layer = &state->factor_layers[depth];
		const GgcOwnedFixedStage *stage = &state->stages[depth];

		state->instrumentation.peak_frontier_bindings = Max(
			state->instrumentation.peak_frontier_bindings,
			(uint32) Min(layer->paths, PG_UINT32_MAX));
		for (GgcFactorNode *node = layer->head; node; node = node->next)
		{
			if (node->paths > PG_UINT64_MAX - state->next_ordinal)
				ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
								errmsg("fixed graph binding ordinal overflow")));
			node->base_ordinal = state->next_ordinal;
			state->next_ordinal += node->paths;
		}
		if (depth + 1 == state->stage_count)
			break;
		for (GgcFactorNode *cursor = layer->head; cursor;)
		{
			GgcFactorRow row;

			cursor = factor_start_batch(state, depth, cursor);
			while (factor_read_row(state, &row))
			{
				GgcFactorLayer *output = &state->factor_layers[depth + 1];
				GgcFactorNode *node = row.parent;
				GgcFactorNode *dst;
				GgcFactorArc *arc;
				Datum edge;
				Datum payload = (Datum) 0;
				bool isnull;

				CHECK_FOR_INTERRUPTS();
				factor_charge(state, node->paths);
				edge = factor_row_attr(&row,
					stage->plan.edge_identity_attnum, &isnull);
				if (isnull)
					ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
									errmsg("visible graph edge has a null identity")));
				isnull = true;
				if (stage->plan.payload_attnum != InvalidAttrNumber)
					payload = factor_row_attr(&row,
						stage->plan.payload_attnum, &isnull);
				dst = factor_node(state, output, row.opposite);
				factor_merge_handle(state, dst, row.opposite_handle);
				arc = factor_alloc(state, sizeof(*arc), true);
				arc->prior = node;
				arc->offset = dst->paths;
				arc->edge = GgcIntegerKeyValue(edge, stage->identity_type);
				arc->isnull = isnull;
				arc->payload = payload;
				if (!isnull && !stage->payload_typbyval)
				{
					Size bytes = datumGetSize(payload, false, stage->payload_typlen);
					void *copy = factor_alloc(state, bytes, false);

					/* The eligible payload is a flat heap text value, including
					 * an ordinary on-disk TOAST pointer, never an expanded Datum. */
					memcpy(copy, DatumGetPointer(payload), bytes);
					arc->payload = PointerGetDatum(copy);
				}
				if (dst->last)
					dst->last->next = arc;
				else
					dst->incoming = arc;
				dst->last = arc;
				/* factor_charge bounded the sum before either addition. */
				dst->paths += node->paths;
				output->paths += node->paths;
				state->stage_instrumentation[depth].output_bindings += node->paths;
				state->factor_arcs++;
			}
		}
	}
	state->factor_cursor = state->factor_layers[state->stage_count - 1].head;
	state->factor_final_node = NULL;
	state->factor_ready = true;
	state->factor_final_open = state->factor_enumerating = false;
	return true;
}

/* Iterative backwards DFS over the layered DAG: no C recursion and no full
 * prefix list. Each independent root binding is emitted with its multiplicity.
 * Rank offsets identify a prefix consistently across different final edges. */
static bool
factor_prefix_next(GgcFixedState *state, uint64 *ordinal)
{
	int last = state->stage_count - 1;

	while (state->factor_depth <= last)
	{
		int depth = state->factor_depth;
		GgcFactorFrame *frame = &state->factor_frames[depth];

		CHECK_FOR_INTERRUPTS();
		if (depth == 0)
		{
			if (frame->root_pos < frame->node->paths)
			{
				uint64 rank = frame->root_pos++;

				state->output_vertices[0] = frame->node->endpoint;
				for (int i = 1; i <= last; i++)
					rank += state->factor_frames[i].chosen->offset;
				*ordinal = state->factor_final_node->base_ordinal + rank;
				return true;
			}
			state->factor_depth++;
		}
		else if (frame->next)
		{
			GgcFactorArc *arc = frame->next;
			GgcFactorFrame *prior = &state->factor_frames[depth - 1];

			frame->chosen = arc;
			frame->next = arc->next;
			state->output_vertices[depth] = frame->node->endpoint;
			state->output_edges[depth - 1] = arc->edge;
			arc->payload = factor_flatten_payload(state, arc->payload, arc->isnull);
			state->output_payloads[depth - 1] = arc->payload;
			state->output_payload_nulls[depth - 1] = arc->isnull;
			fixed_array_update(state, depth - 1);
			*prior = (GgcFactorFrame) {.node = arc->prior, .next = arc->prior->incoming};
			state->factor_depth--;
		}
		else
			state->factor_depth++;
	}
	return false;
}

static bool
factor_next(GgcFixedState *state, GgcFixedResult *result)
{
	uint32 last = state->stage_count - 1;
	const GgcOwnedFixedStage *stage = &state->stages[last];

	for (;;)
	{
		GgcFactorRow row;
		bool isnull;
		Datum edge;
		uint64 prefix;

		CHECK_FOR_INTERRUPTS();
		if (state->factor_enumerating)
		{
			if (factor_prefix_next(state, &prefix))
			{
				factor_charge(state, 1);
				result->binding_ordinal = allocate_binding_ordinal(state);
				result->prefix_ordinal = prefix;
				result->hop_count = state->stage_count;
				result->vertices = state->output_vertices;
				result->edges = state->output_edges;
				result->payloads = state->output_payloads;
				result->payload_nulls = state->output_payload_nulls;
				state->stage_instrumentation[last].output_bindings++;
				state->instrumentation.output_bindings++;
				return true;
			}
			state->factor_enumerating = false;
		}
		if (!state->factor_ready || (!state->factor_final_open && !state->factor_cursor))
		{
			if (state->finished || !factor_load_anchors(state))
			{
				finish_active_batch(state);
				finish_direct_reader(state);
				state->finished = true;
				return false;
			}
			if (!state->factor_cursor)
				continue;
		}
		if (!state->factor_final_open)
		{
			state->factor_cursor = factor_start_batch(state, last, state->factor_cursor);
			state->factor_final_open = true;
		}
		if (!factor_read_row(state, &row))
		{
			state->factor_final_open = false;
			continue;
		}
		state->factor_final_node = row.parent;
		edge = factor_row_attr(&row, stage->plan.edge_identity_attnum, &isnull);
		if (isnull)
			ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
							errmsg("visible graph edge has a null identity")));
		state->output_vertices[last + 1] = row.opposite;
		state->output_edges[last] = GgcIntegerKeyValue(edge, stage->identity_type);
		state->output_payloads[last] = (Datum) 0;
		state->output_payload_nulls[last] = true;
		if (stage->plan.payload_attnum != InvalidAttrNumber)
			state->output_payloads[last] = factor_row_attr(&row,
				stage->plan.payload_attnum, &state->output_payload_nulls[last]);
		state->output_payloads[last] = factor_flatten_payload(state,
			state->output_payloads[last], state->output_payload_nulls[last]);
		fixed_array_update(state, last);
		state->factor_frames[last] = (GgcFactorFrame) {
			.node = state->factor_final_node, .next = state->factor_final_node->incoming};
		state->factor_depth = last;
		state->factor_enumerating = true;
	}
}

static int
pending_vertex_cmp(const void *a, const void *b)
{
	int64 x = ((const GgcPendingVertex *) a)->key;
	int64 y = ((const GgcPendingVertex *) b)->key;
	return (x > y) - (x < y);
}

static int
pending_vertex_pointer_cmp(const void *a, const void *b)
{
	int64 x = (*(GgcPendingVertex *const *) a)->key;
	int64 y = (*(GgcPendingVertex *const *) b)->key;

	return (x > y) - (x < y);
}

/* Bounded, query-snapshot-local membership checks. Only a heap MVCC scan
 * with an already validated ascending unique bigint index can use arrays.
 * Repeated physical TIDs/old index entries are NOT proof of visibility. */
static void
flush_pending_vertices(GgcFixedState *state, uint32 depth,
					   GgcFactorLayer *output, GgcPendingVertex *pending, uint32 count)
{
	GgcOwnedFixedStage *owned = state->stages[depth].vertex_owner;
	GgcFixedStageInstrumentation *instr = &state->stage_instrumentation[depth];
	Datum keys[GGC_ENDPOINT_BATCH_MAX];
	bool visible[GGC_ENDPOINT_BATCH_MAX] = {false};
	uint32 nkeys = 0;
	uint32 pos = 0;
	GgcPendingVertex **order = NULL;

	if (!count)
		return;
	Assert(count <= GGC_ENDPOINT_BATCH_MAX);
	/* Sort only references; the handles retain their original ownership and
	 * every duplicate still participates in visibility/handle merging below.
	 * Scratch belongs to this anchor's bounded factor arena, not an index pin. */
	if (state->pending_order && count > 1)
	{
		order = state->pending_order;
		for (uint32 i = 0; i < count; i++)
			order[i] = &pending[i];
		qsort(order, count, sizeof(*order), pending_vertex_pointer_cmp);
		instr->vertex_pointer_sorts++;
	}
	else
		qsort(pending, count, sizeof(*pending), pending_vertex_cmp);
	for (uint32 i = 0; i < count; i++)
	{
		GgcPendingVertex *entry = order ? order[i] : &pending[i];

		if (!nkeys || entry->key != DatumGetInt64(keys[nkeys - 1]))
			keys[nkeys++] = Int64GetDatum(entry->key);
	}
	if (nkeys < 2 || !owned || !owned->vertex_index_only ||
		!GgcBtreeArrayIndexEligible(owned->vertex_index->rd_index->indnkeyatts,
			(owned->vertex_index->rd_indoption[0] & INDOPTION_DESC) != 0))
	{
		for (uint32 i = 0; i < nkeys; i++)
			visible[i] = vertex_visible(state, depth, keys[i]);
	}
	else
	{
		MemoryContext old = MemoryContextSwitchTo(state->context);
		ArrayType *array = NULL;
		ScanKeyData scan_keys[2];
		ItemPointer tid;
		bool range = state->vertex_range_probe;
		int scan_nkeys;

		for (uint32 i = 1; range && i < nkeys; i++)
		{
			int64 prior = DatumGetInt64(keys[i - 1]);
			if (prior == PG_INT64_MAX || DatumGetInt64(keys[i]) != prior + 1)
				range = false;
		}
		scan_nkeys = range ? 2 : 1;
		instr->vertex_probes += nkeys;
		instr->vertex_array_batches++;
		if (range)
		{
			GgcIntegerScanKey(&scan_keys[0], owned->vertex_index->rd_opcintype[0],
				BTGreaterEqualStrategyNumber, keys[0]);
			GgcIntegerScanKey(&scan_keys[1], owned->vertex_index->rd_opcintype[0],
				BTLessEqualStrategyNumber, keys[nkeys - 1]);
			instr->vertex_range_batches++;
		}
		else
		{
			array = construct_array(keys, nkeys, INT8OID,
				sizeof(int64), FLOAT8PASSBYVAL, TYPALIGN_DOUBLE);
			GgcIntegerScanKey(&scan_keys[0], owned->vertex_index->rd_opcintype[0],
				BTEqualStrategyNumber, PointerGetDatum(array));
			scan_keys[0].sk_flags |= SK_SEARCHARRAY;
		}
		/* The returned key and the subsequent VM check both require the
		 * index-only contract, including a leaf pin across the VM lookup. */
		rescan_vertex(state, depth, scan_keys, scan_nkeys, true);
		if (state->btree_seek_hint && !range)
			btgraphseekhint(owned->vertex_scan);
		if (state->btree_batch_route || state->btree_int8_compare)
			btgraphbatchopts(owned->vertex_scan, state->btree_batch_route,
							 state->btree_int8_compare);
		while ((tid = index_getnext_tid(owned->vertex_scan, ForwardScanDirection)))
		{
			bool isnull;
			int64 actual;

			CHECK_FOR_INTERRUPTS();
			if (owned->vertex_scan->xs_recheck || !owned->vertex_scan->xs_itup)
				elog(ERROR, "invalid graph vertex array index result");
			actual = GgcIntegerKeyValue(index_getattr(owned->vertex_scan->xs_itup, 1,
				owned->vertex_scan->xs_itupdesc, &isnull), owned->vertex_index->rd_opcintype[0]);
			while (pos < nkeys && DatumGetInt64(keys[pos]) < actual)
				pos++;
			if (isnull || pos == nkeys || DatumGetInt64(keys[pos]) != actual)
				elog(ERROR, "unexpected graph vertex array key");
			if (VM_ALL_VISIBLE(owned->vertex_relation,
					ItemPointerGetBlockNumber(tid), &owned->vertex_vm_buffer))
			{
				PredicateLockPage(owned->vertex_relation,
					ItemPointerGetBlockNumber(tid), state->startup_snapshot);
				instr->vertex_index_only++;
				visible[pos] = true;
			}
			else
			{
				instr->vertex_heap_fetches++;
				if (index_fetch_heap(owned->vertex_scan, owned->vertex_slot))
					visible[pos] = true;
				ExecClearTuple(owned->vertex_slot);
			}
		}
		release_vertex_scan(owned);
		if (array)
			pfree(array);
		MemoryContextSwitchTo(old);
		for (uint32 i = 0; i < nkeys; i++)
			if (!visible[i])
				instr->vertex_rejections++;
	}
	pos = 0;
	for (uint32 i = 0; i < count; i++)
	{
		GgcFactorNode *dst;
		GgcPendingVertex *entry = order ? order[i] : &pending[i];

		while (DatumGetInt64(keys[pos]) != entry->key)
			pos++;
		if (!visible[pos])
			continue;
		dst = factor_node(state, output, entry->key);
		factor_merge_handle(state, dst, &entry->handle);
		if (!dst->paths)
		{
			dst->paths = 1;
			output->paths++;
			instr->output_bindings++;
		}
	}
}

/* Shared endpoint insertion for the row and chunk consumers. The caller has
 * already charged/validated the physical edge. No global visited set. */
static void
consume_endpoint_key(GgcFixedState *state, uint32 depth, GgcFactorLayer *output,
					 GgcPendingVertex *pending, uint32 *pending_count,
					 int64 key, const GgcEndpointHandleV2 *handle)
{
	/* Until the first visibility batch is flushed the frontier is empty.
	 * Avoid entering the node index for a lookup that cannot succeed. */
	GgcFactorNode *dst = output->nodes ? factor_find_node(state, output, key) : NULL;

	CHECK_FOR_INTERRUPTS();
	if (!dst)
	{
		if (pending && state->stages[depth].vertex_owner)
		{
			/* Parallel edges can deliver the same unchecked endpoint in a
			 * run. One membership check suffices for this snapshot; each edge
			 * was still validated and charged by the caller. Leave handle-
			 * carrying entries separate so flush detects conflicting stable
			 * references. This queue is used only by endpoint-set execution. */
			if (*pending_count > 0 &&
				pending[*pending_count - 1].key == key &&
				pending[*pending_count - 1].handle.origin == GGC_HANDLE_NONE &&
				(!handle || handle->origin == GGC_HANDLE_NONE))
				return;
			pending[*pending_count].key = key;
			pending[*pending_count].handle = handle ? *handle : (GgcEndpointHandleV2) {0};
			if (++*pending_count == state->vertex_batch_limit)
			{
				flush_pending_vertices(state, depth, output, pending, *pending_count);
				*pending_count = 0;
			}
			return;
		}
		if (!vertex_visible(state, depth, Int64GetDatum(key)))
			return;
		dst = factor_node(state, output, key);
	}
	factor_merge_handle(state, dst, handle);
	if (!dst->paths)
	{
		dst->paths = 1;
		output->paths++;
		state->stage_instrumentation[depth].output_bindings++;
	}
}

typedef struct GgcVisibleConsumer
{
	GgcFixedState *state;
	GgcFactorLayer *output;
	uint32 depth;
} GgcVisibleConsumer;

static void
consume_visible_endpoint(void *arg, int64 endpoint)
{
	GgcVisibleConsumer *consumer = arg;
	GgcFixedState *state = consumer->state;
	GgcFactorNode *node;

	CHECK_FOR_INTERRUPTS();
	factor_charge(state, 1);
	node = factor_node(state, consumer->output, endpoint);
	if (!node->paths)
	{
		node->paths = 1;
		consumer->output->paths++;
		state->stage_instrumentation[consumer->depth].output_bindings++;
	}
}

/* Only the endpoint-set sink is eligible: its callback cannot run arbitrary
 * expressions or alter the scope certified by the visible provider. */
static GgcFactorNode *
visit_visible_batch(GgcFixedState *state, uint32 depth, GgcFactorLayer *output,
					GgcFactorNode *cursor)
{
	GgcVisibleConsumer consumer = {state, output, depth};
	GgcDirection direction = state->stages[depth].plan.direction;
	int64 endpoints[GGC_VISIBLE_ADJ_BATCH_MAX];
	uint32 limit = state->visible_ops->visit_batch ?
		Min(state->options.max_parent_batch, GGC_VISIBLE_ADJ_BATCH_MAX) : 1;
	uint32 count = 0;

	CHECK_FOR_INTERRUPTS();
	Assert(limit > 0);
	do
	{
		endpoints[count++] = cursor->endpoint;
		cursor = cursor->next;
	} while (cursor && count < limit);
	state->stage_instrumentation[depth].executions++;
	state->stage_instrumentation[depth].input_bindings += count;
	if (state->visible_ops->visit_batch)
		state->visible_ops->visit_batch(state->visible_context, endpoints,
			count, direction, consume_visible_endpoint, &consumer);
	else
		state->visible_ops->visit(state->visible_context, endpoints[0],
			direction, consume_visible_endpoint, &consumer);
	return cursor;
}

/* One independent anchor at a time. Reuse the qualified Provider/TableAM
 * readers and stable-handle validation, but never create incoming arcs or
 * multiply prefix counts. Deduplication is local to a depth: revisiting an
 * endpoint in a later layer is required by exact-k WALK semantics.
 * Only the input and output frontier survive a depth transition.  Bag/DAG
 * execution does not use this reclamation: its incoming arcs require history. */
static bool
endpoint_set_next(GgcFixedState *state, int64 *anchor, int64 *endpoint)
{
	for (;;)
	{
		TupleTableSlot *slot;
		bool isnull;
		Datum value;
		GgcFactorNode *root;
		GgcPendingVertex *pending = NULL;
		int64 *chunk = NULL;
		uint32 chunk_capacity = Min(GGC_FIXED_VIEW_BATCH_SIZE,
			state->options.batch_budget.max_candidates);

		CHECK_FOR_INTERRUPTS();
		if (state->visible_ops && !state->visible_context)
			state->visible_context = state->visible_ops->open(state->estate,
				&state->stages[0].plan);
		if (state->factor_cursor)
		{
			*anchor = state->endpoint_anchor;
			*endpoint = state->factor_cursor->endpoint;
			state->factor_cursor = state->factor_cursor->next;
			state->instrumentation.output_bindings++;
			return true;
		}
		if (state->finished)
			return false;
		/* Readers may retain parent pointers until End. Close before reusing
		 * the arena, also on an empty final layer or the next anchor. */
		finish_active_batch(state);
		finish_direct_reader(state);
		slot = ExecProcNode(state->anchor_plan);
		if (TupIsNull(slot))
		{
			state->finished = true;
			return false;
		}
		value = slot_getattr(slot, state->options.anchor_attnum, &isnull);
		if (isnull)
			ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
							errmsg("fixed graph anchor endpoint is null")));
		state->endpoint_anchor = GgcIntegerKeyValue(value,
			TupleDescAttr(slot->tts_tupleDescriptor, state->options.anchor_attnum - 1)->atttypid);
		MemoryContextReset(state->endpoint_context);
		state->factor_used = 0;
		state->factor_parent_capacity = state->factor_parent_count = 0;
		state->factor_parents = NULL;
		state->factor_parent_nodes = NULL;
		state->pending_order = NULL;
		state->factor_layers = factor_alloc(state,
			mul_size(sizeof(GgcFactorLayer), 2), true);
		if (state->vertex_probe_batch)
		{
			pending = factor_alloc(state,
				state->vertex_batch_limit * sizeof(GgcPendingVertex), false);
			if (state->vertex_pointer_sort && state->vertex_batch_limit > 1)
				state->pending_order = factor_alloc(state,
					state->vertex_batch_limit * sizeof(*state->pending_order), false);
		}
		root = factor_node(state, &state->factor_layers[0], state->endpoint_anchor);
		root->paths = state->factor_layers[0].paths = 1;
		state->instrumentation.anchor_batches++;
		state->instrumentation.anchor_bindings++;
		for (uint32 depth = 0; depth < state->stage_count; depth++)
		{
			GgcFactorLayer *input = &state->factor_layers[depth % 2];
			GgcFactorLayer *output = &state->factor_layers[(depth + 1) % 2];
			uint32 pending_count = 0;

			state->instrumentation.peak_frontier_bindings = Max(
				state->instrumentation.peak_frontier_bindings,
				(uint32) Min(input->paths, PG_UINT32_MAX));
			for (GgcFactorNode *cursor = input->head; cursor;)
			{
				GgcFactorRow row;

				if (state->visible_ops)
				{
					cursor = visit_visible_batch(state, depth, output, cursor);
					continue;
				}
				cursor = factor_start_batch(state, depth, cursor);
				if (state->endpoint_pipeline && state->direct && state->direct->multi_key &&
					btree_value_provider(state->direct->provider))
				{
					uint32 count;

					if (!chunk)
						chunk = factor_alloc(state, chunk_capacity * sizeof(int64), false);
					while ((count = read_direct_rows(state, &row, chunk, chunk_capacity)))
						for (uint32 i = 0; i < count; i++)
							consume_endpoint_key(state, depth, output, pending, &pending_count,
								chunk[i], NULL);
					continue;
				}
				while (factor_read_row(state, &row))
				{
					CHECK_FOR_INTERRUPTS();
					/* Charge visible physical candidates, including duplicates;
					 * endpoint-set execution has no logical path multiplicity. */
					factor_charge(state, 1);
					(void) factor_row_attr(&row,
						state->stages[depth].plan.edge_identity_attnum, &isnull);
					if (isnull)
						ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
										errmsg("visible graph edge has a null identity")));
					/* Only the endpoint-set route can discard multiplicity.
					 * A node already in this depth's output was checked under
					 * the same snapshot. Never cache across anchors/rescans or
					 * put an invisible vertex in the traversable frontier. */
					consume_endpoint_key(state, depth, output, pending, &pending_count,
						row.opposite, row.opposite_handle);
				}
			}
			flush_pending_vertices(state, depth, output, pending, pending_count);
			/* All candidates for this depth have been drained.  Readers own
			 * their key/handle copies, not the layer nodes.  Clear the private
			 * restoration pointers before deleting the old input context, while
			 * retaining the compatible IndexScanDesc for the next batch. */
			if (state->direct)
			{
				Assert(state->direct->eof && !state->direct->lease_active);
				state->direct->parent = state->direct->parent_end = NULL;
				if (state->direct->array_parents)
					memset(state->direct->array_parents, 0,
						state->direct->array_capacity * sizeof(GgcFactorNode *));
			}
			if (state->factor_parent_nodes)
				memset(state->factor_parent_nodes, 0,
					state->factor_parent_capacity * sizeof(GgcFactorNode *));
			state->factor_parent_count = 0;
			state->batch_view_count = state->batch_view_pos = 0;
			if (input->context)
				MemoryContextDelete(input->context);
			memset(input, 0, sizeof(*input));
		}
		state->factor_cursor = state->factor_layers[state->stage_count % 2].head;
		/* No pin/lease or borrowed heap slot is exposed to the scalar sink. */
		finish_active_batch(state);
		finish_direct_reader(state);
	}
}

bool
GgcFixedNextEndpoint(GgcFixedState *state, int64 *anchor, int64 *endpoint)
{
	bool found = false;
	MemoryContext caller = CurrentMemoryContext;

	if (!state || !state->endpoint_set || state->failed || !anchor || !endpoint)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("invalid graph endpoint-set execution state")));
	PG_TRY();
	{
		found = endpoint_set_next(state, anchor, endpoint);
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(caller);
		finish_direct_reader(state);
		if (state->active_batch)
		{
			GgcBatchEnd(state->active_batch);
			state->active_batch = NULL;
		}
		state->failed = true;
		PG_RE_THROW();
	}
	PG_END_TRY();
	return found;
}

static void
validate_execution_scope(GgcFixedState *state)
{
	Oid			role;
	int			security_context;

	GetUserIdAndSecContext(&role, &security_context);
	if (state->estate->es_snapshot != state->startup_snapshot ||
		GetCurrentCommandId(false) != state->startup_command_id ||
		role != state->startup_role ||
		security_context != state->startup_security_context)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("fixed graph execution scope changed during rescan")));
}

static void
revalidate_selected_stage(GgcFixedState *state, GgcOwnedFixedStage *stage,
						  uint32 stage_index)
{
	GgcEndpointRequestV2 dummy = {0};
	GgcOpenRequestV2 request;
	GgcRejectReason reason = GGC_ACCEPT;

	initialize_access_request(state, &stage->plan, stage->index_relid,
		stage->build_generation, stage->endpoint_opfamily, &request);
	dummy.request_id = 1;
	dummy.key = Int64GetDatum(0);
	dummy.handle.origin = GGC_HANDLE_NONE;
	request.request_count = 1;
	request.requests = &dummy;
	if (!stage->provider->validate(&request, &reason))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("fixed graph provider changed after executor startup"),
				 errdetail("Stage %u rejected its immutable execution scope (%d).",
						   stage_index + 1, reason)));
}

GgcFixedState *
GgcFixedBegin(PlanState *anchor_plan, EState *estate,
			  const GgcFixedStagePlan *stages, uint32 stage_count,
			  const GgcFixedExecOptions *options)
{
	MemoryContext context;
	GgcFixedState *state;

	validate_options(anchor_plan, estate, stages, stage_count, options);
	context = AllocSetContextCreate(estate->es_query_cxt,
		"fixed graph execution", ALLOCSET_DEFAULT_SIZES);
	state = MemoryContextAllocZero(context, sizeof(*state));
	state->context = context;
	state->anchor_plan = anchor_plan;
	state->estate = estate;
	state->stage_count = stage_count;
	state->options = *options;
	state->binding_memory_limit = mul_size((Size) work_mem, 1024);
	state->startup_snapshot = estate->es_snapshot;
	state->startup_command_id = GetCurrentCommandId(false);
	state->startup_resource_owner = CurrentResourceOwner;
	state->timing = (estate->es_instrument & INSTRUMENT_TIMER) != 0;
	GetUserIdAndSecContext(&state->startup_role,
		&state->startup_security_context);
	state->arenas[0] = AllocSetContextCreate(context,
		"fixed graph frontier arena 0", ALLOCSET_DEFAULT_SIZES);
	state->arenas[1] = AllocSetContextCreate(context,
		"fixed graph frontier arena 1", ALLOCSET_DEFAULT_SIZES);
	state->binding_arena = AllocSetContextCreate(context,
		"fixed graph compact bindings", ALLOCSET_DEFAULT_SIZES);
	state->output_vertices = MemoryContextAlloc(context,
		mul_size(sizeof(int64), stage_count + 1));
	state->output_edges = MemoryContextAlloc(context,
		mul_size(sizeof(int64), stage_count));
	state->output_payloads = MemoryContextAlloc(context,
		mul_size(sizeof(Datum), stage_count));
	state->output_payload_nulls = MemoryContextAlloc(context,
		mul_size(sizeof(bool), stage_count));
	state->stages = MemoryContextAllocZero(context,
		(Size) stage_count * sizeof(*state->stages));
	state->stage_instrumentation = MemoryContextAllocZero(context,
		(Size) stage_count * sizeof(*state->stage_instrumentation));
	state->report_stages = MemoryContextAllocZero(context,
		(Size) stage_count * sizeof(*state->report_stages));
	state->instrumentation.stage_count = stage_count;
	state->instrumentation.stages = state->stage_instrumentation;
	PG_TRY();
	{
		for (uint32 i = 0; i < stage_count; i++)
		{
			bool		used_fallback = false;

			if (i > 0 && reuse_startup_metadata(state, &state->stages[i],
											   &state->stages[i - 1], &stages[i]))
				state->startup_metadata_reuses++;
			else
			{
				resolve_stage(state, &state->stages[i], &stages[i],
					&used_fallback);
				state->instrumentation.startup_revalidations++;
			}
			open_vertex_check(state, &state->stages[i]);
			state->stage_instrumentation[i].startup_fallback = used_fallback;
			if (used_fallback)
				state->instrumentation.startup_fallbacks++;
		}
	}
	PG_CATCH();
	{
		close_vertex_checks(state);
		MemoryContextDelete(context);
		PG_RE_THROW();
	}
	PG_END_TRY();
	return state;
}

void
GgcFixedSetLocalFilters(GgcFixedState *state,
					   const GgcFixedLocalFilter *filters, uint32 stage_count)
{
	if (!state || state->failed || state->finished ||
		state->instrumentation.anchor_batches || state->factor_arena ||
		state->has_local_filters || !filters || stage_count != state->stage_count)
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						errmsg("graph local filters must be installed before bag execution")));
	/* Validate the entire request before installing any callback. */
	for (uint32 i = 0; i < stage_count; i++)
	{
		if (filters[i].edge && !GgcSelectColumnsAllowed(
			state->stages[i].plan.edge_relid,
			state->options.column_acl ? filters[i].edge_columns : NULL, GetUserId()))
			aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLE,
				get_rel_name(state->stages[i].plan.edge_relid));
		if (filters[i].vertex)
		{
			GgcOwnedFixedStage *owner = state->stages[i].vertex_owner;

			if (!owner)
				elog(ERROR, "graph vertex filter requires a vertex relation");
			if (!GgcSelectColumnsAllowed(owner->plan.vertex_relid,
				state->options.column_acl ? filters[i].vertex_columns : NULL, GetUserId()))
				aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLE,
					RelationGetRelationName(owner->vertex_relation));
		}
	}
	for (uint32 i = 0; i < stage_count; i++)
	{
		state->stages[i].filter = filters[i];
		/* Read sets are validation-only, never borrowed for later callbacks. */
		state->stages[i].filter.edge_columns = NULL;
		state->stages[i].filter.vertex_columns = NULL;
		state->has_local_filters |= filters[i].edge != NULL || filters[i].vertex != NULL;
	}
}

void
GgcFixedSetBindingFilters(GgcFixedState *state,
						 const GgcFixedBindingFilter *filters, uint32 stage_count)
{
	if (!state || state->failed || state->finished || state->factor_arena ||
		state->instrumentation.anchor_batches || state->binding_filters ||
		!filters || stage_count != state->stage_count)
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						errmsg("binding filters must be installed before bag execution")));
	state->binding_filters = MemoryContextAlloc(state->context,
		mul_size(stage_count, sizeof(*filters)));
	memcpy(state->binding_filters, filters, stage_count * sizeof(*filters));
}

void
GgcFixedSetUndirectedStages(GgcFixedState *state,
							const GgcFixedAccessPlan *reverse_access,
							uint32 stage_count)
{
	bool any = false;

	if (!state || state->failed || state->finished || state->factor_arena ||
		state->instrumentation.anchor_batches || state->reverse_stages ||
		!reverse_access || stage_count != state->stage_count)
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						errmsg("undirected stages must be installed before bag execution")));
	for (uint32 i = 0; i < stage_count; i++)
		if (OidIsValid(reverse_access[i].index_relid))
		{
			if (state->stages[i].plan.direction != GGC_OUT ||
				state->stages[i].plan.has_fallback)
				elog(ERROR, "undirected stage requires a single OUT primary access");
			any = true;
		}
	if (!any)
		return;
	state->reverse_stages = MemoryContextAllocZero(state->context,
		mul_size(stage_count, sizeof(*state->reverse_stages)));
	for (uint32 i = 0; i < stage_count; i++)
		if (OidIsValid(reverse_access[i].index_relid))
		{
			GgcFixedStagePlan reverse = state->stages[i].plan;
			bool fallback = false;

			reverse.direction = GGC_IN;
			reverse.primary = reverse_access[i];
			resolve_stage(state, &state->reverse_stages[i], &reverse, &fallback);
			state->instrumentation.startup_revalidations++;
		}
}

void
GgcFixedSetBindingPositions(GgcFixedState *state, const uint32 *inputs,
							const int32 *matches, uint32 stage_count)
{
	if (!state || state->failed || state->finished || state->factor_arena ||
		state->instrumentation.anchor_batches || state->input_positions ||
		!inputs || !matches || stage_count != state->stage_count)
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						errmsg("binding positions must be installed before bag execution")));
	for (uint32 i = 0; i < stage_count; i++)
		if (inputs[i] > i || matches[i] < -1 || matches[i] > (int32) i)
			elog(ERROR, "graph stage refers to an unbound vertex position");
	state->input_positions = MemoryContextAlloc(state->context,
		mul_size(stage_count, sizeof(*inputs)));
	state->match_positions = MemoryContextAlloc(state->context,
		mul_size(stage_count, sizeof(*matches)));
	state->bound_key_rejections = MemoryContextAllocZero(state->context,
		mul_size(stage_count, sizeof(*state->bound_key_rejections)));
	memcpy(state->input_positions, inputs, sizeof(*inputs) * stage_count);
	memcpy(state->match_positions, matches, sizeof(*matches) * stage_count);
}

uint64
GgcFixedGetBoundKeyRejections(const GgcFixedState *state, uint32 stage)
{
	if (!state || stage >= state->stage_count)
		elog(ERROR, "invalid graph bound-key instrumentation request");
	return state->bound_key_rejections ? state->bound_key_rejections[stage] : 0;
}

static void
materialize_output_prefix(GgcFixedState *state,
						   const GgcFixedBinding *binding)
{
	const GgcFixedBinding *cursor = binding;

	if (state->output_parent == binding)
		return;
	for (uint32 depth = binding->depth; depth > 0; depth--)
	{
		if (!cursor || cursor->depth != depth)
			elog(ERROR, "invalid compact fixed graph binding chain");
		state->output_vertices[depth] = cursor->endpoint;
		state->output_edges[depth - 1] = cursor->edge;
		state->output_payloads[depth - 1] = cursor->payload;
		state->output_payload_nulls[depth - 1] = cursor->payload_isnull;
		cursor = cursor->parent;
	}
	if (!cursor || cursor->depth != 0 || cursor->parent)
		elog(ERROR, "invalid compact fixed graph anchor binding");
	state->output_vertices[0] = cursor->endpoint;
	state->output_parent = binding;
}

bool
GgcFixedNext(GgcFixedState *state, GgcFixedResult *result)
{
	bool		found = false;
	volatile bool in_batch_next = false;
	uint32		stage_index;
	MemoryContext caller = CurrentMemoryContext;

	if (!state || !result || state->failed || state->endpoint_set)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid fixed graph execution state")));
	stage_index = state->stage_count - 1;
	PG_TRY();
	{
		if (state->factor_arena)
			found = factor_next(state, result);
		else
		for (;;)
		{
			const GgcBatchBindingView *row;
			const GgcOwnedFixedStage *stage = &state->stages[stage_index];
			bool isnull;
			Datum identity;
			instr_time started;

			CHECK_FOR_INTERRUPTS();
			if (!state->final_batch_ready)
			{
				if (state->output_pos >= state->frontier.count)
				{
					if (state->finished || !load_anchor_batch(state))
					{
						finish_active_batch(state);
						state->finished = true;
						break;
					}
					if (state->frontier.count == 0)
						continue;
				}
				state->final_parent_count = Min(state->options.max_parent_batch,
					state->frontier.count - state->output_pos);
				start_parent_batch(state, stage_index, state->output_pos,
					state->final_parent_count, state->final_reverse);
				state->final_batch_ready = true;
			}
			in_batch_next = true;
			if ((row = next_fixed_view(state)) == NULL)
			{
				in_batch_next = false;
				if (!state->final_reverse && state->reverse_stages &&
					OidIsValid(state->reverse_stages[stage_index].index_relid))
					state->final_reverse = true;
				else
				{
					state->final_reverse = false;
					state->output_pos += state->final_parent_count;
				}
				state->final_batch_ready = false;
				continue;
			}
			in_batch_next = false;
			if (row->parent_id == 0 || row->parent_id > state->final_parent_count)
				elog(ERROR, "fixed graph batch returned an unknown parent");
			if (state->final_reverse &&
				DatumGetInt64(row->tuple_view->candidate.opposite_key) ==
				stage_input_binding(state, stage_index,
					state->frontier.items[state->output_pos + row->parent_id - 1])->endpoint)
				continue;
			if (!stage_matches_bound_key(state, stage_index,
					state->frontier.items[state->output_pos + row->parent_id - 1],
					DatumGetInt64(row->tuple_view->candidate.opposite_key)))
				continue;
			if (stage->filter.edge &&
				!stage->filter.edge(stage->filter.edge_arg, row->tuple_view))
				continue;
			if (!vertex_visible(state, stage_index, row->tuple_view->candidate.opposite_key))
				continue;
			if (state->binding_filters && !binding_filter_accepts(state, stage_index,
					state->frontier.items[state->output_pos + row->parent_id - 1],
					row->tuple_view))
				continue;
			if (state->timing)
				INSTR_TIME_SET_CURRENT_FAST(started);
			if (state->budget_expanded_edges >= state->options.max_expanded_edges ||
				state->budget_created_bindings >= state->options.max_path_bindings)
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("fixed graph execution resource limit exceeded"),
						 errdetail("expanded edges: %llu, created bindings: %llu",
							(unsigned long long) state->budget_expanded_edges,
							(unsigned long long) state->budget_created_bindings)));
			identity = GgcBatchTupleViewGetAttr(row->tuple_view, stage->plan.edge_identity_attnum, &isnull);
			if (isnull)
				ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
								errmsg("visible graph edge has a null identity")));
			materialize_output_prefix(state,
				state->frontier.items[state->output_pos + row->parent_id - 1]);
			state->output_vertices[stage_index + 1] =
				DatumGetInt64(row->tuple_view->candidate.opposite_key);
			state->output_edges[stage_index] = GgcIntegerKeyValue(identity, stage->identity_type);
			state->output_payload_nulls[stage_index] = true;
			state->output_payloads[stage_index] = (Datum) 0;
			if (stage->plan.payload_attnum != InvalidAttrNumber)
				state->output_payloads[stage_index] = GgcBatchTupleViewGetAttr(row->tuple_view,
					stage->plan.payload_attnum, &state->output_payload_nulls[stage_index]);
			result->binding_ordinal = allocate_binding_ordinal(state);
			result->prefix_ordinal = row->parent_ordinal;
			result->hop_count = state->stage_count;
			result->vertices = state->output_vertices;
			result->edges = state->output_edges;
			result->payloads = state->output_payloads;
			result->payload_nulls = state->output_payload_nulls;
			state->budget_expanded_edges++;
			state->budget_created_bindings++;
			state->instrumentation.expanded_edges++;
			state->instrumentation.created_bindings++;
			state->stage_instrumentation[stage_index].output_bindings++;
			state->instrumentation.output_bindings++;
			if (state->timing)
			{
				instr_time finished;

				INSTR_TIME_SET_CURRENT_FAST(finished);
				INSTR_TIME_SUBTRACT(finished, started);
				state->stage_instrumentation[stage_index].binding_extend_us +=
					INSTR_TIME_GET_MILLISEC(finished) * 1000.0;
			}
			found = true;
			break;
		}
		/* Prefix leases are an entirely internal optimization. A LIMIT,
		 * external consumer, rescan or End must never see one outstanding. */
		if (state->direct && state->direct->lease_active)
			elog(ERROR, "fixed graph index lease escaped prefix construction");
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(caller);
		finish_direct_reader(state);
		if (in_batch_next)
			state->active_batch = NULL;
		if (state->active_batch)
		{
			GgcBatchEnd(state->active_batch);
			state->active_batch = NULL;
		}
		state->failed = true;
		MemoryContextReset(state->arenas[0]);
		MemoryContextReset(state->arenas[1]);
		MemoryContextReset(state->binding_arena);
		PG_RE_THROW();
	}
	PG_END_TRY();
	if (found && state->array_output)
		fixed_array_store(state, result);
	return found;
}

void
GgcFixedRescan(GgcFixedState *state)
{
	if (!state || state->failed)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid fixed graph rescan state")));
	if (state->active_batch)
		finish_active_batch(state);
	finish_direct_reader(state);
	validate_execution_scope(state);
	MemoryContextReset(state->arenas[0]);
	MemoryContextReset(state->arenas[1]);
	MemoryContextReset(state->binding_arena);
	if (state->endpoint_context)
		MemoryContextReset(state->endpoint_context);
	MemSet(&state->frontier, 0, sizeof(state->frontier));
	state->output_pos = 0;
	state->final_parent_count = 0;
	state->final_batch_ready = false;
	state->final_reverse = false;
	state->output_parent = NULL;
	state->factor_ready = state->factor_final_open = state->factor_enumerating = false;
	state->factor_final_node = NULL;
	state->factor_cursor = NULL;
	if (state->array_output)
	{
		ExecClearTuple(state->array_output->slot);
		state->array_output->payload_array.array_prefix_valid = false;
		state->array_output->array_layout_valid = false;
	}
	state->next_ordinal = 0;
	state->anchor_exhausted = false;
	state->finished = false;
	state->budget_expanded_edges = 0;
	state->budget_created_bindings = 0;
	for (uint32 i = 0; i < state->stage_count; i++)
	{
		revalidate_selected_stage(state, &state->stages[i], i);
		state->instrumentation.rescan_revalidations++;
		if (state->reverse_stages && OidIsValid(state->reverse_stages[i].index_relid))
		{
			revalidate_selected_stage(state, &state->reverse_stages[i], i);
			state->instrumentation.rescan_revalidations++;
		}
	}
	ExecReScan(state->anchor_plan);
	state->instrumentation.rescan_count++;
}

void
GgcFixedGetInstrumentation(const GgcFixedState *state,
						   GgcFixedInstrumentation *instrumentation)
{
	if (!state || !instrumentation)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid fixed graph instrumentation request")));
	*instrumentation = state->instrumentation;
	if (state->active_batch)
	{
		GgcBatchInstrumentation batch;

		memcpy(state->report_stages, state->stage_instrumentation,
			   sizeof(*state->report_stages) * state->stage_count);
		GgcBatchGetInstrumentation(state->active_batch, &batch);
		accumulate_batch(&state->report_stages[state->active_stage].batch, &batch);
		instrumentation->stages = state->report_stages;
	}
}

void
GgcFixedEnd(GgcFixedState *state)
{
	MemoryContext context;

	if (!state)
		return;
	finish_direct_reader(state);
	if (state->active_batch)
		GgcBatchEnd(state->active_batch);
	close_vertex_checks(state);
	context = state->context;
	MemoryContextDelete(context);
}

void
GgcFixedSetMemoryBudget(GgcFixedState *state, Size bytes)
{
	if (!state || state->failed || state->finished ||
		state->instrumentation.anchor_batches || bytes == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("fixed graph memory budget must be configured before execution")));
	state->binding_memory_limit = bytes;
}

Size
GgcFixedBindingPeakBytes(const GgcFixedState *state)
{
	return state ? state->binding_memory_peak : 0;
}

uint64
GgcFixedGetStartupMetadataReuses(const GgcFixedState *state)
{
	return state ? state->startup_metadata_reuses : 0;
}

uint64
GgcFixedGetIndexLeaseBatches(const GgcFixedState *state, uint32 stage)
{
	return state && stage < state->stage_count ?
		state->stages[stage].index_lease_batches : 0;
}

uint64
GgcFixedGetDirectIndexBatches(const GgcFixedState *state, uint32 stage)
{
	return state && stage < state->stage_count ?
		state->stages[stage].direct_index_batches : 0;
}

static bool
enable_factorized(GgcFixedState *state, Size arena_bytes, bool endpoint_set)
{
	Size metadata;
	bool handles = false;

	if (!state || state->failed || state->instrumentation.anchor_batches || state->finished)
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						errmsg("factorized graph mode must be selected before execution")));
	if (state->factor_arena)
		return endpoint_set == (state->endpoint_context != NULL);
	if (state->has_local_filters || state->binding_filters ||
		state->reverse_stages || state->input_positions)
		return false;
	metadata = endpoint_set ? mul_size(2, sizeof(GgcFactorLayer)) :
		mul_size(state->stage_count, sizeof(GgcFactorLayer) + sizeof(GgcFactorFrame));
	if (state->options.batch_budget.preserve_order ||
		(!endpoint_set && !AllocSizeIsValid(arena_bytes)) ||
		arena_bytes < metadata + MAXALIGN(sizeof(GgcFactorNode)))
		return false;
	for (uint32 i = 0; i < state->stage_count; i++)
	{
		const GgcOwnedFixedStage *stage = &state->stages[i];

		if (!(stage->provider->capabilities & GGC_CAP_PATH_INDEPENDENT) ||
			stage->plan.has_fallback ||
			(!endpoint_set && OidIsValid(stage->plan.vertex_relid)) ||
			(stage->plan.payload_attnum != InvalidAttrNumber &&
			 stage->payload_type != INT8OID && stage->payload_type != TEXTOID))
			return false;
		if (stage->provider->capabilities & GGC_CAP_HANDLE_OUTPUT)
			handles = true;
	}
	if (handles && arena_bytes < metadata + MAXALIGN(sizeof(GgcFactorNode)) +
		MAXALIGN(sizeof(GgcEndpointHandleV2)))
		return false;
	state->factor_handles = handles;
	if (endpoint_set)
	{
		/* factor_arena remains the mode marker; endpoint data is allocated
		 * lazily in reclaimable child contexts rather than a full-size buffer. */
		state->factor_arena = MemoryContextAlloc(state->context, 1);
		state->endpoint_context = AllocSetContextCreate(state->context,
			"graph endpoint workspace", ALLOCSET_SMALL_SIZES);
	}
	else
		state->factor_arena = MemoryContextAlloc(state->context, arena_bytes);
	state->factor_capacity = arena_bytes;
	return true;
}

bool
GgcFixedEnableFactorized(GgcFixedState *state, Size arena_bytes)
{
	return enable_factorized(state, arena_bytes, false);
}

bool
GgcFixedIsFactorized(const GgcFixedState *state)
{
	return state && state->factor_arena;
}

bool
GgcFixedEnableEndpointSet(GgcFixedState *state, Size arena_bytes)
{
	if (!state || state->array_output)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("endpoint sets cannot use a path array sink")));
	if (!enable_factorized(state, arena_bytes, true))
		return false;
	state->endpoint_set = true;
	state->edge_probe_batch = true;
	state->vertex_probe_batch = true;
	state->endpoint_batch_limit = GGC_FIXED_VIEW_BATCH_SIZE;
	state->vertex_batch_limit = GGC_FIXED_VIEW_BATCH_SIZE;
	state->edge_index_only = true;
	return true;
}

void
GgcFixedUseVisibleAdj(GgcFixedState *state)
{
	const GgcVisibleAdjOps *ops = *find_rendezvous_variable(GGC_VISIBLE_ADJ_RENDEZVOUS);
	const GgcFixedStagePlan *first;

	if (!enable_graph_age_adjacency)
		elog(ERROR, "AGE adjacency is disabled by enable_graph_age_adjacency");
	if (!state || !state->endpoint_set || state->instrumentation.anchor_bindings ||
		!ops || ops->abi != GGC_VISIBLE_ADJ_ABI || ops->struct_size != sizeof(*ops) ||
		!ops->open || !ops->visit || !ops->name)
		elog(ERROR, "visible adjacency requires a loaded, compatible provider and endpoint-set WALK plan");
	first = &state->stages[0].plan;
	if (TupleDescAttr(ExecGetResultType(state->anchor_plan),
			state->options.anchor_attnum - 1)->atttypid != INT8OID)
		elog(ERROR, "AGE adjacency currently requires bigint keys");
	for (uint32 i = 0; i < state->stage_count; i++)
	{
		const GgcFixedStagePlan *p = &state->stages[i].plan;
		if (get_atttype(p->edge_relid, p->edge_identity_attnum) != INT8OID ||
			get_atttype(p->edge_relid, p->source_attnum) != INT8OID ||
			get_atttype(p->edge_relid, p->destination_attnum) != INT8OID ||
			get_atttype(p->vertex_relid, p->vertex_key_attnum) != INT8OID)
			elog(ERROR, "AGE adjacency currently requires bigint keys");
		if (p->edge_relid != first->edge_relid || p->vertex_relid != first->vertex_relid ||
			p->source_attnum != first->source_attnum || p->destination_attnum != first->destination_attnum ||
			p->edge_identity_attnum != first->edge_identity_attnum ||
			p->vertex_key_attnum != first->vertex_key_attnum ||
			!OidIsValid(p->vertex_relid) || p->has_fallback ||
			(p->direction != GGC_OUT && p->direction != GGC_IN))
			elog(ERROR, "visible adjacency prototype requires homogeneous OUT/IN vertex and edge stages");
	}
	state->visible_ops = ops;
}

const char *
GgcFixedVisibleAdjName(const GgcFixedState *state)
{
	return state && state->visible_ops ? state->visible_ops->name : NULL;
}

void
GgcFixedSetProbeBatching(GgcFixedState *state, bool edges, bool vertices)
{
	if (!state || state->instrumentation.anchor_bindings)
		elog(ERROR, "graph probe batching must be configured before execution");
	state->edge_probe_batch = edges;
	state->vertex_probe_batch = vertices;
}

void
GgcFixedSetEndpointPipeline(GgcFixedState *state, bool enabled,
							uint32 edge_limit, uint32 vertex_limit)
{
	if (!state || !state->endpoint_set || state->instrumentation.anchor_bindings ||
		!edge_limit || edge_limit > GGC_ENDPOINT_BATCH_MAX ||
		!vertex_limit || vertex_limit > GGC_ENDPOINT_BATCH_MAX)
		elog(ERROR, "invalid graph endpoint pipeline policy");
	state->endpoint_pipeline = enabled;
	state->endpoint_batch_limit = edge_limit;
	state->vertex_batch_limit = vertex_limit;
}

void
GgcFixedSetRangeProbes(GgcFixedState *state, bool edges, bool vertices)
{
	if (!state || !state->endpoint_set || state->instrumentation.anchor_bindings)
		elog(ERROR, "graph range probes must be configured before execution");
	state->edge_range_probe = edges;
	state->vertex_range_probe = vertices;
}

void
GgcFixedSetVertexPointerSort(GgcFixedState *state, bool enabled)
{
	if (!state || !state->endpoint_set || state->instrumentation.anchor_bindings)
		elog(ERROR, "graph vertex sorting must be configured before execution");
	state->vertex_pointer_sort = enabled;
}

void
GgcFixedSetBtreeSeekHint(GgcFixedState *state, bool enabled)
{
	if (!state || !state->endpoint_set || state->instrumentation.anchor_bindings)
		elog(ERROR, "graph B-tree positioning must be configured before execution");
	state->btree_seek_hint = enabled;
}

void
GgcFixedSetBtreeBatchOptions(GgcFixedState *state, bool routes, bool int8cmp)
{
	if (!state || !state->endpoint_set || state->instrumentation.anchor_bindings)
		elog(ERROR, "graph B-tree batching must be configured before execution");
	state->btree_batch_route = routes;
	state->btree_int8_compare = int8cmp;
}

void
GgcFixedSetEdgeIndexOnly(GgcFixedState *state, bool enabled)
{
	if (!state || state->instrumentation.anchor_bindings)
		elog(ERROR, "graph edge index-only mode must be configured before execution");
	state->edge_index_only = enabled;
}

/* Wrap a not-yet-populated owned vector in its final SQL array header. */
static ArrayType *
fixed_array_wrap_int8(int64 **values, uint32 count)
{
	Size overhead = ARR_OVERHEAD_NONULLS(1);
	Size bytes = add_size(overhead, mul_size(count, sizeof(int64)));
	ArrayType *array = repalloc(*values, bytes);

	memset(array, 0, overhead);
	SET_VARSIZE(array, bytes);
	ARR_NDIM(array) = 1;
	ARR_ELEMTYPE(array) = INT8OID;
	ARR_DIMS(array)[0] = count;
	ARR_LBOUND(array)[0] = 1;
	*values = (int64 *) ARR_DATA_PTR(array);
	return array;
}

bool
GgcFixedEnableArrayOutput(GgcFixedState *state, TupleTableSlot *slot,
						  ExprContext *econtext)
{
	GgcFixedArrayOutput *output;
	static const Oid types[] = {INT8OID, INT8ARRAYOID, INT8ARRAYOID, TEXTARRAYOID};

	if (!state || state->failed || state->finished ||
		state->instrumentation.anchor_batches || state->array_output)
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						errmsg("graph array output must be selected before execution")));
	if (!state->factor_arena || state->endpoint_set || !slot || !econtext ||
		slot->tts_ops != &TTSOpsVirtual || !slot->tts_tupleDescriptor ||
		slot->tts_tupleDescriptor->natts != lengthof(types) ||
		state->stage_count >= PG_INT32_MAX)
		return false;
	for (int i = 0; i < lengthof(types); i++)
	{
		Form_pg_attribute attr = TupleDescAttr(slot->tts_tupleDescriptor, i);

		if (attr->attisdropped || attr->atttypid != types[i])
			return false;
	}
	for (uint32 i = 0; i < state->stage_count; i++)
		if (state->stages[i].plan.payload_attnum != InvalidAttrNumber &&
			state->stages[i].payload_type != TEXTOID)
			return false;
	output = MemoryContextAllocZero(state->context, sizeof(*output));
	output->slot = slot;
	output->econtext = econtext;
	output->payload_array.array_context = state->context;
	output->payload_array.array_payloads = MemoryContextAlloc(state->context,
		mul_size(state->stage_count, sizeof(Datum)));
	output->vertices = fixed_array_wrap_int8(&state->output_vertices, state->stage_count + 1);
	output->edges = fixed_array_wrap_int8(&state->output_edges, state->stage_count);
	state->array_output = output;
	return true;
}

void
GgcFixedGetArrayOutputStats(const GgcFixedState *state, uint64 *updates,
						   uint64 *rebuilds)
{
	*updates = state && state->array_output ? state->array_output->array_layout_updates : 0;
	*rebuilds = state && state->array_output ? state->array_output->array_layout_rebuilds : 0;
}

Size
GgcFixedFactorizedPeakBytes(const GgcFixedState *state)
{
	return state ? state->factor_peak : 0;
}

uint64
GgcFixedFactorizedArcs(const GgcFixedState *state)
{
	return state ? state->factor_arcs : 0;
}
