/*-------------------------------------------------------------------------
 *
 * graphadj.c
 *    Bootstrap implementation of the graph_adj index access method.
 *
 * The GS-010 through GS-045 prototype implements the V2 page shell, canonical
 * stable Slots, split OUT/IN directories, WAL-logged twin appends, scoped
 * stable-reference traversal, Base/Delta publication, and packed small
 * segments.  GS-050 adds callback-authoritative VACUUM compaction and
 * whole-page reclamation through a WAL-logged free list.  Slot reuse remains
 * deliberately disabled until a later task can prove its ABA horizon.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/generic_xlog.h"
#include "access/graphadj.h"
#include "access/relation.h"
#include "access/reloptions.h"
#include "access/table.h"
#include "access/tableam.h"
#include "access/xlog.h"
#include "access/xloginsert.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_class.h"
#include "catalog/pg_type_d.h"
#include "commands/vacuum.h"
#include "commands/defrem.h"
#include "executor/tuptable.h"
#include "executor/graph_adj_provider.h"
#include "executor/graph_provider.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "storage/bufmgr.h"
#include "storage/bufpage.h"
#include "storage/lock.h"
#include "storage/lmgr.h"
#include "storage/predicate.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/injection_point.h"
#include "utils/rel.h"
#include "utils/selfuncs.h"
#include "utils/snapmgr.h"

PG_FUNCTION_INFO_V1(graphadjhandler);
PG_FUNCTION_INFO_V1(graph_adj_index_info);
PG_FUNCTION_INFO_V1(graph_adj_format_supported);
PG_FUNCTION_INFO_V1(graph_adj_debug_ref_probe);
PG_FUNCTION_INFO_V1(graph_adj_debug_walk);
PG_FUNCTION_INFO_V1(graph_adj_debug_compact);
PG_FUNCTION_INFO_V1(graph_adj_debug_fault);

/* Backend-local, superuser-only crash arm used by the GS recovery tests. */
static Oid graphadj_fault_index_oid = InvalidOid;
static int64 graphadj_fault_key = 0;
static char graphadj_fault_name[NAMEDATALEN];
static bool graphadj_fault_error = false;

static void
graphadj_maybe_crash(Relation index, int64 key, const char *point)
{
	if (graphadj_fault_index_oid != RelationGetRelid(index) ||
		graphadj_fault_key != key ||
		strcmp(graphadj_fault_name, point) != 0)
		return;

	/* Do not let an error handler accidentally fire the same arm twice. */
	graphadj_fault_index_oid = InvalidOid;
	graphadj_fault_name[0] = '\0';
	if (graphadj_fault_error)
	{
		graphadj_fault_error = false;
		ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
						errmsg("graph_adj injected error after %s", point)));
	}
	if (RelationNeedsWAL(index))
		XLogFlush(GetXLogInsertRecPtr());
	ereport(PANIC,
			(errmsg("graph_adj injected crash after %s", point),
			 errdetail("Index %u, endpoint key %lld.",
					   RelationGetRelid(index), (long long) key)));
}

typedef struct GraphAdjMetaData
{
	uint32		magic;
	uint32		format_version;
	uint64		build_generation;
	BlockNumber out_root;
	BlockNumber in_root;
	BlockNumber slot_extent_head;
	BlockNumber retirement_head;
	BlockNumber free_head;
	BlockNumber slot_tail;
	GraphAdjVertexRefData pending_slot;
} GraphAdjMetaData;

typedef struct GraphAdjBuildState
{
	Relation	index;
	Oid			heap_oid;
	double		heap_tuples;
} GraphAdjBuildState;

typedef struct GraphAdjDataRefData
{
	BlockNumber block;
	uint32		segment_offset;
	uint64		segment_generation;
} GraphAdjDataRefData;

/* Advisory allocation hints only: validate every referenced page under its
 * content lock. Relcache invalidation discards them; rollback/reclamation or
 * a concurrent allocator can make them stale without affecting correctness. */
typedef struct GraphAdjAllocHints
{
	BlockNumber page[2][2];	/* Base/Delta, OUT/IN */
} GraphAdjAllocHints;

typedef struct GraphAdjStorageStats
{
	uint32		slot_count;
	uint32		out_directory_items;
	uint32		in_directory_items;
	uint32		out_directory_pages;
	uint32		in_directory_pages;
	uint32		out_directory_leaf_pages;
	uint32		in_directory_leaf_pages;
	GraphAdjPageKind out_root_kind;
	GraphAdjPageKind in_root_kind;
	uint64		out_physical_entries;
	uint64		in_physical_entries;
	uint64		out_visible_entries;
	uint64		in_visible_entries;
	uint32		data_pages;
	uint32		shared_data_pages;
	uint32		allocated_segments;
	uint32		referenced_segments;
	uint32		unreferenced_segments;
	uint32		free_pages;
	uint32		reused_pages;
	uint64		data_usable_bytes;
	uint64		allocated_segment_reserved_bytes;
	uint64		segment_reserved_bytes;
	uint64		retired_segment_bytes;
	uint64		entry_payload_bytes;
	uint32		base_pages;
	uint32		sealed_pages;
	uint32		active_pages;
	uint64		base_entries;
	uint64		sealed_entries;
	uint64		active_entries;
	uint64		inline_entries;
	uint32		inline_directions;
	uint32		empty_direction_heads;
	bool		canonical_mapping_valid;
	bool		opposite_refs_valid;
	bool		all_mutable_pages_have_lsn;
} GraphAdjStorageStats;

typedef struct GraphAdjScanOpaqueData
{
	ItemPointerData *items;
	uint32		count;
	uint32		capacity;
	uint32		next;
	bool		initialized;
} GraphAdjScanOpaqueData;

typedef GraphAdjScanOpaqueData *GraphAdjScanOpaque;

typedef struct GraphAdjCandidateData
{
	int64		opposite_key;
	GraphAdjVertexRefData opposite_ref;
	ItemPointerData root_tid;
} GraphAdjCandidateData;

typedef struct GraphAdjCandidateVector
{
	GraphAdjCandidateData *items;
	uint32		count;
	uint32		capacity;
} GraphAdjCandidateVector;

/* Query-local identity cache, not adjacency/payload data. Slot identities are
 * immutable in formats 3/4 (no Slot reuse); the retained index relation lock also
 * excludes reclamation, rebuild and diagnostic mutation. Descriptor contents
 * are NEVER cached here. Disable/rework this if online Slot reuse is added. */
#define GRAPHADJ_REF_CACHE_SIZE 64U
typedef struct GraphAdjRefCacheEntry
{
	GraphAdjVertexRefData ref;
	int64 key;
} GraphAdjRefCacheEntry;

typedef struct GraphAdjRefCache
{
	GraphAdjRefCacheEntry entries[GRAPHADJ_REF_CACHE_SIZE];
} GraphAdjRefCache;

typedef struct GraphAdjWalkBinding
{
	int64		key;
	GraphAdjVertexRefData ref;
	bool		has_ref;
} GraphAdjWalkBinding;

typedef struct GraphAdjWalkVector
{
	GraphAdjWalkBinding *items;
	uint32		count;
	uint32		capacity;
} GraphAdjWalkVector;

typedef struct GraphAdjDirectoryTuple
{
	int64		key;
	GraphAdjVertexRefData ref;
} GraphAdjDirectoryTuple;

typedef struct GraphAdjDirectoryChild
{
	int64		high_key;
	BlockNumber block;
} GraphAdjDirectoryChild;

typedef struct GraphAdjCompactionResult
{
	GraphAdjVertexRefData owner;
	GraphAdjDataRefData old_base;
	GraphAdjDataRefData sealed;
	GraphAdjDataRefData new_active;
	GraphAdjDataRefData new_base;
	uint64		view_generation_before;
	uint64		view_generation_after_seal;
	uint64		view_generation_after_publish;
	uint32		sealed_cutoff;
	uint32		base_input_items;
	uint32		base_output_items;
	uint32		removed_items;
	uint32		active_items_at_publish;
	bool		resumed_sealed;
	bool		vacuum_rewrite;
	bool		active_retired;
} GraphAdjCompactionResult;

static IndexBuildResult *graphadjbuild(Relation heap, Relation index,
									   IndexInfo *indexInfo);
static void graphadjbuildempty(Relation index);
static bool graphadjinsert(Relation index, Datum *values, bool *isnull,
						   ItemPointer heap_tid, Relation heap,
						   IndexUniqueCheck checkUnique, bool indexUnchanged,
						   IndexInfo *indexInfo);
static IndexBulkDeleteResult *graphadjbulkdelete(IndexVacuumInfo *info,
											 IndexBulkDeleteResult *stats,
											 IndexBulkDeleteCallback callback,
											 void *callback_state);
static IndexBulkDeleteResult *graphadjvacuumcleanup(IndexVacuumInfo *info,
											   IndexBulkDeleteResult *stats);
static void graphadjcostestimate(PlannerInfo *root, IndexPath *path,
								 double loop_count, Cost *indexStartupCost,
								 Cost *indexTotalCost,
								 Selectivity *indexSelectivity,
								 double *indexCorrelation, double *indexPages);
static bytea *graphadjoptions(Datum reloptions, bool validate);
static bool graphadjvalidate(Oid opclassoid);
static IndexScanDesc graphadjbeginscan(Relation index, int nkeys,
									   int norderbys);
static void graphadjrescan(IndexScanDesc scan, ScanKey keys, int nkeys,
						   ScanKey orderbys, int norderbys);
static bool graphadjgettuple(IndexScanDesc scan, ScanDirection direction);
static void graphadjendscan(IndexScanDesc scan);
static CompareType graphadjtranslatestrategy(StrategyNumber strategy,
											 Oid opfamily);
static StrategyNumber graphadjtranslatecmptype(CompareType cmptype,
											 Oid opfamily);
static void graphadj_read_opaque(Relation index, Page page,
								 GraphAdjPageKind expected_kind,
								 GraphAdjDirection expected_direction,
								 uint64 *allocation_generation);
static void graphadj_read_opaque_identity(Relation index, Page page,
										  GraphAdjPageKind *kind,
										  GraphAdjDirection *direction,
										  uint64 *allocation_generation);
static void graphadj_insert_edge(Relation index, Oid heap_oid,
								 Datum *values, bool *isnull,
								 ItemPointer heap_tid);
static void graphadj_prepare_scan(IndexScanDesc scan);
static bool graphadj_ref_matches_slot(Page slot_page, BlockNumber slot_block, int64 key,
								  const GraphAdjVertexRefData *ref);
static bool graphadj_directory_lookup_relation(Relation index,
											BlockNumber root_block,
											GraphAdjDirection direction,
											int64 key,
											GraphAdjVertexRefData *result);
static void graphadj_directory_insert(Relation index, BlockNumber root_block,
									  GraphAdjDirection direction, int64 key,
									  const GraphAdjVertexRefData *ref);
static bool graphadj_compact_endpoint(Relation index, int64 key,
									 GraphAdjDirection direction, int32 pause_ms,
									 IndexBulkDeleteCallback callback,
									 void *callback_state,
									 GraphAdjCompactionResult *result);
static BlockNumber graphadj_reclaim_orphan_pages(Relation index);
static void graphadj_fill_vacuum_stats(Relation index,
									   IndexVacuumInfo *info,
									   IndexBulkDeleteResult *stats);

static inline void
graphadj_put_u16(unsigned char *target, uint16 value)
{
	target[0] = (unsigned char) value;
	target[1] = (unsigned char) (value >> 8);
}

static inline void
graphadj_put_u32(unsigned char *target, uint32 value)
{
	target[0] = (unsigned char) value;
	target[1] = (unsigned char) (value >> 8);
	target[2] = (unsigned char) (value >> 16);
	target[3] = (unsigned char) (value >> 24);
}

static inline void
graphadj_put_u64(unsigned char *target, uint64 value)
{
	int			i;

	for (i = 0; i < 8; i++)
		target[i] = (unsigned char) (value >> (i * 8));
}

static inline uint16
graphadj_get_u16(const unsigned char *source)
{
	return ((uint16) source[0]) | ((uint16) source[1] << 8);
}

static inline uint32
graphadj_get_u32(const unsigned char *source)
{
	return ((uint32) source[0]) |
		((uint32) source[1] << 8) |
		((uint32) source[2] << 16) |
		((uint32) source[3] << 24);
}

static inline uint64
graphadj_get_u64(const unsigned char *source)
{
	uint64		result = 0;
	int			i;

	for (i = 0; i < 8; i++)
		result |= ((uint64) source[i]) << (i * 8);
	return result;
}

static inline Size
graphadj_body_offset(void)
{
	return MAXALIGN(SizeOfPageHeaderData);
}

static inline void
graphadj_set_lower(Page page, Size used_end)
{
	PageHeader	header = (PageHeader) page;

	if (used_end > header->pd_upper)
		elog(ERROR, "graph_adj page body exceeds the special area");
	header->pd_lower = (LocationIndex) used_end;
}

static inline uint16
graphadj_directory_capacity(Page page)
{
	PageHeader	header = (PageHeader) page;
	Size		start = graphadj_body_offset() + GRAPHADJ_DIRECTORY_PAGE_HEADER_SIZE;

	return (uint16) ((header->pd_upper - start) /
					  GRAPHADJ_DIRECTORY_LEAF_ITEM_SIZE);
}

static inline uint16
graphadj_directory_internal_capacity(Page page)
{
	PageHeader	header = (PageHeader) page;
	Size		start = graphadj_body_offset() + GRAPHADJ_DIRECTORY_PAGE_HEADER_SIZE;

	return (uint16) ((header->pd_upper - start) /
					  GRAPHADJ_DIRECTORY_INTERNAL_ITEM_SIZE);
}

static inline uint16
graphadj_slot_capacity(Page page)
{
	PageHeader	header = (PageHeader) page;
	Size		start = graphadj_body_offset() + GRAPHADJ_SLOT_PAGE_HEADER_SIZE;

	return (uint16) ((header->pd_upper - start) /
					  GRAPHADJ_ENDPOINT_SLOT_SIZE);
}

static inline unsigned char *
graphadj_directory_item(Page page, uint16 item_index)
{
	return (unsigned char *) page + graphadj_body_offset() +
		GRAPHADJ_DIRECTORY_PAGE_HEADER_SIZE +
		((Size) item_index * GRAPHADJ_DIRECTORY_LEAF_ITEM_SIZE);
}

static inline unsigned char *
graphadj_directory_internal_item(Page page, uint16 item_index)
{
	return (unsigned char *) page + graphadj_body_offset() +
		GRAPHADJ_DIRECTORY_PAGE_HEADER_SIZE +
		((Size) item_index * GRAPHADJ_DIRECTORY_INTERNAL_ITEM_SIZE);
}

static inline unsigned char *
graphadj_slot(Page page, uint32 ordinal)
{
	return (unsigned char *) page + graphadj_body_offset() +
		GRAPHADJ_SLOT_PAGE_HEADER_SIZE +
		((Size) (ordinal - GRAPHADJ_FIRST_SLOT_ORDINAL) *
		 GRAPHADJ_ENDPOINT_SLOT_SIZE);
}

static inline unsigned char *
graphadj_descriptor(unsigned char *slot, GraphAdjDirection direction)
{
	Assert(direction == GRAPHADJ_DIRECTION_OUT ||
		   direction == GRAPHADJ_DIRECTION_IN);
	return slot + (direction == GRAPHADJ_DIRECTION_OUT ?
		GRAPHADJ_SLOT_OUT_DESCRIPTOR_OFFSET :
		GRAPHADJ_SLOT_IN_DESCRIPTOR_OFFSET);
}

static void
graphadj_put_vertex_ref(unsigned char *target,
						const GraphAdjVertexRefData *ref)
{
	graphadj_put_u64(target + GRAPHADJ_REF_SLOT_ID_OFFSET, ref->slot_id);
	graphadj_put_u64(target + GRAPHADJ_REF_SLOT_GENERATION_OFFSET,
					 ref->slot_generation);
	graphadj_put_u32(target + GRAPHADJ_REF_DOMAIN_OFFSET,
					 ref->endpoint_domain);
	graphadj_put_u32(target + GRAPHADJ_REF_RESERVED_OFFSET, 0);
}

/* The containing Slot identifies owner/direction; these are not DataRefs. */
static unsigned char *
graphadj_inline_entries(unsigned char *slot, GraphAdjDirection direction)
{
	Assert(direction == GRAPHADJ_DIRECTION_OUT ||
		   direction == GRAPHADJ_DIRECTION_IN);
	return slot + (direction == GRAPHADJ_DIRECTION_OUT ?
		GRAPHADJ_SLOT_OUT_INLINE_OFFSET : GRAPHADJ_SLOT_IN_INLINE_OFFSET);
}

static uint32
graphadj_inline_count(const unsigned char *descriptor)
{
	uint32 count = graphadj_get_u32(descriptor + GRAPHADJ_DESC_INLINE_COUNT_OFFSET);

	if (count > GRAPHADJ_INLINE_ITEMS)
		ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
						errmsg("graph_adj inline publication boundary is invalid")));
	return count;
}

static GraphAdjVertexRefData
graphadj_get_vertex_ref(const unsigned char *source)
{
	GraphAdjVertexRefData ref;

	ref.slot_id = graphadj_get_u64(source + GRAPHADJ_REF_SLOT_ID_OFFSET);
	ref.slot_generation = graphadj_get_u64(
		source + GRAPHADJ_REF_SLOT_GENERATION_OFFSET);
	ref.endpoint_domain = graphadj_get_u32(
		source + GRAPHADJ_REF_DOMAIN_OFFSET);
	return ref;
}

static void
graphadj_put_data_ref(unsigned char *target, const GraphAdjDataRefData *ref)
{
	graphadj_put_u32(target + GRAPHADJ_DATA_REF_BLOCK_OFFSET, ref->block);
	graphadj_put_u32(target + GRAPHADJ_DATA_REF_SEGMENT_OFFSET,
					 ref->segment_offset);
	graphadj_put_u64(target + GRAPHADJ_DATA_REF_GENERATION_OFFSET,
					 ref->segment_generation);
}

static GraphAdjDataRefData
graphadj_get_data_ref(const unsigned char *source)
{
	GraphAdjDataRefData ref;

	ref.block = graphadj_get_u32(source + GRAPHADJ_DATA_REF_BLOCK_OFFSET);
	ref.segment_offset = graphadj_get_u32(
		source + GRAPHADJ_DATA_REF_SEGMENT_OFFSET);
	ref.segment_generation = graphadj_get_u64(
		source + GRAPHADJ_DATA_REF_GENERATION_OFFSET);
	return ref;
}

static void
graphadj_put_invalid_data_ref(unsigned char *target)
{
	GraphAdjDataRefData ref = {
		.block = InvalidBlockNumber,
		.segment_offset = 0,
		.segment_generation = 0
	};

	graphadj_put_data_ref(target, &ref);
}

static GraphAdjDataRefData
graphadj_invalid_data_ref(void)
{
	GraphAdjDataRefData ref = {
		.block = InvalidBlockNumber,
		.segment_offset = 0,
		.segment_generation = 0
	};

	return ref;
}

static bool
graphadj_data_ref_is_valid(const GraphAdjDataRefData *ref)
{
	return GraphAdjDataRefBoundsValid(ref->block, ref->segment_offset,
								  ref->segment_generation);
}

static bool
graphadj_data_ref_equal(const GraphAdjDataRefData *left,
						const GraphAdjDataRefData *right)
{
	return left->block == right->block &&
		left->segment_offset == right->segment_offset &&
		left->segment_generation == right->segment_generation;
}

static bool
graphadj_vertex_ref_equal(const GraphAdjVertexRefData *left,
						  const GraphAdjVertexRefData *right)
{
	return left->slot_id == right->slot_id &&
		left->slot_generation == right->slot_generation &&
		left->endpoint_domain == right->endpoint_domain;
}

static void
graphadj_write_opaque(Page page, GraphAdjPageKind kind,
					  GraphAdjDirection direction, uint64 allocation_generation)
{
	unsigned char *opaque = (unsigned char *) PageGetSpecialPointer(page);

	MemSet(opaque, 0, MAXALIGN(GRAPHADJ_OPAQUE_SIZE));
	graphadj_put_u16(opaque + GRAPHADJ_OPAQUE_KIND_OFFSET, (uint16) kind);
	graphadj_put_u16(opaque + GRAPHADJ_OPAQUE_DIRECTION_OFFSET,
					 (uint16) direction);
	graphadj_put_u32(opaque + GRAPHADJ_OPAQUE_FORMAT_OFFSET,
					 GRAPHADJ_FORMAT_VERSION);
	graphadj_put_u64(opaque + GRAPHADJ_OPAQUE_ALLOCATION_GENERATION_OFFSET,
					 allocation_generation);
	graphadj_put_u32(opaque + GRAPHADJ_OPAQUE_RIGHTLINK_OFFSET,
					 InvalidBlockNumber);
	graphadj_put_u32(opaque + GRAPHADJ_OPAQUE_FLAGS_OFFSET, 0);
}

static void
graphadj_set_rightlink(Page page, BlockNumber rightlink)
{
	unsigned char *opaque = (unsigned char *) PageGetSpecialPointer(page);

	graphadj_put_u32(opaque + GRAPHADJ_OPAQUE_RIGHTLINK_OFFSET, rightlink);
}

static BlockNumber
graphadj_get_rightlink(Page page)
{
	const unsigned char *opaque =
		(const unsigned char *) PageGetSpecialPointer(page);

	return graphadj_get_u32(opaque + GRAPHADJ_OPAQUE_RIGHTLINK_OFFSET);
}

/* Slot pages are append-only and never enter the data-page free list. Their
 * rightlinks therefore strictly increase, making malformed cycles detectable. */
static BlockNumber
graphadj_next_slot_page(Relation index, Buffer buffer)
{
	Page page = BufferGetPage(buffer);
	const unsigned char *body = (const unsigned char *) PageGetContents(page);
	BlockNumber next;

	graphadj_read_opaque(index, page, GRAPHADJ_PAGE_ENDPOINT_SLOT,
		GRAPHADJ_DIRECTION_NONE, NULL);
	if (graphadj_get_u16(body + 2) != graphadj_slot_capacity(page) ||
		graphadj_get_u16(body) > graphadj_slot_capacity(page))
		ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
						errmsg("graph_adj Slot page header is corrupt")));
	next = graphadj_get_rightlink(page);
	if (next != InvalidBlockNumber &&
		(next <= BufferGetBlockNumber(buffer) || next >= RelationGetNumberOfBlocks(index)))
		ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
						errmsg("graph_adj Slot extent rightlink is corrupt")));
	return next;
}

static void
graphadj_init_slot_page(Page page, BlockNumber block)
{
	unsigned char *body;

	PageInit(page, BLCKSZ, MAXALIGN(GRAPHADJ_OPAQUE_SIZE));
	graphadj_write_opaque(page, GRAPHADJ_PAGE_ENDPOINT_SLOT,
		GRAPHADJ_DIRECTION_NONE, GRAPHADJ_FIRST_ALLOCATION_GENERATION + block);
	body = (unsigned char *) PageGetContents(page);
	graphadj_put_u16(body, 0);
	graphadj_put_u16(body + 2, graphadj_slot_capacity(page));
	graphadj_put_u16(body + 4, GRAPHADJ_FIRST_SLOT_ORDINAL);
	graphadj_set_lower(page, graphadj_body_offset() + GRAPHADJ_SLOT_PAGE_HEADER_SIZE);
}

static void
graphadj_init_directory_page(Page page, GraphAdjPageKind kind,
							 GraphAdjDirection direction,
							 uint64 allocation_generation)
{
	unsigned char *body;
	uint16		capacity;

	Assert(kind == GRAPHADJ_PAGE_DIR_LEAF ||
		   kind == GRAPHADJ_PAGE_DIR_INTERNAL);
	PageInit(page, BLCKSZ, MAXALIGN(GRAPHADJ_OPAQUE_SIZE));
	graphadj_write_opaque(page, kind, direction, allocation_generation);
	body = (unsigned char *) PageGetContents(page);
	capacity = kind == GRAPHADJ_PAGE_DIR_LEAF ?
		graphadj_directory_capacity(page) :
		graphadj_directory_internal_capacity(page);
	graphadj_put_u16(body, 0);
	graphadj_put_u16(body + 2, capacity);
	graphadj_put_u16(body + 4,
					 kind == GRAPHADJ_PAGE_DIR_LEAF ? 0 : 1);
	graphadj_put_u16(body + 6, 0);
	graphadj_set_lower(page, graphadj_body_offset() +
					   GRAPHADJ_DIRECTORY_PAGE_HEADER_SIZE);
}

static void
graphadj_write_meta(Page page)
{
	unsigned char *meta = (unsigned char *) PageGetContents(page);

	MemSet(meta, 0, GRAPHADJ_META_SIZE);
	graphadj_put_u32(meta + GRAPHADJ_META_MAGIC_OFFSET, GRAPHADJ_MAGIC);
	graphadj_put_u32(meta + GRAPHADJ_META_FORMAT_OFFSET,
					 GRAPHADJ_FORMAT_VERSION);
	graphadj_put_u64(meta + GRAPHADJ_META_BUILD_GENERATION_OFFSET,
					 GRAPHADJ_BUILD_GENERATION);
	graphadj_put_u32(meta + GRAPHADJ_META_OUT_ROOT_OFFSET,
					 GRAPHADJ_OUT_ROOT_BLKNO);
	graphadj_put_u32(meta + GRAPHADJ_META_IN_ROOT_OFFSET,
					 GRAPHADJ_IN_ROOT_BLKNO);
	graphadj_put_u32(meta + GRAPHADJ_META_SLOT_EXTENT_OFFSET,
					 GRAPHADJ_SLOT_EXTENT_BLKNO);
	graphadj_put_u32(meta + GRAPHADJ_META_RETIREMENT_HEAD_OFFSET,
					 InvalidBlockNumber);
	graphadj_put_u32(meta + GRAPHADJ_META_FREE_HEAD_OFFSET,
					 InvalidBlockNumber);
	graphadj_put_u32(meta + GRAPHADJ_META_FLAGS_OFFSET, 0);
	graphadj_put_u32(meta + GRAPHADJ_META_SLOT_TAIL_OFFSET,
					 GRAPHADJ_SLOT_EXTENT_BLKNO);
}

static void
graphadj_init_one_page(Relation index, ForkNumber forknum,
					   BlockNumber expected_block, GraphAdjPageKind kind,
					   GraphAdjDirection direction)
{
	Buffer		buffer;
	BlockNumber actual_block;
	Page		page;

	/* P_NEW skips the extension lock in PG19; use the locked extension API. */
	buffer = ExtendBufferedRel(BMR_REL(index), forknum, NULL, EB_LOCK_FIRST);
	actual_block = BufferGetBlockNumber(buffer);
	if (actual_block != expected_block)
		elog(ERROR, "graph_adj page allocation expected block %u, got %u",
			 expected_block, actual_block);
	START_CRIT_SECTION();
	page = BufferGetPage(buffer);
	PageInit(page, BLCKSZ, MAXALIGN(GRAPHADJ_OPAQUE_SIZE));
	graphadj_write_opaque(page, kind, direction,
					  GRAPHADJ_FIRST_ALLOCATION_GENERATION + actual_block);
	if (kind == GRAPHADJ_PAGE_META)
	{
		graphadj_write_meta(page);
		graphadj_set_lower(page, graphadj_body_offset() + GRAPHADJ_META_SIZE);
	}
	else if (kind == GRAPHADJ_PAGE_DIR_LEAF ||
			 kind == GRAPHADJ_PAGE_DIR_INTERNAL)
	{
		graphadj_init_directory_page(page, kind, direction,
								 GRAPHADJ_FIRST_ALLOCATION_GENERATION +
								 actual_block);
	}
	else if (kind == GRAPHADJ_PAGE_ENDPOINT_SLOT)
	{
		unsigned char *body = (unsigned char *) PageGetContents(page);

		/* slot_count, capacity, free_hint, reserved */
		graphadj_put_u16(body, 0);
		graphadj_put_u16(body + 2, graphadj_slot_capacity(page));
		graphadj_put_u16(body + 4, GRAPHADJ_FIRST_SLOT_ORDINAL);
		graphadj_put_u16(body + 6, 0);
		graphadj_set_lower(page, graphadj_body_offset() +
					   GRAPHADJ_SLOT_PAGE_HEADER_SIZE);
	}
	MarkBufferDirty(buffer);
	if (forknum == INIT_FORKNUM || RelationNeedsWAL(index))
		/* Fixed-page bodies live outside line pointers; WAL must include them. */
		log_newpage_buffer(buffer, false);
	END_CRIT_SECTION();

	UnlockReleaseBuffer(buffer);
}

static void
graphadj_init_index(Relation index, ForkNumber forknum)
{
	if (RelationGetNumberOfBlocksInFork(index, forknum) != 0)
		elog(ERROR, "graph_adj index \"%s\" already contains data",
			 RelationGetRelationName(index));

	graphadj_init_one_page(index, forknum, GRAPHADJ_META_BLKNO,
					   GRAPHADJ_PAGE_META, GRAPHADJ_DIRECTION_NONE);
	graphadj_init_one_page(index, forknum, GRAPHADJ_OUT_ROOT_BLKNO,
					   GRAPHADJ_PAGE_DIR_LEAF, GRAPHADJ_DIRECTION_OUT);
	graphadj_init_one_page(index, forknum, GRAPHADJ_IN_ROOT_BLKNO,
					   GRAPHADJ_PAGE_DIR_LEAF, GRAPHADJ_DIRECTION_IN);
	graphadj_init_one_page(index, forknum, GRAPHADJ_SLOT_EXTENT_BLKNO,
					   GRAPHADJ_PAGE_ENDPOINT_SLOT, GRAPHADJ_DIRECTION_NONE);
}

static void
graphadj_validate_definition(Relation heap, Relation index, IndexInfo *indexInfo)
{
	int			i;

	if (heap->rd_rel->relkind != RELKIND_RELATION ||
		heap->rd_rel->relpersistence != RELPERSISTENCE_PERMANENT ||
		heap->rd_rel->relam != HEAP_TABLE_AM_OID)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("graph_adj supports only ordinary logged heap tables")));
	if (index->rd_index->indnkeyatts != 2 || index->rd_index->indnatts != 2)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("graph_adj requires exactly two key columns"),
				 errdetail("The prototype key order is (source bigint, destination bigint).")));
	if (indexInfo->ii_Unique || indexInfo->ii_ExclusionOps != NULL ||
		indexInfo->ii_Predicate != NIL || indexInfo->ii_Expressions != NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("graph_adj does not support unique, exclusion, partial, or expression indexes")));

	for (i = 0; i < 2; i++)
	{
		AttrNumber	attno = index->rd_index->indkey.values[i];
		Form_pg_attribute attr;

		if (attno <= 0)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("graph_adj does not support expression or system columns")));
		attr = TupleDescAttr(RelationGetDescr(heap), attno - 1);
		if (attr->atttypid != INT8OID || !attr->attnotnull)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("graph_adj endpoint columns must be bigint NOT NULL"),
					 errdetail("Column %d has type %s and attnotnull=%s.",
							   i + 1, format_type_be(attr->atttypid),
							   attr->attnotnull ? "true" : "false")));
	}
}

static bool
graphadj_directory_lookup(Relation index, Page page, int64 key,
						  GraphAdjVertexRefData *result)
{
	const unsigned char *body = (const unsigned char *) PageGetContents(page);
	uint16		count = graphadj_get_u16(body);
	uint16		capacity = graphadj_get_u16(body + 2);
	uint16		i;
	int64		previous = PG_INT64_MIN;

	if (capacity != graphadj_directory_capacity(page) || count > capacity)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("graph_adj directory header is corrupt")));
	for (i = 0; i < count; i++)
	{
		const unsigned char *item = graphadj_directory_item(page, i);
		int64		item_key;

		if (graphadj_get_u32(item + GRAPHADJ_DIR_ITEM_SIZE_OFFSET) !=
			GRAPHADJ_DIRECTORY_LEAF_ITEM_SIZE ||
			graphadj_get_u16(item + GRAPHADJ_DIR_ITEM_KIND_OFFSET) !=
			GRAPHADJ_DIR_ITEM_KIND_ENDPOINT)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("graph_adj directory item is corrupt")));
		item_key = (int64) graphadj_get_u64(
			item + GRAPHADJ_DIR_ITEM_KEY_OFFSET);
		if (i > 0 && item_key <= previous)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("graph_adj directory leaf keys are not strictly ordered")));
		previous = item_key;
		if (item_key == key)
		{
			*result = graphadj_get_vertex_ref(
				item + GRAPHADJ_DIR_ITEM_REF_OFFSET);
			return true;
		}
		if (item_key > key)
			return false;
	}
	return false;
}

static void
graphadj_directory_append(Page page, int64 key,
						  const GraphAdjVertexRefData *ref)
{
	unsigned char *body = (unsigned char *) PageGetContents(page);
	uint16		count = graphadj_get_u16(body);
	uint16		capacity = graphadj_get_u16(body + 2);
	unsigned char *item;
	uint16		position = 0;
	GraphAdjVertexRefData existing;

	if (count >= capacity)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("graph_adj directory leaf has no physical space")));
	if (graphadj_directory_lookup(NULL, page, key, &existing))
	{
		if (!graphadj_vertex_ref_equal(&existing, ref))
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("graph_adj directory key maps to conflicting stable references")));
		return;
	}
	while (position < count)
	{
		item = graphadj_directory_item(page, position);
		if ((int64) graphadj_get_u64(item + GRAPHADJ_DIR_ITEM_KEY_OFFSET) > key)
			break;
		position++;
	}
	item = graphadj_directory_item(page, position);
	if (position < count)
		memmove(item + GRAPHADJ_DIRECTORY_LEAF_ITEM_SIZE, item,
				((Size) (count - position) *
				 GRAPHADJ_DIRECTORY_LEAF_ITEM_SIZE));
	MemSet(item, 0, GRAPHADJ_DIRECTORY_LEAF_ITEM_SIZE);
	graphadj_put_u32(item + GRAPHADJ_DIR_ITEM_SIZE_OFFSET,
					 GRAPHADJ_DIRECTORY_LEAF_ITEM_SIZE);
	graphadj_put_u16(item + GRAPHADJ_DIR_ITEM_KIND_OFFSET,
					 GRAPHADJ_DIR_ITEM_KIND_ENDPOINT);
	graphadj_put_u64(item + GRAPHADJ_DIR_ITEM_KEY_OFFSET, (uint64) key);
	graphadj_put_vertex_ref(item + GRAPHADJ_DIR_ITEM_REF_OFFSET, ref);
	graphadj_put_u16(body, count + 1);
	graphadj_set_lower(page, graphadj_body_offset() +
					   GRAPHADJ_DIRECTORY_PAGE_HEADER_SIZE +
					   ((Size) (count + 1) *
						GRAPHADJ_DIRECTORY_LEAF_ITEM_SIZE));
}


static GraphAdjDirectoryChild
graphadj_directory_get_child(Relation index, Page page, uint16 item_index)
{
	const unsigned char *body = (const unsigned char *) PageGetContents(page);
	uint16		count = graphadj_get_u16(body);
	uint16		capacity = graphadj_get_u16(body + 2);
	const unsigned char *item;
	GraphAdjDirectoryChild child;

	if (capacity != graphadj_directory_internal_capacity(page) ||
		count == 0 || count > capacity || item_index >= count)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("graph_adj directory internal header is corrupt")));
	item = graphadj_directory_internal_item(page, item_index);
	child.high_key = (int64) graphadj_get_u64(
		item + GRAPHADJ_DIR_INTERNAL_HIGH_KEY_OFFSET);
	child.block = graphadj_get_u32(
		item + GRAPHADJ_DIR_INTERNAL_CHILD_OFFSET);
	if (child.block == InvalidBlockNumber || child.block == GRAPHADJ_META_BLKNO)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("graph_adj directory internal item has invalid child")));
	return child;
}

static uint16
graphadj_directory_choose_child(Relation index, Page page, int64 key,
								GraphAdjDirectoryChild *result)
{
	const unsigned char *body = (const unsigned char *) PageGetContents(page);
	uint16		count = graphadj_get_u16(body);
	uint16		i;
	int64		previous = PG_INT64_MIN;

	if (count == 0 || count > graphadj_directory_internal_capacity(page))
		ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
						errmsg("graph_adj directory internal count is corrupt")));
	for (i = 0; i < count; i++)
	{
		GraphAdjDirectoryChild child = graphadj_directory_get_child(index,
			page, i);

		if (i > 0 && child.high_key <= previous)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("graph_adj directory internal high keys are not ordered")));
		previous = child.high_key;
		if (key <= child.high_key || i == count - 1)
		{
			*result = child;
			return i;
		}
	}
	pg_unreachable();
}

static void
graphadj_directory_put_child(Page page, uint16 item_index,
							 int64 high_key, BlockNumber block)
{
	unsigned char *item = graphadj_directory_internal_item(page, item_index);

	MemSet(item, 0, GRAPHADJ_DIRECTORY_INTERNAL_ITEM_SIZE);
	graphadj_put_u64(item + GRAPHADJ_DIR_INTERNAL_HIGH_KEY_OFFSET,
					 (uint64) high_key);
	graphadj_put_u32(item + GRAPHADJ_DIR_INTERNAL_CHILD_OFFSET, block);
	graphadj_put_u32(item + GRAPHADJ_DIR_INTERNAL_RESERVED_OFFSET, 0);
}

static void
graphadj_directory_insert_child(Page page, uint16 position,
								int64 high_key, BlockNumber block)
{
	unsigned char *body = (unsigned char *) PageGetContents(page);
	uint16		count = graphadj_get_u16(body);
	uint16		capacity = graphadj_get_u16(body + 2);
	unsigned char *item;

	if (count >= capacity || position > count)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("graph_adj directory internal page has no physical space")));
	item = graphadj_directory_internal_item(page, position);
	if (position < count)
		memmove(item + GRAPHADJ_DIRECTORY_INTERNAL_ITEM_SIZE, item,
				((Size) (count - position) *
				 GRAPHADJ_DIRECTORY_INTERNAL_ITEM_SIZE));
	graphadj_directory_put_child(page, position, high_key, block);
	graphadj_put_u16(body, count + 1);
	graphadj_set_lower(page, graphadj_body_offset() +
					   GRAPHADJ_DIRECTORY_PAGE_HEADER_SIZE +
					   ((Size) (count + 1) *
						GRAPHADJ_DIRECTORY_INTERNAL_ITEM_SIZE));
}

static bool
graphadj_directory_lookup_relation(Relation index, BlockNumber root_block,
								   GraphAdjDirection direction, int64 key,
								   GraphAdjVertexRefData *result)
{
	Buffer		root_buffer = ReadBuffer(index, root_block);
	Buffer		buffer = root_buffer;
	BlockNumber ancestors[32];
	unsigned	depth = 0;
	bool		found;

	/*
	 * Keep the stable root locked throughout descent.  Directory writers take
	 * it exclusively, so no downlink can change while we follow it.  This is
	 * intentionally the existing coarse locking protocol, not a B-link tree.
	 */
	LockBuffer(root_buffer, BUFFER_LOCK_SHARE);
	for (;;)
	{
		Page		page = BufferGetPage(buffer);
		GraphAdjPageKind kind;
		GraphAdjDirection stored_direction;
		GraphAdjDirectoryChild child;
		Buffer		next;
		unsigned	i;

		CHECK_FOR_INTERRUPTS();
		graphadj_read_opaque_identity(index, page, &kind, &stored_direction, NULL);
		if (stored_direction != direction ||
			(kind != GRAPHADJ_PAGE_DIR_LEAF && kind != GRAPHADJ_PAGE_DIR_INTERNAL))
			ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
							errmsg("graph_adj directory page kind or direction mismatch")));
		if (kind == GRAPHADJ_PAGE_DIR_LEAF)
		{
			found = graphadj_directory_lookup(index, page, key, result);
			break;
		}
		if (depth >= lengthof(ancestors))
			ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
							errmsg("graph_adj directory depth is invalid")));
		ancestors[depth++] = BufferGetBlockNumber(buffer);
		(void) graphadj_directory_choose_child(index, page, key, &child);
		for (i = 0; i < depth; i++)
			if (child.block == ancestors[i])
				ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
								errmsg("graph_adj directory contains a cycle")));
		next = ReadBuffer(index, child.block);
		LockBuffer(next, BUFFER_LOCK_SHARE);
		if (buffer != root_buffer)
			UnlockReleaseBuffer(buffer);
		buffer = next;
	}
	if (buffer != root_buffer)
		UnlockReleaseBuffer(buffer);
	UnlockReleaseBuffer(root_buffer);
	return found;
}

/*
 * Split a full page BEFORE descending into it.  A non-root parent is known
 * not to be full.  Root growth keeps the root block stable, publishing two
 * new children; otherwise the old page remains the left child.
 *
 * Format 7 high keys are inclusive routing fences, NOT exact child maxima.
 * The right fence is inherited from the parent; the root's is INT64_MAX.
 * This avoids ancestor updates on insertion, and every split is one atomic
 * Generic WAL record with at most three registered buffers.  A crash between
 * splits or before the subsequent insertion leaves a complete searchable
 * tree.  The existing pending-Slot protocol completes interrupted creation.
 */
static void
graphadj_directory_split(Relation index, Buffer parent_buffer, uint16 position,
						 Buffer buffer, GraphAdjDirection direction)
{
	bool		is_root = !BufferIsValid(parent_buffer);
	PGAlignedBlock saved;
	Page		old_page = saved.data;
	Page		page = BufferGetPage(buffer);
	GraphAdjPageKind kind;
	GraphAdjDirection stored_direction;
	uint16		count;
	uint16		left_count;
	int64		left_high;
	int64		right_high = PG_INT64_MAX;
	BlockNumber old_right;
	Buffer		left_buffer;
	Buffer		right_buffer;
	BlockNumber left_block;
	BlockNumber right_block;
	Page		left_page;
	Page		right_page;
	Page		parent_page;
	GenericXLogState *state;
	Size		item_size;
	Size		header = graphadj_body_offset() + GRAPHADJ_DIRECTORY_PAGE_HEADER_SIZE;

	memcpy(old_page, page, BLCKSZ);
	graphadj_read_opaque_identity(index, old_page, &kind, &stored_direction, NULL);
	Assert(stored_direction == direction);
	Assert(kind == GRAPHADJ_PAGE_DIR_LEAF || kind == GRAPHADJ_PAGE_DIR_INTERNAL);
	count = graphadj_get_u16((unsigned char *) PageGetContents(old_page));
	Assert(count == graphadj_get_u16((unsigned char *) PageGetContents(old_page) + 2));
	left_count = count / 2;
	old_right = graphadj_get_u32((unsigned char *) PageGetSpecialPointer(old_page) +
								GRAPHADJ_OPAQUE_RIGHTLINK_OFFSET);
	if (kind == GRAPHADJ_PAGE_DIR_LEAF)
	{
		item_size = GRAPHADJ_DIRECTORY_LEAF_ITEM_SIZE;
		left_high = (int64) graphadj_get_u64(
			graphadj_directory_item(old_page, left_count - 1) + GRAPHADJ_DIR_ITEM_KEY_OFFSET);
	}
	else
	{
		item_size = GRAPHADJ_DIRECTORY_INTERNAL_ITEM_SIZE;
		left_high = graphadj_directory_get_child(index, old_page, left_count - 1).high_key;
	}
	if (!is_root)
		right_high = graphadj_directory_get_child(index,
			BufferGetPage(parent_buffer), position).high_key;
	if (left_high >= right_high)
		ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
						errmsg("graph_adj directory split fence is invalid")));
	left_buffer = is_root ?
		ExtendBufferedRel(BMR_REL(index), MAIN_FORKNUM, NULL, EB_LOCK_FIRST) : buffer;
	right_buffer = ExtendBufferedRel(BMR_REL(index), MAIN_FORKNUM, NULL, EB_LOCK_FIRST);
	left_block = BufferGetBlockNumber(left_buffer);
	right_block = BufferGetBlockNumber(right_buffer);

	state = GenericXLogStart(index);
	parent_page = GenericXLogRegisterBuffer(state,
		is_root ? buffer : parent_buffer, is_root ? GENERIC_XLOG_FULL_IMAGE : 0);
	left_page = GenericXLogRegisterBuffer(state, left_buffer, GENERIC_XLOG_FULL_IMAGE);
	right_page = GenericXLogRegisterBuffer(state, right_buffer, GENERIC_XLOG_FULL_IMAGE);
	graphadj_init_directory_page(left_page, kind, direction,
		GRAPHADJ_FIRST_ALLOCATION_GENERATION + left_block);
	graphadj_init_directory_page(right_page, kind, direction,
		GRAPHADJ_FIRST_ALLOCATION_GENERATION + right_block);
	memcpy((char *) left_page + header, (char *) old_page + header, left_count * item_size);
	memcpy((char *) right_page + header, (char *) old_page + header + left_count * item_size,
		   (count - left_count) * item_size);
	graphadj_put_u16((unsigned char *) PageGetContents(left_page), left_count);
	graphadj_put_u16((unsigned char *) PageGetContents(right_page), count - left_count);
	graphadj_set_lower(left_page, header + left_count * item_size);
	graphadj_set_lower(right_page, header + (count - left_count) * item_size);
	graphadj_set_rightlink(left_page, right_block);
	graphadj_set_rightlink(right_page, old_right);
	if (is_root)
	{
		graphadj_init_directory_page(parent_page, GRAPHADJ_PAGE_DIR_INTERNAL, direction,
			GRAPHADJ_FIRST_ALLOCATION_GENERATION + BufferGetBlockNumber(buffer));
		graphadj_directory_insert_child(parent_page, 0, left_high, left_block);
		graphadj_directory_insert_child(parent_page, 1, right_high, right_block);
	}
	else
	{
		graphadj_directory_put_child(parent_page, position, left_high, left_block);
		graphadj_directory_insert_child(parent_page, position + 1, right_high, right_block);
	}
	GenericXLogFinish(state);
	UnlockReleaseBuffer(right_buffer);
	if (is_root)
		UnlockReleaseBuffer(left_buffer);
	graphadj_maybe_crash(index, 0, is_root ?
		"directory_root_split" : "directory_child_split");
	/* No buffers/content locks escape to heap access or expression evaluation. */
}

static void
graphadj_directory_insert(Relation index, BlockNumber root_block,
						  GraphAdjDirection direction, int64 key,
						  const GraphAdjVertexRefData *ref)
{
	Buffer		root_buffer = ReadBuffer(index, root_block);
	Buffer		buffer = root_buffer;
	BlockNumber ancestors[32];
	unsigned	depth = 0;

	LockBuffer(root_buffer, BUFFER_LOCK_EXCLUSIVE);
	for (;;)
	{
		Page		page = BufferGetPage(buffer);
		GraphAdjPageKind kind;
		GraphAdjDirection stored_direction;
		GraphAdjVertexRefData existing;
		GraphAdjDirectoryChild child;
		uint16		position;
		uint16		count;
		uint16		capacity;
		Buffer		next;
		Page		next_page;
		unsigned	i;

		CHECK_FOR_INTERRUPTS();
		graphadj_read_opaque_identity(index, page, &kind, &stored_direction, NULL);
		if (stored_direction != direction ||
			(kind != GRAPHADJ_PAGE_DIR_LEAF && kind != GRAPHADJ_PAGE_DIR_INTERNAL))
			ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
							errmsg("graph_adj directory page kind or direction mismatch")));
		count = graphadj_get_u16((unsigned char *) PageGetContents(page));
		capacity = kind == GRAPHADJ_PAGE_DIR_LEAF ?
			graphadj_directory_capacity(page) : graphadj_directory_internal_capacity(page);
		if (graphadj_get_u16((unsigned char *) PageGetContents(page) + 2) != capacity ||
			count > capacity)
			ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
							errmsg("graph_adj directory header is corrupt")));
		if (buffer == root_buffer && count == capacity)
		{
			graphadj_directory_split(index, InvalidBuffer, 0, buffer, direction);
			continue;
		}
		Assert(count < capacity);
		if (kind == GRAPHADJ_PAGE_DIR_LEAF)
		{
			if (graphadj_directory_lookup(index, page, key, &existing))
			{
				if (!graphadj_vertex_ref_equal(&existing, ref))
					ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
									errmsg("graph_adj directory key maps to conflicting stable references")));
			}
			else
			{
				GenericXLogState *state = GenericXLogStart(index);

				page = GenericXLogRegisterBuffer(state, buffer, 0);
				graphadj_directory_append(page, key, ref);
				GenericXLogFinish(state);
			}
			break;
		}
		if (depth >= lengthof(ancestors))
			ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
							errmsg("graph_adj directory depth is invalid")));
		ancestors[depth++] = BufferGetBlockNumber(buffer);
		position = graphadj_directory_choose_child(index, page, key, &child);
		for (i = 0; i < depth; i++)
			if (child.block == ancestors[i])
				ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
								errmsg("graph_adj directory contains a cycle")));
		next = ReadBuffer(index, child.block);
		LockBuffer(next, BUFFER_LOCK_EXCLUSIVE);
		next_page = BufferGetPage(next);
		graphadj_read_opaque_identity(index, next_page, &kind, &stored_direction, NULL);
		if (stored_direction != direction ||
			(kind != GRAPHADJ_PAGE_DIR_LEAF && kind != GRAPHADJ_PAGE_DIR_INTERNAL))
			ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
							errmsg("graph_adj directory child identity is corrupt")));
		capacity = kind == GRAPHADJ_PAGE_DIR_LEAF ?
			graphadj_directory_capacity(next_page) : graphadj_directory_internal_capacity(next_page);
		count = graphadj_get_u16((unsigned char *) PageGetContents(next_page));
		if (graphadj_get_u16((unsigned char *) PageGetContents(next_page) + 2) != capacity ||
			count > capacity)
			ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
							errmsg("graph_adj directory child header is corrupt")));
		if (count == capacity)
		{
			graphadj_directory_split(index, buffer, position, next, direction);
			UnlockReleaseBuffer(next);
			page = BufferGetPage(buffer);
			(void) graphadj_directory_choose_child(index, page, key, &child);
			next = ReadBuffer(index, child.block);
			LockBuffer(next, BUFFER_LOCK_EXCLUSIVE);
		}
		if (buffer != root_buffer)
			UnlockReleaseBuffer(buffer);
		buffer = next;
	}
	if (buffer != root_buffer)
		UnlockReleaseBuffer(buffer);
	UnlockReleaseBuffer(root_buffer);
}

static void
graphadj_validate_slot_identity(Relation index, Buffer slot_buffer, int64 key,
								const GraphAdjVertexRefData *ref)
{
	Page slot_page = BufferGetPage(slot_buffer);
	const unsigned char *body =
		(const unsigned char *) PageGetContents(slot_page);
	uint16		count = graphadj_get_u16(body);
	uint32		ordinal = GraphAdjSlotIdGetOrdinal(ref->slot_id);
	const unsigned char *slot;

	if (GraphAdjSlotIdGetBlock(ref->slot_id) != BufferGetBlockNumber(slot_buffer) ||
		count > graphadj_slot_capacity(slot_page) ||
		ordinal < GRAPHADJ_FIRST_SLOT_ORDINAL || ordinal > count)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("graph_adj directory contains an invalid Slot reference")));
	slot = graphadj_slot(slot_page, ordinal);
	if (graphadj_get_u32(slot + GRAPHADJ_SLOT_STATE_OFFSET) !=
		GRAPHADJ_SLOT_STATE_LIVE ||
		graphadj_get_u64(slot + GRAPHADJ_SLOT_GENERATION_OFFSET) !=
		ref->slot_generation ||
		graphadj_get_u32(slot + GRAPHADJ_SLOT_DOMAIN_OFFSET) !=
		ref->endpoint_domain ||
		(int64) graphadj_get_u64(slot + GRAPHADJ_SLOT_KEY_OFFSET) != key)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("graph_adj Slot identity does not match its directory mapping"),
				 errdetail("Index \"%s\" key %lld has a stale or conflicting persistent reference.",
						   RelationGetRelationName(index), (long long) key)));
}

/* Caller serializes creators with the head Slot EX lock. Never hold Meta
 * while waiting for a Slot lock; existing writers use Slot -> Meta too. */
static void
graphadj_creator_state(Relation index, BlockNumber *tail,
					   GraphAdjVertexRefData *pending)
{
	Buffer buffer = ReadBuffer(index, GRAPHADJ_META_BLKNO);
	Page page;
	const unsigned char *meta;
	BlockNumber blocks;

	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buffer);
	graphadj_read_opaque(index, page, GRAPHADJ_PAGE_META,
		GRAPHADJ_DIRECTION_NONE, NULL);
	if (((PageHeader) page)->pd_lower < graphadj_body_offset() + GRAPHADJ_META_SIZE)
		ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
						errmsg("graph_adj creator Meta is truncated")));
	meta = (const unsigned char *) PageGetContents(page);
	*tail = graphadj_get_u32(meta + GRAPHADJ_META_SLOT_TAIL_OFFSET);
	*pending = graphadj_get_vertex_ref(meta + GRAPHADJ_META_PENDING_SLOT_OFFSET);
	UnlockReleaseBuffer(buffer);
	blocks = RelationGetNumberOfBlocks(index);
	if (*tail < GRAPHADJ_SLOT_EXTENT_BLKNO || *tail >= blocks ||
		(pending->slot_id == GRAPHADJ_INVALID_SLOT_ID ?
		 (pending->slot_generation != 0 || pending->endpoint_domain != 0) :
		 (!GraphAdjSlotIdIsValid(pending->slot_id) || pending->slot_generation == 0 ||
		  GraphAdjSlotIdGetBlock(pending->slot_id) < GRAPHADJ_SLOT_EXTENT_BLKNO ||
		  GraphAdjSlotIdGetBlock(pending->slot_id) >= blocks)))
		ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
						errmsg("graph_adj creator Meta contains an invalid tail or pending reference")));
}

static void
graphadj_clear_pending(Relation index, const GraphAdjVertexRefData *expected)
{
	Buffer buffer = ReadBuffer(index, GRAPHADJ_META_BLKNO);
	Page page;
	unsigned char *meta;
	GraphAdjVertexRefData pending;
	GenericXLogState *state;

	LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buffer);
	graphadj_read_opaque(index, page, GRAPHADJ_PAGE_META,
		GRAPHADJ_DIRECTION_NONE, NULL);
	meta = (unsigned char *) PageGetContents(page);
	pending = graphadj_get_vertex_ref(meta + GRAPHADJ_META_PENDING_SLOT_OFFSET);
	if (!graphadj_vertex_ref_equal(&pending, expected))
		ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
						errmsg("graph_adj pending creator identity changed")));
	state = GenericXLogStart(index);
	page = GenericXLogRegisterBuffer(state, buffer, 0);
	meta = (unsigned char *) PageGetContents(page);
	MemSet(meta + GRAPHADJ_META_PENDING_SLOT_OFFSET, 0, GRAPHADJ_VERTEX_REF_SIZE);
	GenericXLogFinish(state);
	UnlockReleaseBuffer(buffer);
}

static void
graphadj_recover_pending(Relation index, Buffer head_buffer, Oid endpoint_domain,
						 const GraphAdjVertexRefData *pending)
{
	BlockNumber block = GraphAdjSlotIdGetBlock(pending->slot_id);
	Buffer buffer = block == GRAPHADJ_SLOT_EXTENT_BLKNO ? head_buffer : ReadBuffer(index, block);
	Page page;
	uint32 ordinal = GraphAdjSlotIdGetOrdinal(pending->slot_id);
	int64 key;
	GraphAdjDirection direction;

	if (buffer != head_buffer)
		LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
	page = BufferGetPage(buffer);
	graphadj_read_opaque(index, page, GRAPHADJ_PAGE_ENDPOINT_SLOT,
		GRAPHADJ_DIRECTION_NONE, NULL);
	if (pending->endpoint_domain != endpoint_domain || ordinal < GRAPHADJ_FIRST_SLOT_ORDINAL ||
		ordinal > graphadj_slot_capacity(page) ||
		ordinal > graphadj_get_u16((unsigned char *) PageGetContents(page)))
		ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
						errmsg("graph_adj pending Slot identity is invalid")));
	key = (int64) graphadj_get_u64(graphadj_slot(page, ordinal) + GRAPHADJ_SLOT_KEY_OFFSET);
	graphadj_validate_slot_identity(index, buffer, key, pending);
	for (direction = GRAPHADJ_DIRECTION_OUT; direction <= GRAPHADJ_DIRECTION_IN; direction++)
	{
		GraphAdjVertexRefData found;
		BlockNumber root = direction == GRAPHADJ_DIRECTION_OUT ?
			GRAPHADJ_OUT_ROOT_BLKNO : GRAPHADJ_IN_ROOT_BLKNO;

		if (graphadj_directory_lookup_relation(index, root, direction, key, &found))
		{
			if (!graphadj_vertex_ref_equal(&found, pending))
				ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
								errmsg("graph_adj pending Slot conflicts with a published directory")));
		}
		else
			graphadj_directory_insert(index, root, direction, key, pending);
	}
	graphadj_clear_pending(index, pending);
	if (buffer != head_buffer)
		UnlockReleaseBuffer(buffer);
}

static GraphAdjVertexRefData
graphadj_resolve_or_create_slot(Relation index, Oid endpoint_domain, int64 key)
{
	Buffer head_buffer;
	Buffer		slot_buffer;
	Page		slot_page;
	GraphAdjVertexRefData out_ref;
	GraphAdjVertexRefData in_ref;
	GraphAdjVertexRefData result;
	bool		out_found;
	bool		in_found;
	bool slot_found = false;
	BlockNumber tail = InvalidBlockNumber;
	GraphAdjVertexRefData pending;

	/* The Slot page is the prototype canonical-key creator serialization point. */
	slot_buffer = ReadBuffer(index, GRAPHADJ_SLOT_EXTENT_BLKNO);
	head_buffer = slot_buffer;
	LockBuffer(slot_buffer, BUFFER_LOCK_EXCLUSIVE);
	slot_page = BufferGetPage(slot_buffer);
	graphadj_read_opaque(index, slot_page, GRAPHADJ_PAGE_ENDPOINT_SLOT,
						 GRAPHADJ_DIRECTION_NONE, NULL);

	out_found = graphadj_directory_lookup_relation(index,
		GRAPHADJ_OUT_ROOT_BLKNO, GRAPHADJ_DIRECTION_OUT, key, &out_ref);
	in_found = graphadj_directory_lookup_relation(index,
		GRAPHADJ_IN_ROOT_BLKNO, GRAPHADJ_DIRECTION_IN, key, &in_ref);
	if (!out_found || !in_found)
	{
		graphadj_creator_state(index, &tail, &pending);
		if (pending.slot_id != GRAPHADJ_INVALID_SLOT_ID)
		{
			/* A different key must complete the interrupted creator as well. */
			graphadj_recover_pending(index, head_buffer, endpoint_domain, &pending);
			out_found = graphadj_directory_lookup_relation(index,
				GRAPHADJ_OUT_ROOT_BLKNO, GRAPHADJ_DIRECTION_OUT, key, &out_ref);
			in_found = graphadj_directory_lookup_relation(index,
				GRAPHADJ_IN_ROOT_BLKNO, GRAPHADJ_DIRECTION_IN, key, &in_ref);
		}
		if (out_found != in_found)
			ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
							errmsg("graph_adj partial directory publication has no pending creator")));
	}
	if (out_found && in_found &&
		!graphadj_vertex_ref_equal(&out_ref, &in_ref))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("graph_adj OUT/IN directories disagree for endpoint %lld",
						(long long) key)));

	if (out_found || in_found)
	{
		result = out_found ? out_ref : in_ref;
		if (result.endpoint_domain != endpoint_domain)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("graph_adj endpoint domain mismatch")));
		if (GraphAdjSlotIdGetBlock(result.slot_id) != GRAPHADJ_SLOT_EXTENT_BLKNO)
		{
			slot_buffer = ReadBuffer(index, GraphAdjSlotIdGetBlock(result.slot_id));
			LockBuffer(slot_buffer, BUFFER_LOCK_EXCLUSIVE);
			slot_page = BufferGetPage(slot_buffer);
			(void) graphadj_next_slot_page(index, slot_buffer);
		}
		graphadj_validate_slot_identity(index, slot_buffer, key, &result);
		slot_found = true;
	}
	else
	{
		/* Persisted tail: no normal-path or recovery walk over existing Slots. */
		if (tail != GRAPHADJ_SLOT_EXTENT_BLKNO)
		{
			slot_buffer = ReadBuffer(index, tail);
			LockBuffer(slot_buffer, BUFFER_LOCK_EXCLUSIVE);
			slot_page = BufferGetPage(slot_buffer);
		}
		if (graphadj_next_slot_page(index, slot_buffer) != InvalidBlockNumber)
			ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
							errmsg("graph_adj creator tail is not terminal")));
	}
	if (!slot_found)
	{
		GenericXLogState *state;
		unsigned char *body;
		unsigned char *slot;
		unsigned char *descriptor;
		uint16		count;
		uint16		capacity;
		uint32		ordinal;
		Buffer meta_buffer;
		Page meta_page;
		unsigned char *meta;

		body = (unsigned char *) PageGetContents(slot_page);
		count = graphadj_get_u16(body);
		capacity = graphadj_get_u16(body + 2);
		if (capacity != graphadj_slot_capacity(slot_page) || count > capacity)
			elog(ERROR, "graph_adj Slot extent header is corrupt");
		if (count == capacity)
		{
			Buffer new_buffer = ExtendBufferedRel(BMR_REL(index), MAIN_FORKNUM,
												 NULL, EB_LOCK_FIRST);
			Page new_page;

			meta_buffer = ReadBuffer(index, GRAPHADJ_META_BLKNO);
			LockBuffer(meta_buffer, BUFFER_LOCK_EXCLUSIVE);
			state = GenericXLogStart(index);
			slot_page = GenericXLogRegisterBuffer(state, slot_buffer, 0);
			new_page = GenericXLogRegisterBuffer(state, new_buffer, GENERIC_XLOG_FULL_IMAGE);
			meta_page = GenericXLogRegisterBuffer(state, meta_buffer, 0);
			meta = (unsigned char *) PageGetContents(meta_page);
			graphadj_init_slot_page(new_page, BufferGetBlockNumber(new_buffer));
			graphadj_set_rightlink(slot_page, BufferGetBlockNumber(new_buffer));
			graphadj_put_u32(meta + GRAPHADJ_META_SLOT_TAIL_OFFSET, BufferGetBlockNumber(new_buffer));
			GenericXLogFinish(state);
			UnlockReleaseBuffer(meta_buffer);
			graphadj_maybe_crash(index, key, "slot_page_linked");
			if (slot_buffer != head_buffer)
				UnlockReleaseBuffer(slot_buffer);
			slot_buffer = new_buffer;
			slot_page = BufferGetPage(slot_buffer);
			count = 0;
		}

		ordinal = count + GRAPHADJ_FIRST_SLOT_ORDINAL;
		result.slot_id = GraphAdjMakeSlotId(BufferGetBlockNumber(slot_buffer),
										  ordinal);
		result.slot_generation = 1;
		result.endpoint_domain = endpoint_domain;

		meta_buffer = ReadBuffer(index, GRAPHADJ_META_BLKNO);
		LockBuffer(meta_buffer, BUFFER_LOCK_EXCLUSIVE);
		state = GenericXLogStart(index);
		slot_page = GenericXLogRegisterBuffer(state, slot_buffer, 0);
		meta_page = GenericXLogRegisterBuffer(state, meta_buffer, 0);
		meta = (unsigned char *) PageGetContents(meta_page);
		graphadj_put_vertex_ref(meta + GRAPHADJ_META_PENDING_SLOT_OFFSET, &result);
		slot = graphadj_slot(slot_page, ordinal);
		MemSet(slot, 0, GRAPHADJ_ENDPOINT_SLOT_SIZE);
		graphadj_put_u64(slot + GRAPHADJ_SLOT_GENERATION_OFFSET,
						 result.slot_generation);
		graphadj_put_u32(slot + GRAPHADJ_SLOT_DOMAIN_OFFSET,
						 result.endpoint_domain);
		graphadj_put_u32(slot + GRAPHADJ_SLOT_STATE_OFFSET,
						 GRAPHADJ_SLOT_STATE_LIVE);
		graphadj_put_u64(slot + GRAPHADJ_SLOT_KEY_OFFSET, (uint64) key);
		descriptor = graphadj_descriptor(slot, GRAPHADJ_DIRECTION_OUT);
		graphadj_put_invalid_data_ref(descriptor + GRAPHADJ_DESC_BASE_OFFSET);
		graphadj_put_invalid_data_ref(descriptor + GRAPHADJ_DESC_SEALED_OFFSET);
		graphadj_put_invalid_data_ref(descriptor + GRAPHADJ_DESC_ACTIVE_OFFSET);
		descriptor = graphadj_descriptor(slot, GRAPHADJ_DIRECTION_IN);
		graphadj_put_invalid_data_ref(descriptor + GRAPHADJ_DESC_BASE_OFFSET);
		graphadj_put_invalid_data_ref(descriptor + GRAPHADJ_DESC_SEALED_OFFSET);
		graphadj_put_invalid_data_ref(descriptor + GRAPHADJ_DESC_ACTIVE_OFFSET);
		body = (unsigned char *) PageGetContents(slot_page);
		graphadj_put_u16(body, count + 1);
		graphadj_put_u16(body + 4, ordinal + 1);
		graphadj_set_lower(slot_page, graphadj_body_offset() +
						   GRAPHADJ_SLOT_PAGE_HEADER_SIZE +
						   ((Size) (count + 1) *
							GRAPHADJ_ENDPOINT_SLOT_SIZE));
		GenericXLogFinish(state);
		UnlockReleaseBuffer(meta_buffer);
		graphadj_maybe_crash(index, key, "slot_initialized");
	}

	/*
	 * Publish each direction independently and idempotently.  Holding the Slot
	 * creator lock prevents competing refs; a crash between publications leaves
	 * a recoverable missing mapping, not two reachable identities.
	 */
	if (!out_found)
	{
		graphadj_directory_insert(index, GRAPHADJ_OUT_ROOT_BLKNO,
			GRAPHADJ_DIRECTION_OUT, key, &result);
		graphadj_maybe_crash(index, key, "out_directory_published");
	}
	if (!in_found)
		graphadj_directory_insert(index, GRAPHADJ_IN_ROOT_BLKNO,
			GRAPHADJ_DIRECTION_IN, key, &result);
	if (!slot_found)
	{
		graphadj_maybe_crash(index, key, "in_directory_published");
		graphadj_clear_pending(index, &result);
		graphadj_maybe_crash(index, key, "pending_cleared");
	}

	if (slot_buffer != head_buffer)
		UnlockReleaseBuffer(slot_buffer);
	UnlockReleaseBuffer(head_buffer);
	return result;
}

static void
graphadj_write_segment_header(Page page, uint32 segment_offset,
						  const GraphAdjVertexRefData *owner,
						  GraphAdjDirection direction,
						  uint64 generation, uint32 capacity,
						  bool sealed)
{
	unsigned char *segment = (unsigned char *) page + segment_offset;
	GraphAdjDataRefData invalid = {
		.block = InvalidBlockNumber,
		.segment_offset = 0,
		.segment_generation = 0
	};
	Size		reserved_end;

	MemSet(segment, 0, GRAPHADJ_SEGMENT_HEADER_SIZE +
		((Size) capacity * GRAPHADJ_ENTRY_SIZE));
	graphadj_put_vertex_ref(segment + GRAPHADJ_SEGMENT_OWNER_OFFSET, owner);
	graphadj_put_u64(segment + GRAPHADJ_SEGMENT_VIEW_GENERATION_OFFSET, 1);
	graphadj_put_data_ref(segment + GRAPHADJ_SEGMENT_NEXT_OFFSET, &invalid);
	graphadj_put_u32(segment + GRAPHADJ_SEGMENT_LANE_OFFSET, 0);
	graphadj_put_u32(segment + GRAPHADJ_SEGMENT_PUBLISHED_OFFSET, 0);
	graphadj_put_u32(segment + GRAPHADJ_SEGMENT_CAPACITY_OFFSET, capacity);
	graphadj_put_u16(segment + GRAPHADJ_SEGMENT_DIRECTION_OFFSET,
					 (uint16) direction);
	graphadj_put_u16(segment + GRAPHADJ_SEGMENT_SEALED_OFFSET,
					 sealed ? 1 : 0);
	reserved_end = segment_offset + GRAPHADJ_SEGMENT_HEADER_SIZE +
		((Size) capacity * GRAPHADJ_ENTRY_SIZE);
	graphadj_set_lower(page, Max((Size) ((PageHeader) page)->pd_lower,
								 reserved_end));
}

static uint32
graphadj_data_page_segment_count(Page page)
{
	const unsigned char *opaque =
		(const unsigned char *) PageGetSpecialPointer(page);

	return graphadj_get_u32(opaque + GRAPHADJ_OPAQUE_FLAGS_OFFSET);
}

static void
graphadj_set_data_page_segment_count(Page page, uint32 count)
{
	unsigned char *opaque = (unsigned char *) PageGetSpecialPointer(page);

	graphadj_put_u32(opaque + GRAPHADJ_OPAQUE_FLAGS_OFFSET, count);
}

static Buffer
graphadj_allocate_segment_buffer(Relation index, GraphAdjPageKind kind,
								 GraphAdjDirection direction,
								 uint32 capacity,
								 BlockNumber exclude_block,
								 BlockNumber owner_slot_block,
								 GraphAdjDataRefData *data_ref,
								 bool *new_page)
{
	BlockNumber blocks = RelationGetNumberOfBlocks(index);
	BlockNumber block;
	BlockNumber first = GRAPHADJ_SLOT_EXTENT_BLKNO + 1;
	BlockNumber start;
	BlockNumber *hint;
	GraphAdjAllocHints *hints;
	Size		bytes = GRAPHADJ_SEGMENT_HEADER_SIZE +
		((Size) capacity * GRAPHADJ_ENTRY_SIZE);

	Assert(kind == GRAPHADJ_PAGE_PACKED_BASE || kind == GRAPHADJ_PAGE_DELTA);
	Assert(direction == GRAPHADJ_DIRECTION_OUT || direction == GRAPHADJ_DIRECTION_IN);
	if (!index->rd_amcache)
		index->rd_amcache = MemoryContextAllocZero(index->rd_indexcxt, sizeof(GraphAdjAllocHints));
	hints = (GraphAdjAllocHints *) index->rd_amcache;
	hint = &hints->page[kind == GRAPHADJ_PAGE_DELTA][direction == GRAPHADJ_DIRECTION_IN];
	start = *hint >= first && *hint < blocks ? *hint : first;

	/* Start at the last successful page, wrap at EOF, and examine each page
	 * at most once. A hint never authorizes reuse or suppresses WAL/locking. */
	for (BlockNumber tried = 0; blocks > first && tried < blocks - first; tried++)
	{
		Buffer		buffer;
		Page		page;
		const unsigned char *opaque;
		Size		offset;
		uint64		candidate = (uint64) start + tried;

		block = candidate < blocks ? (BlockNumber) candidate :
			first + (BlockNumber) (candidate - blocks);
		if (block == exclude_block || block == owner_slot_block)
			continue;
		buffer = ReadBuffer(index, block);
		/* Never wait on another endpoint's Slot/data while owning a Slot. */
		if (!ConditionalLockBuffer(buffer))
		{
			ReleaseBuffer(buffer);
			continue;
		}
		page = BufferGetPage(buffer);
		if (PageIsNew(page) ||
			PageGetSpecialSize(page) != MAXALIGN(GRAPHADJ_OPAQUE_SIZE))
		{
			UnlockReleaseBuffer(buffer);
			continue;
		}
		opaque = (const unsigned char *) PageGetSpecialPointer(page);
		if (graphadj_get_u32(opaque + GRAPHADJ_OPAQUE_FORMAT_OFFSET) !=
			GRAPHADJ_FORMAT_VERSION ||
			graphadj_get_u16(opaque + GRAPHADJ_OPAQUE_KIND_OFFSET) !=
			(uint16) kind ||
			graphadj_get_u16(opaque + GRAPHADJ_OPAQUE_DIRECTION_OFFSET) !=
			(uint16) direction)
		{
			UnlockReleaseBuffer(buffer);
			continue;
		}
		offset = MAXALIGN((Size) ((PageHeader) page)->pd_lower);
		if (offset < graphadj_body_offset() ||
			offset + bytes > ((PageHeader) page)->pd_upper)
		{
			UnlockReleaseBuffer(buffer);
			continue;
		}
		data_ref->block = block;
		data_ref->segment_offset = (uint32) offset;
		data_ref->segment_generation = graphadj_get_u64(
			opaque + GRAPHADJ_OPAQUE_ALLOCATION_GENERATION_OFFSET);
		*new_page = false;
		*hint = block;
		return buffer;
	}

	/*
	 * Reuse only whole pages that VACUUM proved unreachable from every live
	 * Descriptor.  Popping Meta.free_head and changing the page identity are
	 * one WAL record.  A crash after this record but before the caller writes
	 * its segment merely leaves an unreferenced data page for a later VACUUM
	 * to rediscover.
	 */
	{
		Buffer		meta_buffer = ReadBuffer(index, GRAPHADJ_META_BLKNO);
		Page		meta_page;
		unsigned char *meta;
		BlockNumber free_block;

		LockBuffer(meta_buffer, BUFFER_LOCK_EXCLUSIVE);
		meta_page = BufferGetPage(meta_buffer);
		graphadj_read_opaque(index, meta_page, GRAPHADJ_PAGE_META,
			GRAPHADJ_DIRECTION_NONE, NULL);
		meta = (unsigned char *) PageGetContents(meta_page);
		free_block = graphadj_get_u32(meta + GRAPHADJ_META_FREE_HEAD_OFFSET);
		if (free_block != InvalidBlockNumber)
		{
			Buffer		free_buffer;
			Page		free_page;
			BlockNumber next_free;
			uint64		old_generation;
			uint64		new_generation;
			GenericXLogState *state;

			if (free_block <= GRAPHADJ_SLOT_EXTENT_BLKNO ||
				free_block >= RelationGetNumberOfBlocks(index) ||
				free_block == exclude_block)
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("graph_adj free-list head is invalid")));
			free_buffer = ReadBuffer(index, free_block);
			LockBuffer(free_buffer, BUFFER_LOCK_EXCLUSIVE);
			free_page = BufferGetPage(free_buffer);
			graphadj_read_opaque(index, free_page, GRAPHADJ_PAGE_FREE,
				GRAPHADJ_DIRECTION_NONE, &old_generation);
			next_free = graphadj_get_rightlink(free_page);
			if (old_generation == PG_UINT64_MAX)
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("graph_adj allocation generation is exhausted")));
			new_generation = old_generation + 1;

			state = GenericXLogStart(index);
			meta_page = GenericXLogRegisterBuffer(state, meta_buffer, 0);
			free_page = GenericXLogRegisterBuffer(state, free_buffer,
				GENERIC_XLOG_FULL_IMAGE);
			meta = (unsigned char *) PageGetContents(meta_page);
			graphadj_put_u32(meta + GRAPHADJ_META_FREE_HEAD_OFFSET, next_free);
			PageInit(free_page, BLCKSZ, MAXALIGN(GRAPHADJ_OPAQUE_SIZE));
			graphadj_write_opaque(free_page, kind, direction, new_generation);
			GenericXLogFinish(state);
			graphadj_maybe_crash(index, 0, "free_page_popped");

			UnlockReleaseBuffer(meta_buffer);
			data_ref->block = free_block;
			data_ref->segment_offset = graphadj_body_offset();
			data_ref->segment_generation = new_generation;
			/* The page shell is already initialized by the pop WAL record. */
			*new_page = false;
			*hint = free_block;
			return free_buffer;
		}
		UnlockReleaseBuffer(meta_buffer);
	}

	{
		/*
		 * OUT/IN compaction and other endpoints can extend concurrently.
		 * A Slot/directory lock does not serialize the relation's EOF.  The
		 * extension API reserves a distinct block and returns it locked,
		 * before any allocator can observe an uninitialized page.
		 */
		Buffer		buffer = ExtendBufferedRel(BMR_REL(index), MAIN_FORKNUM,
												 NULL, EB_LOCK_FIRST);
		BlockNumber new_block = BufferGetBlockNumber(buffer);

		data_ref->block = new_block;
		data_ref->segment_offset = graphadj_body_offset();
		data_ref->segment_generation =
			GRAPHADJ_FIRST_ALLOCATION_GENERATION + new_block;
		*new_page = true;
		*hint = new_block;
		return buffer;
	}
}

static void
graphadj_initialize_segment(Page page, bool new_page, GraphAdjPageKind kind,
							GraphAdjDirection direction,
							const GraphAdjDataRefData *data_ref,
							const GraphAdjVertexRefData *owner,
							uint32 capacity, bool sealed)
{
	uint32		segment_count = 0;

	if (new_page)
	{
		PageInit(page, BLCKSZ, MAXALIGN(GRAPHADJ_OPAQUE_SIZE));
		graphadj_write_opaque(page, kind, direction,
			data_ref->segment_generation);
	}
	else
		segment_count = graphadj_data_page_segment_count(page);
	graphadj_write_segment_header(page, data_ref->segment_offset, owner,
		direction, data_ref->segment_generation, capacity, sealed);
	graphadj_set_data_page_segment_count(page, segment_count + 1);
}

static void
graphadj_append_direction(Relation index, const GraphAdjVertexRefData *owner,
						  GraphAdjDirection direction, int64 opposite_key,
						  const GraphAdjVertexRefData *opposite,
						  ItemPointer heap_tid)
{
	Buffer		slot_buffer;
	Buffer		data_buffer;
	Page		slot_page;
	Page		data_page;
	unsigned char *slot;
	unsigned char *descriptor;
	GraphAdjDataRefData active;
	bool		new_page = false;
	bool		new_segment = false;
	uint32		ordinal = GraphAdjSlotIdGetOrdinal(owner->slot_id);
	GenericXLogState *state;
	unsigned char *segment;
	unsigned char *entry;
	uint32		published;
	uint32		capacity;
	uint64		view_generation;
	uint64		allocation_generation;
	BlockNumber tid_block;
	OffsetNumber tid_offset;
	int64		owner_key;

	slot_buffer = ReadBuffer(index, GraphAdjSlotIdGetBlock(owner->slot_id));
	LockBuffer(slot_buffer, BUFFER_LOCK_EXCLUSIVE);
	slot_page = BufferGetPage(slot_buffer);
	graphadj_read_opaque(index, slot_page, GRAPHADJ_PAGE_ENDPOINT_SLOT,
						 GRAPHADJ_DIRECTION_NONE, NULL);
	graphadj_validate_slot_identity(index, slot_buffer,
		(int64) graphadj_get_u64(graphadj_slot(slot_page, ordinal) +
								 GRAPHADJ_SLOT_KEY_OFFSET), owner);
	slot = graphadj_slot(slot_page, ordinal);
	owner_key = (int64) graphadj_get_u64(slot + GRAPHADJ_SLOT_KEY_OFFSET);
	descriptor = graphadj_descriptor(slot, direction);
	published = graphadj_inline_count(descriptor);
	if (published < GRAPHADJ_INLINE_ITEMS)
	{
		/* No external page or migration: publish this direction atomically. */
		state = GenericXLogStart(index);
		slot_page = GenericXLogRegisterBuffer(state, slot_buffer, 0);
		slot = graphadj_slot(slot_page, ordinal);
		descriptor = graphadj_descriptor(slot, direction);
		entry = graphadj_inline_entries(slot, direction) +
			(Size) published * GRAPHADJ_ENTRY_SIZE;
		MemSet(entry, 0, GRAPHADJ_ENTRY_SIZE);
		graphadj_put_u64(entry + GRAPHADJ_ENTRY_KEY_OFFSET, (uint64) opposite_key);
		graphadj_put_vertex_ref(entry + GRAPHADJ_ENTRY_REF_OFFSET, opposite);
		graphadj_put_u32(entry + GRAPHADJ_ENTRY_TID_BLOCK_OFFSET,
			ItemPointerGetBlockNumber(heap_tid));
		graphadj_put_u16(entry + GRAPHADJ_ENTRY_TID_OFFSET_OFFSET,
			ItemPointerGetOffsetNumber(heap_tid));
		graphadj_put_u32(descriptor + GRAPHADJ_DESC_INLINE_COUNT_OFFSET, published + 1);
		view_generation = graphadj_get_u64(
			descriptor + GRAPHADJ_DESC_VIEW_GENERATION_OFFSET) + 1;
		graphadj_put_u64(descriptor + GRAPHADJ_DESC_VIEW_GENERATION_OFFSET,
			view_generation);
		GenericXLogFinish(state);
		graphadj_maybe_crash(index, owner_key, "entry_published");
		UnlockReleaseBuffer(slot_buffer);
		return;
	}
	active = graphadj_get_data_ref(descriptor + GRAPHADJ_DESC_ACTIVE_OFFSET);

	if (!graphadj_data_ref_is_valid(&active))
	{
		data_buffer = graphadj_allocate_segment_buffer(index,
			GRAPHADJ_PAGE_DELTA, direction, GRAPHADJ_SMALL_SEGMENT_ITEMS,
			InvalidBlockNumber, GraphAdjSlotIdGetBlock(owner->slot_id), &active, &new_page);
		new_segment = true;
	}
	else
	{
		data_buffer = ReadBuffer(index, active.block);
		LockBuffer(data_buffer, BUFFER_LOCK_EXCLUSIVE);
		data_page = BufferGetPage(data_buffer);
		graphadj_read_opaque(index, data_page, GRAPHADJ_PAGE_DELTA,
							 direction, &allocation_generation);
		if (allocation_generation != active.segment_generation)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("graph_adj active DataRef generation mismatch")));
		segment = (unsigned char *) data_page + active.segment_offset;
		if (!graphadj_vertex_ref_equal(
				&((GraphAdjVertexRefData) {
					.slot_id = graphadj_get_u64(segment + GRAPHADJ_REF_SLOT_ID_OFFSET),
					.slot_generation = graphadj_get_u64(segment + GRAPHADJ_REF_SLOT_GENERATION_OFFSET),
					.endpoint_domain = graphadj_get_u32(segment + GRAPHADJ_REF_DOMAIN_OFFSET)
				}), owner) ||
			graphadj_get_u16(segment + GRAPHADJ_SEGMENT_DIRECTION_OFFSET) !=
			(uint16) direction ||
			graphadj_get_u16(segment + GRAPHADJ_SEGMENT_SEALED_OFFSET) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("graph_adj active segment owner or direction mismatch")));
		published = graphadj_get_u32(segment +
			GRAPHADJ_SEGMENT_PUBLISHED_OFFSET);
		capacity = graphadj_get_u32(segment + GRAPHADJ_SEGMENT_CAPACITY_OFFSET);
		if (published >= capacity)
		{
			GraphAdjCompactionResult compacted;

			UnlockReleaseBuffer(data_buffer);
			UnlockReleaseBuffer(slot_buffer);
			if (!graphadj_compact_endpoint(index, owner_key, direction, 0,
										NULL, NULL, &compacted))
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("graph_adj could not grow a full active segment")));
			graphadj_append_direction(index, owner, direction, opposite_key,
				opposite, heap_tid);
			return;
		}
	}

	state = GenericXLogStart(index);
	slot_page = GenericXLogRegisterBuffer(state, slot_buffer, 0);
	data_page = GenericXLogRegisterBuffer(state, data_buffer,
		new_page ? GENERIC_XLOG_FULL_IMAGE : 0);
	slot = graphadj_slot(slot_page, ordinal);
	descriptor = graphadj_descriptor(slot, direction);
	if (new_segment)
	{
		graphadj_initialize_segment(data_page, new_page, GRAPHADJ_PAGE_DELTA,
			direction, &active, owner, GRAPHADJ_SMALL_SEGMENT_ITEMS, false);
		graphadj_put_data_ref(descriptor + GRAPHADJ_DESC_ACTIVE_OFFSET,
						  &active);
		graphadj_put_u32(descriptor + GRAPHADJ_DESC_LANE_COUNT_OFFSET, 1);
	}
	segment = (unsigned char *) data_page + active.segment_offset;
	if (!graphadj_vertex_ref_equal(
			&((GraphAdjVertexRefData) {
				.slot_id = graphadj_get_u64(segment + GRAPHADJ_REF_SLOT_ID_OFFSET),
				.slot_generation = graphadj_get_u64(segment + GRAPHADJ_REF_SLOT_GENERATION_OFFSET),
				.endpoint_domain = graphadj_get_u32(segment + GRAPHADJ_REF_DOMAIN_OFFSET)
			}), owner) ||
		graphadj_get_u16(segment + GRAPHADJ_SEGMENT_DIRECTION_OFFSET) !=
		(uint16) direction ||
		graphadj_get_u16(segment + GRAPHADJ_SEGMENT_SEALED_OFFSET) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("graph_adj active segment owner or direction mismatch")));
	published = graphadj_get_u32(segment + GRAPHADJ_SEGMENT_PUBLISHED_OFFSET);
	capacity = graphadj_get_u32(segment + GRAPHADJ_SEGMENT_CAPACITY_OFFSET);
	if (published >= capacity)
		elog(ERROR, "graph_adj active segment became full while exclusively locked");

	entry = segment + GRAPHADJ_SEGMENT_HEADER_SIZE +
		((Size) published * GRAPHADJ_ENTRY_SIZE);
	MemSet(entry, 0, GRAPHADJ_ENTRY_SIZE);
	graphadj_put_u64(entry + GRAPHADJ_ENTRY_KEY_OFFSET, (uint64) opposite_key);
	graphadj_put_vertex_ref(entry + GRAPHADJ_ENTRY_REF_OFFSET, opposite);
	tid_block = ItemPointerGetBlockNumber(heap_tid);
	tid_offset = ItemPointerGetOffsetNumber(heap_tid);
	graphadj_put_u32(entry + GRAPHADJ_ENTRY_TID_BLOCK_OFFSET, tid_block);
	graphadj_put_u16(entry + GRAPHADJ_ENTRY_TID_OFFSET_OFFSET, tid_offset);
	graphadj_put_u16(entry + GRAPHADJ_ENTRY_FLAGS_OFFSET, 0);

	/* Complete entry first; this published boundary is the reader cutoff. */
	graphadj_put_u32(segment + GRAPHADJ_SEGMENT_PUBLISHED_OFFSET,
					 published + 1);
	view_generation = graphadj_get_u64(
		descriptor + GRAPHADJ_DESC_VIEW_GENERATION_OFFSET) + 1;
	graphadj_put_u64(descriptor + GRAPHADJ_DESC_VIEW_GENERATION_OFFSET,
					 view_generation);
	GenericXLogFinish(state);
	graphadj_maybe_crash(index, owner_key, "entry_published");
	if (new_segment && !new_page)
		graphadj_maybe_crash(index, owner_key, "shared_segment_published");

	UnlockReleaseBuffer(data_buffer);
	UnlockReleaseBuffer(slot_buffer);
}

static void
graphadj_insert_edge(Relation index, Oid heap_oid, Datum *values,
					 bool *isnull, ItemPointer heap_tid)
{
	int64		source;
	int64		destination;
	GraphAdjVertexRefData source_ref;
	GraphAdjVertexRefData destination_ref;

	if (isnull[0] || isnull[1])
		ereport(ERROR,
				(errcode(ERRCODE_NOT_NULL_VIOLATION),
				 errmsg("graph_adj endpoint values cannot be NULL")));
	source = DatumGetInt64(values[0]);
	destination = DatumGetInt64(values[1]);
	source_ref = graphadj_resolve_or_create_slot(index, heap_oid, source);
	destination_ref = graphadj_resolve_or_create_slot(index, heap_oid,
											 destination);

	/* Never report AM success until both recoverable physical copies exist. */
		graphadj_append_direction(index, &source_ref, GRAPHADJ_DIRECTION_OUT,
						  destination, &destination_ref, heap_tid);
	graphadj_maybe_crash(index, source, "out_entry_published");
	graphadj_append_direction(index, &destination_ref, GRAPHADJ_DIRECTION_IN,
						  source, &source_ref, heap_tid);
}

static void
graphadj_build_callback(Relation index, ItemPointer tid, Datum *values,
						bool *isnull, bool tupleIsAlive, void *state)
{
	GraphAdjBuildState *buildstate = (GraphAdjBuildState *) state;

	if (!tupleIsAlive)
		return;
	graphadj_insert_edge(buildstate->index, buildstate->heap_oid,
						 values, isnull, tid);
	buildstate->heap_tuples += 1;
}

static IndexBuildResult *
graphadjbuild(Relation heap, Relation index, IndexInfo *indexInfo)
{
	GraphAdjBuildState buildstate = {0};
	IndexBuildResult *result;
	double		heap_tuples;

	graphadj_validate_definition(heap, index, indexInfo);
	graphadj_init_index(index, MAIN_FORKNUM);
	buildstate.index = index;
	buildstate.heap_oid = RelationGetRelid(heap);
	heap_tuples = table_index_build_scan(heap, index, indexInfo, true, true,
									 graphadj_build_callback, &buildstate, NULL);

	result = palloc_object(IndexBuildResult);
	result->heap_tuples = heap_tuples;
	result->index_tuples = buildstate.heap_tuples;
	return result;
}

static void
graphadjbuildempty(Relation index)
{
	graphadj_init_index(index, INIT_FORKNUM);
}

static bool
graphadjinsert(Relation index, Datum *values, bool *isnull,
			   ItemPointer heap_tid, Relation heap,
			   IndexUniqueCheck checkUnique, bool indexUnchanged,
			   IndexInfo *indexInfo)
{
	graphadj_insert_edge(index, RelationGetRelid(heap), values, isnull, heap_tid);
	return false;
}

/*
 * Reclaim only pages for which a fresh scan of every live Descriptor finds
 * no DataRef.  The caller holds AccessExclusiveLock on the index relation,
 * so no scan, writer, or debug compactor can retain or publish a reference
 * while this reachability decision is made.
 */
static BlockNumber
graphadj_reclaim_orphan_pages(Relation index)
{
	BlockNumber blocks = RelationGetNumberOfBlocks(index);
	bool	   *referenced = palloc0(sizeof(bool) * blocks);
	Buffer		slot_buffer;
	Page		slot_page;
	const unsigned char *slot_body;
	uint16		slot_count;
	uint32		ordinal;
	BlockNumber block;
	BlockNumber reclaimed = 0;
	BlockNumber slot_block = GRAPHADJ_SLOT_EXTENT_BLKNO;

	while (slot_block != InvalidBlockNumber)
	{
	slot_buffer = ReadBuffer(index, slot_block);
	LockBuffer(slot_buffer, BUFFER_LOCK_SHARE);
	slot_page = BufferGetPage(slot_buffer);
	graphadj_read_opaque(index, slot_page, GRAPHADJ_PAGE_ENDPOINT_SLOT,
		GRAPHADJ_DIRECTION_NONE, NULL);
	slot_body = (const unsigned char *) PageGetContents(slot_page);
	slot_count = graphadj_get_u16(slot_body);
	for (ordinal = GRAPHADJ_FIRST_SLOT_ORDINAL;
		 ordinal < GRAPHADJ_FIRST_SLOT_ORDINAL + slot_count; ordinal++)
	{
		const unsigned char *slot = graphadj_slot(slot_page, ordinal);
		GraphAdjDirection direction;

		if (graphadj_get_u32(slot + GRAPHADJ_SLOT_STATE_OFFSET) !=
			GRAPHADJ_SLOT_STATE_LIVE)
			continue;
		for (direction = GRAPHADJ_DIRECTION_OUT;
			 direction <= GRAPHADJ_DIRECTION_IN; direction++)
		{
			const unsigned char *descriptor = graphadj_descriptor(
				(unsigned char *) slot, direction);
			const uint32 offsets[] = {
				GRAPHADJ_DESC_BASE_OFFSET,
				GRAPHADJ_DESC_SEALED_OFFSET,
				GRAPHADJ_DESC_ACTIVE_OFFSET
			};
			int			i;

			for (i = 0; i < lengthof(offsets); i++)
			{
				GraphAdjDataRefData ref = graphadj_get_data_ref(
					descriptor + offsets[i]);

				if (!graphadj_data_ref_is_valid(&ref))
					continue;
				if (ref.block >= blocks ||
					ref.block <= GRAPHADJ_SLOT_EXTENT_BLKNO)
					ereport(ERROR,
							(errcode(ERRCODE_INDEX_CORRUPTED),
							 errmsg("graph_adj Descriptor contains an invalid data block")));
				referenced[ref.block] = true;
			}
		}
	}
	slot_block = graphadj_next_slot_page(index, slot_buffer);
	UnlockReleaseBuffer(slot_buffer);
	}

	for (block = GRAPHADJ_SLOT_EXTENT_BLKNO + 1; block < blocks; block++)
	{
		Buffer		buffer;
		Page		page;
		GraphAdjPageKind kind;
		GraphAdjDirection direction;
		uint64		generation;

		if (referenced[block])
			continue;
		buffer = ReadBuffer(index, block);
		LockBuffer(buffer, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buffer);
		if (PageIsNew(page) ||
			PageGetSpecialSize(page) != MAXALIGN(GRAPHADJ_OPAQUE_SIZE))
		{
			UnlockReleaseBuffer(buffer);
			continue;
		}
		graphadj_read_opaque_identity(index, page, &kind, &direction,
			&generation);
		UnlockReleaseBuffer(buffer);
		if (kind != GRAPHADJ_PAGE_PACKED_BASE && kind != GRAPHADJ_PAGE_DELTA)
			continue;

		/* Meta before data is the free-list lock order used by allocation too. */
		{
			Buffer		meta_buffer = ReadBuffer(index, GRAPHADJ_META_BLKNO);
			Page		meta_page;
			unsigned char *meta;
			BlockNumber old_head;
			GenericXLogState *state;

			LockBuffer(meta_buffer, BUFFER_LOCK_EXCLUSIVE);
			meta_page = BufferGetPage(meta_buffer);
			graphadj_read_opaque(index, meta_page, GRAPHADJ_PAGE_META,
				GRAPHADJ_DIRECTION_NONE, NULL);
			buffer = ReadBuffer(index, block);
			LockBuffer(buffer, BUFFER_LOCK_EXCLUSIVE);
			page = BufferGetPage(buffer);
			graphadj_read_opaque_identity(index, page, &kind, &direction,
				&generation);
			if (kind != GRAPHADJ_PAGE_PACKED_BASE &&
				kind != GRAPHADJ_PAGE_DELTA)
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("graph_adj orphan page changed identity during reclaim")));
			meta = (unsigned char *) PageGetContents(meta_page);
			old_head = graphadj_get_u32(meta + GRAPHADJ_META_FREE_HEAD_OFFSET);

			state = GenericXLogStart(index);
			meta_page = GenericXLogRegisterBuffer(state, meta_buffer, 0);
			page = GenericXLogRegisterBuffer(state, buffer,
				GENERIC_XLOG_FULL_IMAGE);
			meta = (unsigned char *) PageGetContents(meta_page);
			graphadj_put_u32(meta + GRAPHADJ_META_FREE_HEAD_OFFSET, block);
			PageInit(page, BLCKSZ, MAXALIGN(GRAPHADJ_OPAQUE_SIZE));
			graphadj_write_opaque(page, GRAPHADJ_PAGE_FREE,
				GRAPHADJ_DIRECTION_NONE, generation);
			graphadj_set_rightlink(page, old_head);
			GenericXLogFinish(state);
			graphadj_maybe_crash(index, 0, "free_page_published");
			UnlockReleaseBuffer(buffer);
			UnlockReleaseBuffer(meta_buffer);
			reclaimed++;
		}
	}
	pfree(referenced);
	return reclaimed;
}

/* Caller holds the index reclamation barrier and this Slot page EX lock.
 * Copy/filter before starting WAL; callbacks may raise an error. */
static void
graphadj_vacuum_inline(Relation index, Buffer buffer, uint32 ordinal,
					  IndexBulkDeleteCallback callback, void *callback_state,
					  uint64 *removed)
{
	unsigned char kept[2][GRAPHADJ_INLINE_ITEMS * GRAPHADJ_ENTRY_SIZE];
	uint32 counts[2];
	uint32 survivors[2] = {0, 0};
	unsigned char *slot = graphadj_slot(BufferGetPage(buffer), ordinal);
	GraphAdjDirection direction;
	GenericXLogState *state;
	Page page;

	for (direction = GRAPHADJ_DIRECTION_OUT;
		 direction <= GRAPHADJ_DIRECTION_IN; direction++)
	{
		uint32 lane = direction - GRAPHADJ_DIRECTION_OUT;
		unsigned char *entries = graphadj_inline_entries(slot, direction);
		uint32 i;

		counts[lane] = graphadj_inline_count(graphadj_descriptor(slot, direction));
		for (i = 0; i < counts[lane]; i++)
		{
			unsigned char *entry = entries + (Size) i * GRAPHADJ_ENTRY_SIZE;
			ItemPointerData tid;

			ItemPointerSet(&tid,
				graphadj_get_u32(entry + GRAPHADJ_ENTRY_TID_BLOCK_OFFSET),
				graphadj_get_u16(entry + GRAPHADJ_ENTRY_TID_OFFSET_OFFSET));
			if (!ItemPointerIsValid(&tid) ||
				ItemPointerGetBlockNumber(&tid) == InvalidBlockNumber)
				ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
								errmsg("graph_adj inline entry contains an invalid heap root TID")));
			if (!callback(&tid, callback_state))
			{
				memcpy(kept[lane] + (Size) survivors[lane] * GRAPHADJ_ENTRY_SIZE,
					   entry, GRAPHADJ_ENTRY_SIZE);
				survivors[lane]++;
			}
		}
	}
	if (counts[0] == survivors[0] && counts[1] == survivors[1])
		return;
	state = GenericXLogStart(index);
	page = GenericXLogRegisterBuffer(state, buffer, 0);
	slot = graphadj_slot(page, ordinal);
	for (direction = GRAPHADJ_DIRECTION_OUT;
		 direction <= GRAPHADJ_DIRECTION_IN; direction++)
	{
		uint32 lane = direction - GRAPHADJ_DIRECTION_OUT;
		unsigned char *descriptor = graphadj_descriptor(slot, direction);
		unsigned char *entries = graphadj_inline_entries(slot, direction);

		if (counts[lane] == survivors[lane])
			continue;
		memcpy(entries, kept[lane], (Size) survivors[lane] * GRAPHADJ_ENTRY_SIZE);
		MemSet(entries + (Size) survivors[lane] * GRAPHADJ_ENTRY_SIZE, 0,
			   (GRAPHADJ_INLINE_ITEMS - survivors[lane]) * GRAPHADJ_ENTRY_SIZE);
		graphadj_put_u32(descriptor + GRAPHADJ_DESC_INLINE_COUNT_OFFSET, survivors[lane]);
		graphadj_put_u64(descriptor + GRAPHADJ_DESC_VIEW_GENERATION_OFFSET,
			graphadj_get_u64(descriptor + GRAPHADJ_DESC_VIEW_GENERATION_OFFSET) + 1);
		removed[direction] += counts[lane] - survivors[lane];
	}
	GenericXLogFinish(state);
}

static IndexBulkDeleteResult *
graphadjbulkdelete(IndexVacuumInfo *info, IndexBulkDeleteResult *stats,
				   IndexBulkDeleteCallback callback, void *callback_state)
{
	Oid			index_oid = RelationGetRelid(info->index);
	int64	   *keys = NULL;
	uint32		slot_count = 0;
	uint64		removed_direction_entries[3] = {0};
	BlockNumber reclaimed = 0;

	if (stats == NULL)
		stats = palloc0_object(IndexBulkDeleteResult);

	/*
	 * This conservative GS-050 barrier makes the all-reader/all-builder reuse
	 * horizon explicit.  Later work may replace it with finer epochs without
	 * weakening the on-disk generation check.
	 */
	LockRelationOid(index_oid, AccessExclusiveLock);
	PG_TRY();
	{
		Buffer		slot_buffer;
		BlockNumber slot_block = GRAPHADJ_SLOT_EXTENT_BLKNO;
		Page		slot_page;
		const unsigned char *body;
		uint32		ordinal;
		uint32		i;

		while (slot_block != InvalidBlockNumber)
		{
		uint16 page_count;

		slot_buffer = ReadBuffer(info->index, slot_block);
		LockBuffer(slot_buffer, BUFFER_LOCK_EXCLUSIVE);
		slot_page = BufferGetPage(slot_buffer);
		graphadj_read_opaque(info->index, slot_page,
			GRAPHADJ_PAGE_ENDPOINT_SLOT, GRAPHADJ_DIRECTION_NONE, NULL);
		body = (const unsigned char *) PageGetContents(slot_page);
		page_count = graphadj_get_u16(body);
		if (page_count > 0)
		{
			Size bytes = mul_size(sizeof(int64), add_size(slot_count, page_count));

			keys = keys ? repalloc(keys, bytes) : palloc(bytes);
		}
		for (ordinal = GRAPHADJ_FIRST_SLOT_ORDINAL, i = slot_count;
			 ordinal < GRAPHADJ_FIRST_SLOT_ORDINAL + page_count;
			 ordinal++, i++)
		{
			const unsigned char *slot = graphadj_slot(slot_page, ordinal);

			if (graphadj_get_u32(slot + GRAPHADJ_SLOT_STATE_OFFSET) !=
				GRAPHADJ_SLOT_STATE_LIVE)
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("graph_adj Slot extent contains a non-live allocated Slot")));
			keys[i] = (int64) graphadj_get_u64(slot + GRAPHADJ_SLOT_KEY_OFFSET);
			graphadj_vacuum_inline(info->index, slot_buffer, ordinal,
				callback, callback_state, removed_direction_entries);
		}
		slot_count += page_count;
		slot_block = graphadj_next_slot_page(info->index, slot_buffer);
		UnlockReleaseBuffer(slot_buffer);
		}

		for (i = 0; i < slot_count; i++)
		{
			GraphAdjDirection direction;

			for (direction = GRAPHADJ_DIRECTION_OUT;
				 direction <= GRAPHADJ_DIRECTION_IN; direction++)
			{
				GraphAdjCompactionResult result;

				CHECK_FOR_INTERRUPTS();
				if (graphadj_compact_endpoint(info->index, keys[i], direction, 0,
						callback, callback_state, &result))
					removed_direction_entries[direction] += result.removed_items;
			}
		}
		reclaimed = graphadj_reclaim_orphan_pages(info->index);
		/*
		 * A PANIC may leave a WAL-replayed index entry from an uncommitted
		 * insertion in only one direction.  The heap callback is authoritative,
		 * so remove that residue without inventing half a logical tuple.  Fully
		 * paired removals still contribute one logical removal each.
		 */
		stats->tuples_removed += (double) Min(
			removed_direction_entries[GRAPHADJ_DIRECTION_OUT],
			removed_direction_entries[GRAPHADJ_DIRECTION_IN]);
		stats->pages_newly_deleted += reclaimed;
		graphadj_fill_vacuum_stats(info->index, info, stats);
		if (keys)
			pfree(keys);
	}
	PG_FINALLY();
	{
		UnlockRelationOid(index_oid, AccessExclusiveLock);
	}
	PG_END_TRY();
	return stats;
}

static IndexBulkDeleteResult *
graphadjvacuumcleanup(IndexVacuumInfo *info, IndexBulkDeleteResult *stats)
{
	Oid			index_oid = RelationGetRelid(info->index);
	BlockNumber reclaimed = 0;

	if (stats == NULL)
		stats = palloc0_object(IndexBulkDeleteResult);
	if (info->analyze_only)
	{
		stats->num_pages = RelationGetNumberOfBlocks(info->index);
		return stats;
	}
	LockRelationOid(index_oid, AccessExclusiveLock);
	PG_TRY();
	{
		reclaimed = graphadj_reclaim_orphan_pages(info->index);
		stats->pages_newly_deleted += reclaimed;
		graphadj_fill_vacuum_stats(info->index, info, stats);
	}
	PG_FINALLY();
	{
		UnlockRelationOid(index_oid, AccessExclusiveLock);
	}
	PG_END_TRY();
	return stats;
}

static void
graphadjcostestimate(PlannerInfo *root, IndexPath *path, double loop_count,
					 Cost *indexStartupCost, Cost *indexTotalCost,
					 Selectivity *indexSelectivity, double *indexCorrelation,
					 double *indexPages)
{
	GenericCosts costs = {0};

	/* Until graph statistics arrive, use PG's normal equality selectivity. */
	costs.numNonLeafPages = 1;
	genericcostestimate(root, path, loop_count, &costs);
	*indexStartupCost = costs.indexStartupCost;
	*indexTotalCost = costs.indexTotalCost;
	*indexSelectivity = costs.indexSelectivity;
	*indexCorrelation = 0.0;
	*indexPages = costs.numIndexPages;
}

static bytea *
graphadjoptions(Datum reloptions, bool validate)
{
	if (DatumGetPointer(reloptions) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("graph_adj does not support storage parameters")));
	return NULL;
}

static bool
graphadjvalidate(Oid opclassoid)
{
	return true;
}

static void
graphadj_read_opaque_identity(Relation index, Page page,
							  GraphAdjPageKind *kind,
							  GraphAdjDirection *direction,
							  uint64 *allocation_generation)
{
	const unsigned char *opaque;
	uint16		stored_kind;
	uint16		stored_direction;
	uint32		format;

	if (PageIsNew(page) || PageGetSpecialSize(page) != MAXALIGN(GRAPHADJ_OPAQUE_SIZE))
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("index \"%s\" is not a graph_adj V2 index",
						RelationGetRelationName(index))));
	opaque = (const unsigned char *) PageGetSpecialPointer(page);
	stored_kind = graphadj_get_u16(opaque + GRAPHADJ_OPAQUE_KIND_OFFSET);
	stored_direction = graphadj_get_u16(
		opaque + GRAPHADJ_OPAQUE_DIRECTION_OFFSET);
	format = graphadj_get_u32(opaque + GRAPHADJ_OPAQUE_FORMAT_OFFSET);
	if (format != GRAPHADJ_FORMAT_VERSION)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("graph_adj index \"%s\" has unsupported format version %u",
						RelationGetRelationName(index), format),
					 errhint("REINDEX with a server supporting this graph_adj disk format.")));
	if (kind)
		*kind = (GraphAdjPageKind) stored_kind;
	if (direction)
		*direction = (GraphAdjDirection) stored_direction;
	if (allocation_generation)
		*allocation_generation = graphadj_get_u64(
			opaque + GRAPHADJ_OPAQUE_ALLOCATION_GENERATION_OFFSET);
}

static void
graphadj_read_opaque(Relation index, Page page, GraphAdjPageKind expected_kind,
						 GraphAdjDirection expected_direction,
						 uint64 *allocation_generation)
{
	GraphAdjPageKind kind;
	GraphAdjDirection direction;

	graphadj_read_opaque_identity(index, page, &kind, &direction,
								  allocation_generation);
	if (kind != expected_kind || direction != expected_direction)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("graph_adj page kind or direction mismatch"),
				 errdetail("Expected kind %u direction %u, found kind %u direction %u.",
						   expected_kind, expected_direction, kind, direction)));
}

static void
graphadj_read_meta(Relation index, GraphAdjMetaData *result,
				   uint64 *allocation_generation)
{
	Buffer		buffer;
	Page		page;
	const unsigned char *meta;

	if (RelationGetNumberOfBlocks(index) < 4)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("graph_adj index \"%s\" is missing required pages",
						RelationGetRelationName(index))));
	buffer = ReadBuffer(index, GRAPHADJ_META_BLKNO);
	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buffer);
	graphadj_read_opaque(index, page, GRAPHADJ_PAGE_META,
						 GRAPHADJ_DIRECTION_NONE, allocation_generation);
	meta = (const unsigned char *) PageGetContents(page);
	result->magic = graphadj_get_u32(meta + GRAPHADJ_META_MAGIC_OFFSET);
	result->format_version = graphadj_get_u32(meta + GRAPHADJ_META_FORMAT_OFFSET);
	result->build_generation = graphadj_get_u64(
		meta + GRAPHADJ_META_BUILD_GENERATION_OFFSET);
	result->out_root = graphadj_get_u32(meta + GRAPHADJ_META_OUT_ROOT_OFFSET);
	result->in_root = graphadj_get_u32(meta + GRAPHADJ_META_IN_ROOT_OFFSET);
	result->slot_extent_head = graphadj_get_u32(
		meta + GRAPHADJ_META_SLOT_EXTENT_OFFSET);
	result->retirement_head = graphadj_get_u32(
		meta + GRAPHADJ_META_RETIREMENT_HEAD_OFFSET);
	result->free_head = graphadj_get_u32(meta + GRAPHADJ_META_FREE_HEAD_OFFSET);
	result->slot_tail = graphadj_get_u32(meta + GRAPHADJ_META_SLOT_TAIL_OFFSET);
	result->pending_slot = graphadj_get_vertex_ref(meta + GRAPHADJ_META_PENDING_SLOT_OFFSET);
	UnlockReleaseBuffer(buffer);

	if (result->magic != GRAPHADJ_MAGIC)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("index \"%s\" has invalid graph_adj magic",
						RelationGetRelationName(index))));
	if (result->format_version != GRAPHADJ_FORMAT_VERSION)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("graph_adj index \"%s\" has unsupported meta format version %u",
						RelationGetRelationName(index), result->format_version),
					 errhint("REINDEX with a server supporting this graph_adj disk format.")));
}

static void
graphadj_scan_append(GraphAdjScanOpaque opaque, BlockNumber block,
					 OffsetNumber offset)
{
	if (opaque->count == opaque->capacity)
	{
		opaque->capacity = opaque->capacity == 0 ? 64 : opaque->capacity * 2;
		if (opaque->items == NULL)
			opaque->items = palloc(sizeof(ItemPointerData) * opaque->capacity);
		else
			opaque->items = repalloc(opaque->items,
								 sizeof(ItemPointerData) * opaque->capacity);
	}
	ItemPointerSet(&opaque->items[opaque->count], block, offset);
	opaque->count++;
}

static void
graphadj_candidate_append(GraphAdjCandidateVector *vector, int64 opposite_key,
						  const GraphAdjVertexRefData *opposite_ref,
						  BlockNumber block, OffsetNumber offset)
{
	GraphAdjCandidateData *candidate;

	if (vector->count == vector->capacity)
	{
		vector->capacity = vector->capacity == 0 ? 16 : vector->capacity * 2;
		if (vector->items == NULL)
			vector->items = palloc(sizeof(GraphAdjCandidateData) *
								  vector->capacity);
		else
			vector->items = repalloc(vector->items,
				sizeof(GraphAdjCandidateData) * vector->capacity);
	}
	candidate = &vector->items[vector->count++];
	candidate->opposite_key = opposite_key;
	candidate->opposite_ref = *opposite_ref;
	ItemPointerSet(&candidate->root_tid, block, offset);
}

/* Copy under the Slot content lock; never hand a page pointer to a reader. */
static void
graphadj_collect_inline_candidates(unsigned char *slot,
								  GraphAdjDirection direction,
								  bool filter_opposite, int64 opposite_key,
								  GraphAdjCandidateVector *candidates)
{
	uint32 count = graphadj_inline_count(graphadj_descriptor(slot, direction));
	unsigned char *entries = graphadj_inline_entries(slot, direction);
	uint32 i;

	for (i = 0; i < count; i++)
	{
		unsigned char *entry = entries + (Size) i * GRAPHADJ_ENTRY_SIZE;
		int64 key = (int64) graphadj_get_u64(entry + GRAPHADJ_ENTRY_KEY_OFFSET);
		GraphAdjVertexRefData ref;
		BlockNumber block = graphadj_get_u32(entry + GRAPHADJ_ENTRY_TID_BLOCK_OFFSET);
		OffsetNumber offset = graphadj_get_u16(entry + GRAPHADJ_ENTRY_TID_OFFSET_OFFSET);

		if (block == InvalidBlockNumber || !OffsetNumberIsValid(offset))
			ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
							errmsg("graph_adj inline entry contains an invalid heap root TID")));
		if (filter_opposite && key != opposite_key)
			continue;
		ref = graphadj_get_vertex_ref(entry + GRAPHADJ_ENTRY_REF_OFFSET);
		graphadj_candidate_append(candidates, key, &ref, block, offset);
	}
}

static void
graphadj_collect_data_candidates(Relation index, Page slot_page,
								 const GraphAdjVertexRefData *owner,
								 GraphAdjDirection direction,
								 const GraphAdjDataRefData *data_ref,
								 GraphAdjPageKind expected_kind,
								 bool expected_sealed,
								 bool filter_opposite, int64 opposite_key,
								 GraphAdjCandidateVector *candidates)
{
	Buffer		data_buffer;
	Page		data_page;
	const unsigned char *segment;
	GraphAdjVertexRefData stored_owner;
	uint64		allocation_generation;
	uint32		published;
	uint32		capacity;
	uint32		i;

	if (!graphadj_data_ref_is_valid(data_ref))
		return;
	data_buffer = ReadBuffer(index, data_ref->block);
	LockBuffer(data_buffer, BUFFER_LOCK_SHARE);
	data_page = BufferGetPage(data_buffer);
	graphadj_read_opaque(index, data_page, expected_kind,
						 direction, &allocation_generation);
	if (allocation_generation != data_ref->segment_generation)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("graph_adj scan found a stale DataRef")));
	segment = (const unsigned char *) data_page + data_ref->segment_offset;
	stored_owner = graphadj_get_vertex_ref(
		segment + GRAPHADJ_SEGMENT_OWNER_OFFSET);
	if (!graphadj_vertex_ref_equal(&stored_owner, owner) ||
		graphadj_get_u16(segment + GRAPHADJ_SEGMENT_DIRECTION_OFFSET) !=
		(uint16) direction ||
		(graphadj_get_u16(segment + GRAPHADJ_SEGMENT_SEALED_OFFSET) != 0) !=
		expected_sealed)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("graph_adj scan segment identity mismatch")));
	published = graphadj_get_u32(segment + GRAPHADJ_SEGMENT_PUBLISHED_OFFSET);
	capacity = graphadj_get_u32(segment + GRAPHADJ_SEGMENT_CAPACITY_OFFSET);
	if (published > capacity ||
		data_ref->segment_offset + GRAPHADJ_SEGMENT_HEADER_SIZE +
		((Size) published * GRAPHADJ_ENTRY_SIZE) >
		((PageHeader) data_page)->pd_lower)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("graph_adj scan found an invalid published boundary")));

	for (i = 0; i < published; i++)
	{
		const unsigned char *entry = segment + GRAPHADJ_SEGMENT_HEADER_SIZE +
			((Size) i * GRAPHADJ_ENTRY_SIZE);
		int64		stored_key = (int64) graphadj_get_u64(
			entry + GRAPHADJ_ENTRY_KEY_OFFSET);
		GraphAdjVertexRefData opposite = graphadj_get_vertex_ref(
			entry + GRAPHADJ_ENTRY_REF_OFFSET);
		BlockNumber block;
		OffsetNumber offset;

		if (filter_opposite && stored_key != opposite_key)
			continue;
		/* Checked after releasing the owner/data locks: the opposite Slot may
		 * live on another page, whose writer may in turn need this data page. */
		block = graphadj_get_u32(entry + GRAPHADJ_ENTRY_TID_BLOCK_OFFSET);
		offset = graphadj_get_u16(entry + GRAPHADJ_ENTRY_TID_OFFSET_OFFSET);
		if (block == InvalidBlockNumber || !OffsetNumberIsValid(offset))
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("graph_adj scan found an invalid heap root TID")));
		graphadj_candidate_append(candidates, stored_key, &opposite,
							  block, offset);
	}
	UnlockReleaseBuffer(data_buffer);
}

static bool
graphadj_check_candidates(Relation index, const GraphAdjCandidateVector *candidates,
						  GraphAdjRefCache *cache)
{
	Buffer buffer = InvalidBuffer;
	BlockNumber current = InvalidBlockNumber;
	BlockNumber blocks = InvalidBlockNumber;
	bool valid = true;

	for (uint32 i = 0; i < candidates->count; i++)
	{
		const GraphAdjCandidateData *candidate = &candidates->items[i];
		const GraphAdjVertexRefData *ref = &candidate->opposite_ref;
		BlockNumber block = GraphAdjSlotIdGetBlock(ref->slot_id);
		GraphAdjRefCacheEntry *cached = cache ?
			&cache->entries[(ref->slot_id ^ (ref->slot_id >> 32)) % GRAPHADJ_REF_CACHE_SIZE] : NULL;

		if (cached && cached->key == candidate->opposite_key &&
			cached->ref.slot_generation != 0 && graphadj_vertex_ref_equal(&cached->ref, ref))
			continue;
		if (blocks == InvalidBlockNumber)
			blocks = RelationGetNumberOfBlocks(index);
		if (!GraphAdjSlotIdIsValid(ref->slot_id) || block >= blocks)
		{
			valid = false;
			break;
		}
		if (block != current)
		{
			if (BufferIsValid(buffer))
				UnlockReleaseBuffer(buffer);
			buffer = ReadBuffer(index, block);
			LockBuffer(buffer, BUFFER_LOCK_SHARE);
			graphadj_read_opaque(index, BufferGetPage(buffer), GRAPHADJ_PAGE_ENDPOINT_SLOT,
				GRAPHADJ_DIRECTION_NONE, NULL);
			current = block;
		}
		if (!graphadj_ref_matches_slot(BufferGetPage(buffer), block,
				candidate->opposite_key, ref))
		{
			valid = false;
			break;
		}
		if (cached)
		{
			cached->key = candidate->opposite_key;
			cached->ref = *ref;
		}
	}
	if (BufferIsValid(buffer))
		UnlockReleaseBuffer(buffer);
	return valid;
}

static void
graphadj_collect_candidates(Relation index, Buffer slot_buffer,
							const GraphAdjVertexRefData *owner,
							GraphAdjDirection direction,
							bool filter_opposite, int64 opposite_key,
							GraphAdjCandidateVector *candidates,
							GraphAdjRefCache *cache)
{
	Page slot_page = BufferGetPage(slot_buffer);
	uint32		ordinal = GraphAdjSlotIdGetOrdinal(owner->slot_id);
	const unsigned char *slot = graphadj_slot(slot_page, ordinal);
	const unsigned char *descriptor = graphadj_descriptor(
		(unsigned char *) slot, direction);
	GraphAdjDataRefData base = graphadj_get_data_ref(
		descriptor + GRAPHADJ_DESC_BASE_OFFSET);
	GraphAdjDataRefData sealed = graphadj_get_data_ref(
		descriptor + GRAPHADJ_DESC_SEALED_OFFSET);
	GraphAdjDataRefData active = graphadj_get_data_ref(
		descriptor + GRAPHADJ_DESC_ACTIVE_OFFSET);

	graphadj_collect_inline_candidates((unsigned char *) slot, direction,
		filter_opposite, opposite_key, candidates);
	graphadj_collect_data_candidates(index, slot_page, owner, direction,
		&base, GRAPHADJ_PAGE_PACKED_BASE, true,
		filter_opposite, opposite_key, candidates);
	graphadj_collect_data_candidates(index, slot_page, owner, direction,
		&sealed, GRAPHADJ_PAGE_DELTA, true,
		filter_opposite, opposite_key, candidates);
	graphadj_collect_data_candidates(index, slot_page, owner, direction,
		&active, GRAPHADJ_PAGE_DELTA, false,
		filter_opposite, opposite_key, candidates);
	/* The relation's retained lock excludes reclamation. The copied entries
	 * remain safe while ordinary writers/compactors update their descriptors. */
	LockBuffer(slot_buffer, BUFFER_LOCK_UNLOCK);
	if (!graphadj_check_candidates(index, candidates, cache))
		ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
						errmsg("graph_adj scan found a conflicting persistent VertexRef")));
	LockBuffer(slot_buffer, BUFFER_LOCK_SHARE);
}

static void
graphadj_collect_scan_descriptor(IndexScanDesc scan, Buffer slot_buffer,
								 const GraphAdjVertexRefData *owner,
								 GraphAdjDirection direction,
								 bool filter_opposite,
								 int64 opposite_key)
{
	GraphAdjScanOpaque opaque = (GraphAdjScanOpaque) scan->opaque;
	GraphAdjCandidateVector candidates = {0};
	uint32		i;

	graphadj_collect_candidates(scan->indexRelation, slot_buffer, owner,
								direction, filter_opposite, opposite_key,
								&candidates, NULL);
	for (i = 0; i < candidates.count; i++)
		graphadj_scan_append(opaque,
			ItemPointerGetBlockNumber(&candidates.items[i].root_tid),
			ItemPointerGetOffsetNumber(&candidates.items[i].root_tid));
	if (candidates.items)
		pfree(candidates.items);
}

static int64
graphadj_scankey_int64(ScanKey key)
{
	switch (key->sk_subtype)
	{
		case InvalidOid:
		case INT8OID:
			return DatumGetInt64(key->sk_argument);
		case INT4OID:
			return (int64) DatumGetInt32(key->sk_argument);
		case INT2OID:
			return (int64) DatumGetInt16(key->sk_argument);
		default:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("graph_adj scan received unsupported equality subtype %s",
							format_type_be(key->sk_subtype))));
	}
	pg_unreachable();
}

static void
graphadj_prepare_scan(IndexScanDesc scan)
{
	GraphAdjScanOpaque opaque = (GraphAdjScanOpaque) scan->opaque;
	bool		has_source = false;
	bool		has_destination = false;
	bool		empty = false;
	int64		source = 0;
	int64		destination = 0;
	GraphAdjDirection direction;
	int64		endpoint;
	bool		filter_opposite;
	int64		opposite_key;
	int		keyno;
	Buffer		slot_buffer;
	Page		slot_page;

	opaque->initialized = true;
	for (keyno = 0; keyno < scan->numberOfKeys; keyno++)
	{
		ScanKey		key = &scan->keyData[keyno];
		int64		value;

		if ((key->sk_flags & SK_ISNULL) != 0)
		{
			empty = true;
			continue;
		}
		if (key->sk_strategy != 1 ||
			(key->sk_attno != 1 && key->sk_attno != 2))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("graph_adj ordinary scans support only endpoint equality")));
		value = graphadj_scankey_int64(key);
		if (key->sk_attno == 1)
		{
			if (has_source && source != value)
				empty = true;
			has_source = true;
			source = value;
		}
		else
		{
			if (has_destination && destination != value)
				empty = true;
			has_destination = true;
			destination = value;
		}
	}
	if (empty)
		return;

	if (has_source)
	{
		direction = GRAPHADJ_DIRECTION_OUT;
		endpoint = source;
		filter_opposite = has_destination;
		opposite_key = destination;
	}
	else if (has_destination)
	{
		direction = GRAPHADJ_DIRECTION_IN;
		endpoint = destination;
		filter_opposite = false;
		opposite_key = 0;
	}
	else
	{
		/* A no-key scan walks OUT descriptors so each heap edge appears once. */
		const unsigned char *body;
		uint16		count;
		uint32		ordinal;
		BlockNumber slot_block = GRAPHADJ_SLOT_EXTENT_BLKNO;

		while (slot_block != InvalidBlockNumber)
		{
		slot_buffer = ReadBuffer(scan->indexRelation,
								 slot_block);
		LockBuffer(slot_buffer, BUFFER_LOCK_SHARE);
		slot_page = BufferGetPage(slot_buffer);
		graphadj_read_opaque(scan->indexRelation, slot_page,
							 GRAPHADJ_PAGE_ENDPOINT_SLOT,
							 GRAPHADJ_DIRECTION_NONE, NULL);
		body = (const unsigned char *) PageGetContents(slot_page);
		count = graphadj_get_u16(body);
		for (ordinal = GRAPHADJ_FIRST_SLOT_ORDINAL;
			 ordinal < GRAPHADJ_FIRST_SLOT_ORDINAL + count; ordinal++)
		{
			const unsigned char *slot = graphadj_slot(slot_page, ordinal);
			GraphAdjVertexRefData ref = {
				.slot_id = GraphAdjMakeSlotId(slot_block,
										  ordinal),
				.slot_generation = graphadj_get_u64(
					slot + GRAPHADJ_SLOT_GENERATION_OFFSET),
				.endpoint_domain = graphadj_get_u32(
					slot + GRAPHADJ_SLOT_DOMAIN_OFFSET)
			};

			graphadj_collect_scan_descriptor(scan, slot_buffer, &ref,
				GRAPHADJ_DIRECTION_OUT, false, 0);
		}
		slot_block = graphadj_next_slot_page(scan->indexRelation, slot_buffer);
		UnlockReleaseBuffer(slot_buffer);
		}
		return;
	}

	{
		GraphAdjVertexRefData ref;
		bool		found;

		found = graphadj_directory_lookup_relation(scan->indexRelation,
			direction == GRAPHADJ_DIRECTION_OUT ?
			GRAPHADJ_OUT_ROOT_BLKNO : GRAPHADJ_IN_ROOT_BLKNO,
			direction, endpoint, &ref);
		if (!found)
			return;

		slot_buffer = ReadBuffer(scan->indexRelation,
								 GraphAdjSlotIdGetBlock(ref.slot_id));
		LockBuffer(slot_buffer, BUFFER_LOCK_SHARE);
		slot_page = BufferGetPage(slot_buffer);
		graphadj_read_opaque(scan->indexRelation, slot_page,
							 GRAPHADJ_PAGE_ENDPOINT_SLOT,
							 GRAPHADJ_DIRECTION_NONE, NULL);
		graphadj_validate_slot_identity(scan->indexRelation, slot_buffer,
									  endpoint, &ref);
		graphadj_collect_scan_descriptor(scan, slot_buffer, &ref, direction,
								 filter_opposite, opposite_key);
		UnlockReleaseBuffer(slot_buffer);
	}
}

static IndexScanDesc
graphadjbeginscan(Relation index, int nkeys, int norderbys)
{
	GraphAdjMetaData meta;
	IndexScanDesc scan;

	graphadj_read_meta(index, &meta, NULL);
	scan = RelationGetIndexScan(index, nkeys, norderbys);
	scan->opaque = palloc0_object(GraphAdjScanOpaqueData);
	return scan;
}

static void
graphadjrescan(IndexScanDesc scan, ScanKey keys, int nkeys,
			   ScanKey orderbys, int norderbys)
{
	GraphAdjScanOpaque opaque = (GraphAdjScanOpaque) scan->opaque;

	if (keys && scan->numberOfKeys > 0)
		memmove(scan->keyData, keys, scan->numberOfKeys * sizeof(ScanKeyData));
	if (opaque->items != NULL)
		pfree(opaque->items);
	MemSet(opaque, 0, sizeof(*opaque));
}

static bool
graphadjgettuple(IndexScanDesc scan, ScanDirection direction)
{
	GraphAdjScanOpaque opaque = (GraphAdjScanOpaque) scan->opaque;

	if (direction != ForwardScanDirection)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("graph_adj supports forward ordinary scans only")));
	if (!opaque->initialized)
		graphadj_prepare_scan(scan);
	if (opaque->next >= opaque->count)
		return false;
	scan->xs_heaptid = opaque->items[opaque->next++];
	scan->xs_recheck = false;
	return true;
}

static void
graphadjendscan(IndexScanDesc scan)
{
	GraphAdjScanOpaque opaque = (GraphAdjScanOpaque) scan->opaque;

	if (opaque == NULL)
		return;
	if (opaque->items != NULL)
		pfree(opaque->items);
	pfree(opaque);
	scan->opaque = NULL;
}

static CompareType
graphadjtranslatestrategy(StrategyNumber strategy, Oid opfamily)
{
	return strategy == 1 ? COMPARE_EQ : COMPARE_INVALID;
}

static StrategyNumber
graphadjtranslatecmptype(CompareType cmptype, Oid opfamily)
{
	return cmptype == COMPARE_EQ ? 1 : InvalidStrategy;
}

Datum
graphadjhandler(PG_FUNCTION_ARGS)
{
	static const IndexAmRoutine amroutine = {
		.type = T_IndexAmRoutine,
		.amgetgraphprovider = GetGgcGraphAdjProvider,
		.amstrategies = 1,
		.amsupport = 0,
		.amoptsprocnum = 0,
		.amcanorder = false,
		.amcanorderbyop = false,
		.amcanhash = false,
		.amconsistentequality = true,
		.amconsistentordering = false,
		.amcanbackward = false,
		.amcanunique = false,
		.amcanmulticol = true,
		.amoptionalkey = true,
		.amsearcharray = false,
		.amsearchnulls = false,
		.amstorage = false,
		.amclusterable = false,
		.ampredlocks = false,
		.amcanparallel = false,
		.amcanbuildparallel = false,
		.amcaninclude = false,
		.amusemaintenanceworkmem = false,
		.amsummarizing = false,
		.amparallelvacuumoptions = 0,
		.amkeytype = INT8OID,
		.ambuild = graphadjbuild,
		.ambuildempty = graphadjbuildempty,
		.aminsert = graphadjinsert,
		.aminsertcleanup = NULL,
		.ambulkdelete = graphadjbulkdelete,
		.amvacuumcleanup = graphadjvacuumcleanup,
		.amcanreturn = NULL,
		.amcostestimate = graphadjcostestimate,
		.amgettreeheight = NULL,
		.amoptions = graphadjoptions,
		.amproperty = NULL,
		.ambuildphasename = NULL,
		.amvalidate = graphadjvalidate,
		.amadjustmembers = NULL,
		.ambeginscan = graphadjbeginscan,
		.amrescan = graphadjrescan,
		.amgettuple = graphadjgettuple,
		.amgetbitmap = NULL,
		.amendscan = graphadjendscan,
		.ammarkpos = NULL,
		.amrestrpos = NULL,
		.amestimateparallelscan = NULL,
		.aminitparallelscan = NULL,
		.amparallelrescan = NULL,
		.amtranslatestrategy = graphadjtranslatestrategy,
		.amtranslatecmptype = graphadjtranslatecmptype,
	};

	PG_RETURN_POINTER(&amroutine);
}

static const char *
graphadj_kind_name(GraphAdjPageKind kind)
{
	switch (kind)
	{
		case GRAPHADJ_PAGE_META:
			return "META";
		case GRAPHADJ_PAGE_DIR_INTERNAL:
			return "DIR_INTERNAL";
		case GRAPHADJ_PAGE_DIR_LEAF:
			return "DIR_LEAF";
		case GRAPHADJ_PAGE_ENDPOINT_SLOT:
			return "ENDPOINT_SLOT";
		case GRAPHADJ_PAGE_PACKED_BASE:
			return "PACKED_BASE";
		case GRAPHADJ_PAGE_DELTA:
			return "DELTA";
		case GRAPHADJ_PAGE_RETIRED:
			return "RETIRED";
		case GRAPHADJ_PAGE_FREE:
			return "FREE";
	}
	return "UNKNOWN";
}

static const char *
graphadj_direction_name(GraphAdjDirection direction)
{
	switch (direction)
	{
		case GRAPHADJ_DIRECTION_NONE:
			return "NONE";
		case GRAPHADJ_DIRECTION_OUT:
			return "OUT";
		case GRAPHADJ_DIRECTION_IN:
			return "IN";
	}
	return "UNKNOWN";
}

static void
graphadj_read_page_identity(Relation index, BlockNumber block,
							GraphAdjPageKind expected_kind,
							GraphAdjDirection expected_direction)
{
	Buffer		buffer = ReadBuffer(index, block);
	Page		page;

	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buffer);
	graphadj_read_opaque(index, page, expected_kind, expected_direction, NULL);
	UnlockReleaseBuffer(buffer);
}

static GraphAdjPageKind
graphadj_read_page_kind(Relation index, BlockNumber block,
						GraphAdjDirection expected_direction)
{
	Buffer		buffer = ReadBuffer(index, block);
	Page		page;
	GraphAdjPageKind kind;
	GraphAdjDirection direction;

	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buffer);
	graphadj_read_opaque_identity(index, page, &kind, &direction, NULL);
	UnlockReleaseBuffer(buffer);
	if (direction != expected_direction)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("graph_adj page direction mismatch")));
	return kind;
}

static uint32
graphadj_collect_directory_node(Relation index, BlockNumber block,
								GraphAdjDirection direction, uint32 *page_count,
								uint32 *leaf_page_count, GraphAdjPageKind *kind,
								bool *all_pages_have_lsn, BlockNumber *ancestors,
								unsigned depth, bool has_lower, int64 lower,
								int64 upper)
{
	Buffer		buffer;
	Page		page;
	GraphAdjDirection stored_direction;
	uint16		count;
	uint16		capacity;
	uint16		i;
	uint32		items = 0;

	CHECK_FOR_INTERRUPTS();
	check_stack_depth();
	if (depth >= 32)
		ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
						errmsg("graph_adj directory depth is invalid")));
	for (i = 0; i < depth; i++)
		if (ancestors[i] == block)
			ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
							errmsg("graph_adj directory contains a cycle")));
	ancestors[depth] = block;
	buffer = ReadBuffer(index, block);
	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buffer);
	graphadj_read_opaque_identity(index, page, kind, &stored_direction, NULL);
	if (stored_direction != direction ||
		(*kind != GRAPHADJ_PAGE_DIR_LEAF && *kind != GRAPHADJ_PAGE_DIR_INTERNAL))
		ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
						errmsg("graph_adj directory page identity is corrupt")));
	*all_pages_have_lsn = *all_pages_have_lsn && PageGetLSN(page) != InvalidXLogRecPtr;
	(*page_count)++;
	count = graphadj_get_u16((unsigned char *) PageGetContents(page));
	capacity = *kind == GRAPHADJ_PAGE_DIR_LEAF ?
		graphadj_directory_capacity(page) : graphadj_directory_internal_capacity(page);
	if (graphadj_get_u16((unsigned char *) PageGetContents(page) + 2) != capacity ||
		count > capacity || (count == 0 && (*kind != GRAPHADJ_PAGE_DIR_LEAF || depth != 0)))
		ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
						errmsg("graph_adj directory header is corrupt")));
	if (*kind == GRAPHADJ_PAGE_DIR_LEAF)
	{
		GraphAdjVertexRefData ignored;

		(*leaf_page_count)++;
		if (count > 0)
		{
			int64 first = (int64) graphadj_get_u64(
				graphadj_directory_item(page, 0) + GRAPHADJ_DIR_ITEM_KEY_OFFSET);
			int64 last = (int64) graphadj_get_u64(
				graphadj_directory_item(page, count - 1) + GRAPHADJ_DIR_ITEM_KEY_OFFSET);

			if ((has_lower && first <= lower) || last > upper)
				ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
								errmsg("graph_adj directory child violates routing fence")));
			/* Validate all leaf tuples, including strict ordering. */
			(void) graphadj_directory_lookup(index, page, last, &ignored);
		}
		items = count;
	}
	else
	{
		GraphAdjPageKind previous_kind = GRAPHADJ_PAGE_META;

		for (i = 0; i < count; i++)
		{
			GraphAdjDirectoryChild child = graphadj_directory_get_child(index, page, i);
			GraphAdjPageKind child_kind;

			if ((has_lower && child.high_key <= lower) || child.high_key > upper ||
				(i == count - 1 && child.high_key != upper))
				ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
								errmsg("graph_adj directory internal fence is corrupt")));
			items += graphadj_collect_directory_node(index, child.block, direction,
				page_count, leaf_page_count, &child_kind, all_pages_have_lsn,
				ancestors, depth + 1, has_lower, lower, child.high_key);
			if (i > 0 && child_kind != previous_kind)
				ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
								errmsg("graph_adj directory has inconsistent child levels")));
			previous_kind = child_kind;
			lower = child.high_key;
			has_lower = true;
		}
	}
	UnlockReleaseBuffer(buffer);
	return items;
}

static uint32
graphadj_collect_directory_stats(Relation index, BlockNumber root_block,
								 GraphAdjDirection direction,
								 uint32 *page_count,
								 uint32 *leaf_page_count,
								 GraphAdjPageKind *root_kind,
								 bool *all_pages_have_lsn)
{
	BlockNumber ancestors[32];

	*page_count = *leaf_page_count = 0;
	return graphadj_collect_directory_node(index, root_block, direction, page_count,
		leaf_page_count, root_kind, all_pages_have_lsn, ancestors, 0, false, 0,
		PG_INT64_MAX);
}

typedef struct GraphAdjTidVector
{
	ItemPointerData *items;
	uint64		count;
	uint64		capacity;
} GraphAdjTidVector;

static void
graphadj_tid_vector_append(GraphAdjTidVector *vector, BlockNumber block,
						   OffsetNumber offset)
{
	if (vector->count == vector->capacity)
	{
		vector->capacity = vector->capacity == 0 ? 64 : vector->capacity * 2;
		if (vector->items == NULL)
			vector->items = palloc(sizeof(ItemPointerData) * vector->capacity);
		else
			vector->items = repalloc(vector->items,
								   sizeof(ItemPointerData) * vector->capacity);
	}
	ItemPointerSet(&vector->items[vector->count], block, offset);
	vector->count++;
}

static bool
graphadj_ref_matches_slot(Page slot_page, BlockNumber slot_block, int64 key,
						  const GraphAdjVertexRefData *ref)
{
	const unsigned char *body =
		(const unsigned char *) PageGetContents(slot_page);
	uint16		count = graphadj_get_u16(body);
	uint32		ordinal = GraphAdjSlotIdGetOrdinal(ref->slot_id);
	const unsigned char *slot;

	if (GraphAdjSlotIdGetBlock(ref->slot_id) != slot_block ||
		count > graphadj_slot_capacity(slot_page) ||
		ordinal < GRAPHADJ_FIRST_SLOT_ORDINAL || ordinal > count)
		return false;
	slot = graphadj_slot(slot_page, ordinal);
	return graphadj_get_u32(slot + GRAPHADJ_SLOT_STATE_OFFSET) ==
		GRAPHADJ_SLOT_STATE_LIVE &&
		graphadj_get_u64(slot + GRAPHADJ_SLOT_GENERATION_OFFSET) ==
		ref->slot_generation &&
		graphadj_get_u32(slot + GRAPHADJ_SLOT_DOMAIN_OFFSET) ==
		ref->endpoint_domain &&
		(int64) graphadj_get_u64(slot + GRAPHADJ_SLOT_KEY_OFFSET) == key;
}

/* The caller keeps the owner Slot locked until every segment is copied. */
static void
graphadj_collect_segment(Relation index,
						 const GraphAdjVertexRefData *owner,
						 GraphAdjDirection direction,
						 const GraphAdjDataRefData *data_ref,
						 GraphAdjPageKind expected_kind,
						 bool expected_sealed,
						 GraphAdjStorageStats *stats,
						 GraphAdjTidVector *tids,
						 GraphAdjCandidateVector *candidates)
{
	Buffer		buffer;
	Page		page;
	const unsigned char *segment;
	GraphAdjVertexRefData stored_owner;
	uint64		allocation_generation;
	uint32		published;
	uint32		capacity;
	uint32		i;

	buffer = ReadBuffer(index, data_ref->block);
	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buffer);
	graphadj_read_opaque(index, page, expected_kind, direction,
						 &allocation_generation);
	if (allocation_generation != data_ref->segment_generation)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("graph_adj DataRef generation mismatch")));
	segment = (const unsigned char *) page + data_ref->segment_offset;
	stored_owner = graphadj_get_vertex_ref(
		segment + GRAPHADJ_SEGMENT_OWNER_OFFSET);
	if (!graphadj_vertex_ref_equal(&stored_owner, owner) ||
		graphadj_get_u16(segment + GRAPHADJ_SEGMENT_DIRECTION_OFFSET) !=
		(uint16) direction ||
		(graphadj_get_u16(segment + GRAPHADJ_SEGMENT_SEALED_OFFSET) != 0) !=
		expected_sealed)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("graph_adj segment identity mismatch")));
	published = graphadj_get_u32(segment + GRAPHADJ_SEGMENT_PUBLISHED_OFFSET);
	capacity = graphadj_get_u32(segment + GRAPHADJ_SEGMENT_CAPACITY_OFFSET);
	if (published > capacity ||
		data_ref->segment_offset + GRAPHADJ_SEGMENT_HEADER_SIZE +
		((Size) published * GRAPHADJ_ENTRY_SIZE) >
		((PageHeader) page)->pd_lower)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("graph_adj published entry boundary is corrupt")));
	stats->all_mutable_pages_have_lsn =
		stats->all_mutable_pages_have_lsn &&
		PageGetLSN(page) != InvalidXLogRecPtr;
	stats->referenced_segments++;
	stats->segment_reserved_bytes += GRAPHADJ_SEGMENT_HEADER_SIZE +
		((uint64) capacity * GRAPHADJ_ENTRY_SIZE);
	stats->entry_payload_bytes += (uint64) published * GRAPHADJ_ENTRY_SIZE;
	if (expected_kind == GRAPHADJ_PAGE_PACKED_BASE)
	{
		stats->base_pages++;
		stats->base_entries += published;
	}
	else if (expected_sealed)
	{
		stats->sealed_pages++;
		stats->sealed_entries += published;
	}
	else
	{
		stats->active_pages++;
		stats->active_entries += published;
	}
	for (i = 0; i < published; i++)
	{
		const unsigned char *entry = segment + GRAPHADJ_SEGMENT_HEADER_SIZE +
			((Size) i * GRAPHADJ_ENTRY_SIZE);
		int64		opposite_key = (int64) graphadj_get_u64(
			entry + GRAPHADJ_ENTRY_KEY_OFFSET);
		GraphAdjVertexRefData opposite = graphadj_get_vertex_ref(
			entry + GRAPHADJ_ENTRY_REF_OFFSET);
		BlockNumber tid_block = graphadj_get_u32(
			entry + GRAPHADJ_ENTRY_TID_BLOCK_OFFSET);
		OffsetNumber tid_offset = graphadj_get_u16(
			entry + GRAPHADJ_ENTRY_TID_OFFSET_OFFSET);

		graphadj_candidate_append(candidates, opposite_key, &opposite, tid_block, tid_offset);
		if (tid_block == InvalidBlockNumber || !OffsetNumberIsValid(tid_offset))
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("graph_adj entry contains an invalid heap root TID")));
		graphadj_tid_vector_append(tids, tid_block, tid_offset);
	}
	UnlockReleaseBuffer(buffer);
}

static void
graphadj_collect_data_page_stats(Relation index, GraphAdjStorageStats *stats)
{
	BlockNumber blocks = RelationGetNumberOfBlocks(index);
	BlockNumber block;

	for (block = GRAPHADJ_SLOT_EXTENT_BLKNO + 1; block < blocks; block++)
	{
		Buffer		buffer = ReadBuffer(index, block);
		Page		page;
		const unsigned char *opaque;
		uint16		kind;
		uint32		segments;

		LockBuffer(buffer, BUFFER_LOCK_SHARE);
		page = BufferGetPage(buffer);
		if (PageIsNew(page) ||
			PageGetSpecialSize(page) != MAXALIGN(GRAPHADJ_OPAQUE_SIZE))
		{
			UnlockReleaseBuffer(buffer);
			continue;
		}
		opaque = (const unsigned char *) PageGetSpecialPointer(page);
		if (graphadj_get_u32(opaque + GRAPHADJ_OPAQUE_FORMAT_OFFSET) !=
			GRAPHADJ_FORMAT_VERSION)
		{
			UnlockReleaseBuffer(buffer);
			continue;
		}
		kind = graphadj_get_u16(opaque + GRAPHADJ_OPAQUE_KIND_OFFSET);
		if (graphadj_get_u64(
				opaque + GRAPHADJ_OPAQUE_ALLOCATION_GENERATION_OFFSET) >
			GRAPHADJ_FIRST_ALLOCATION_GENERATION + block)
			stats->reused_pages++;
		if (kind == GRAPHADJ_PAGE_FREE)
		{
			if (graphadj_get_u16(opaque + GRAPHADJ_OPAQUE_DIRECTION_OFFSET) !=
				GRAPHADJ_DIRECTION_NONE)
				ereport(ERROR,
						(errcode(ERRCODE_INDEX_CORRUPTED),
						 errmsg("graph_adj free page has a direction")));
			stats->free_pages++;
			stats->all_mutable_pages_have_lsn =
				stats->all_mutable_pages_have_lsn &&
				PageGetLSN(page) != InvalidXLogRecPtr;
			UnlockReleaseBuffer(buffer);
			continue;
		}
		if (kind != GRAPHADJ_PAGE_DELTA &&
			kind != GRAPHADJ_PAGE_PACKED_BASE)
		{
			UnlockReleaseBuffer(buffer);
			continue;
		}
		segments = graphadj_data_page_segment_count(page);
		{
			Size		offset = graphadj_body_offset();
			uint32		i;

			for (i = 0; i < segments; i++)
			{
				const unsigned char *segment;
				uint32		capacity;
				Size		reserved;

				if (offset + GRAPHADJ_SEGMENT_HEADER_SIZE >
					(Size) ((PageHeader) page)->pd_lower)
					ereport(ERROR,
							(errcode(ERRCODE_INDEX_CORRUPTED),
							 errmsg("graph_adj segment table exceeds page boundary")));
				segment = (const unsigned char *) page + offset;
				capacity = graphadj_get_u32(
					segment + GRAPHADJ_SEGMENT_CAPACITY_OFFSET);
				reserved = GRAPHADJ_SEGMENT_HEADER_SIZE +
					((Size) capacity * GRAPHADJ_ENTRY_SIZE);
				if (capacity == 0 ||
					offset + reserved > (Size) ((PageHeader) page)->pd_lower)
					ereport(ERROR,
							(errcode(ERRCODE_INDEX_CORRUPTED),
							 errmsg("graph_adj segment reservation is corrupt")));
				stats->allocated_segment_reserved_bytes += reserved;
				offset = MAXALIGN(offset + reserved);
			}
		}
		stats->data_pages++;
		stats->allocated_segments += segments;
		if (segments > 1)
			stats->shared_data_pages++;
		stats->data_usable_bytes +=
			((PageHeader) page)->pd_upper - graphadj_body_offset();
		stats->all_mutable_pages_have_lsn =
			stats->all_mutable_pages_have_lsn &&
			PageGetLSN(page) != InvalidXLogRecPtr;
		UnlockReleaseBuffer(buffer);
	}
	if (stats->allocated_segments < stats->referenced_segments)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("graph_adj data page segment accounting is corrupt")));
	stats->unreferenced_segments =
		stats->allocated_segments - stats->referenced_segments;
	if (stats->allocated_segment_reserved_bytes <
		stats->segment_reserved_bytes)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("graph_adj segment byte accounting is corrupt")));
	stats->retired_segment_bytes = stats->allocated_segment_reserved_bytes -
		stats->segment_reserved_bytes;
}

static void
graphadj_collect_storage_stats(Relation index, GraphAdjStorageStats *stats)
{
	Buffer		slot_buffer;
	Page		slot_page;
	const unsigned char *slot_body;
	GraphAdjTidVector out_tids = {0};
	GraphAdjTidVector in_tids = {0};
	BlockNumber slot_block = GRAPHADJ_SLOT_EXTENT_BLKNO;
	uint16 page_slot_count;
	uint32		ordinal;
	Relation	heap;
	TupleTableSlot *tuple_slot;
	IndexFetchTableData *fetch;
	uint64		i;

	MemSet(stats, 0, sizeof(*stats));
	stats->canonical_mapping_valid = true;
	stats->opposite_refs_valid = true;
	stats->all_mutable_pages_have_lsn = true;
	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	stats->out_directory_items = graphadj_collect_directory_stats(index,
		GRAPHADJ_OUT_ROOT_BLKNO, GRAPHADJ_DIRECTION_OUT,
		&stats->out_directory_pages, &stats->out_directory_leaf_pages,
		&stats->out_root_kind, &stats->all_mutable_pages_have_lsn);
	stats->in_directory_items = graphadj_collect_directory_stats(index,
		GRAPHADJ_IN_ROOT_BLKNO, GRAPHADJ_DIRECTION_IN,
		&stats->in_directory_pages, &stats->in_directory_leaf_pages,
		&stats->in_root_kind, &stats->all_mutable_pages_have_lsn);
	while (slot_block != InvalidBlockNumber)
	{
	slot_buffer = ReadBuffer(index, slot_block);
	LockBuffer(slot_buffer, BUFFER_LOCK_SHARE);
	slot_page = BufferGetPage(slot_buffer);
	graphadj_read_opaque(index, slot_page, GRAPHADJ_PAGE_ENDPOINT_SLOT,
						 GRAPHADJ_DIRECTION_NONE, NULL);
	stats->all_mutable_pages_have_lsn =
		stats->all_mutable_pages_have_lsn &&
		PageGetLSN(slot_page) != InvalidXLogRecPtr;
	slot_body = (const unsigned char *) PageGetContents(slot_page);
	page_slot_count = graphadj_get_u16(slot_body);
	stats->slot_count += page_slot_count;

	for (ordinal = GRAPHADJ_FIRST_SLOT_ORDINAL;
		 ordinal < GRAPHADJ_FIRST_SLOT_ORDINAL + page_slot_count;
		 ordinal++)
	{
		const unsigned char *slot = graphadj_slot(slot_page, ordinal);
		int64		key = (int64) graphadj_get_u64(
			slot + GRAPHADJ_SLOT_KEY_OFFSET);
		GraphAdjVertexRefData ref = {
			.slot_id = GraphAdjMakeSlotId(slot_block, ordinal),
			.slot_generation = graphadj_get_u64(
				slot + GRAPHADJ_SLOT_GENERATION_OFFSET),
			.endpoint_domain = graphadj_get_u32(
				slot + GRAPHADJ_SLOT_DOMAIN_OFFSET)
		};
		GraphAdjVertexRefData out_ref;
		GraphAdjVertexRefData in_ref;
		bool		out_found;
		bool		in_found;
		GraphAdjDirection direction;

		out_found = graphadj_directory_lookup_relation(index,
			GRAPHADJ_OUT_ROOT_BLKNO, GRAPHADJ_DIRECTION_OUT, key, &out_ref);
		in_found = graphadj_directory_lookup_relation(index,
			GRAPHADJ_IN_ROOT_BLKNO, GRAPHADJ_DIRECTION_IN, key, &in_ref);
		if (!out_found || !in_found ||
			!graphadj_vertex_ref_equal(&ref, &out_ref) ||
			!graphadj_vertex_ref_equal(&ref, &in_ref))
			stats->canonical_mapping_valid = false;

		for (direction = GRAPHADJ_DIRECTION_OUT;
			 direction <= GRAPHADJ_DIRECTION_IN; direction++)
		{
			const unsigned char *descriptor = graphadj_descriptor(
				(unsigned char *) slot, direction);
			GraphAdjDataRefData base = graphadj_get_data_ref(
				descriptor + GRAPHADJ_DESC_BASE_OFFSET);
			GraphAdjDataRefData sealed = graphadj_get_data_ref(
				descriptor + GRAPHADJ_DESC_SEALED_OFFSET);
			GraphAdjDataRefData active = graphadj_get_data_ref(
				descriptor + GRAPHADJ_DESC_ACTIVE_OFFSET);
			GraphAdjCandidateVector candidates = {0};
			GraphAdjTidVector *tids = direction == GRAPHADJ_DIRECTION_OUT ?
				&out_tids : &in_tids;
			uint32 j;

			/*
			 * Snapshot this direction's inline entries and all three segments
			 * under the same owner-Slot lock.  Compaction takes that lock before
			 * sealing active or publishing a replacement Descriptor.  Releasing
			 * it between segments would leave the remaining DataRefs stale.
			 * Keep the existing Slot -> data-page lock order; opposite-Slot
			 * validation happens only after all data-page locks are released.
			 */
			graphadj_collect_inline_candidates((unsigned char *) slot, direction,
				false, 0, &candidates);
			stats->inline_entries += candidates.count;
			if (candidates.count > 0)
				stats->inline_directions++;
			for (j = 0; j < candidates.count; j++)
				graphadj_tid_vector_append(tids,
					ItemPointerGetBlockNumber(&candidates.items[j].root_tid),
					ItemPointerGetOffsetNumber(&candidates.items[j].root_tid));

			if (!graphadj_data_ref_is_valid(&base) &&
				!graphadj_data_ref_is_valid(&sealed) &&
				!graphadj_data_ref_is_valid(&active))
			{
				if (candidates.count == 0)
					stats->empty_direction_heads++;
			}
			if (graphadj_data_ref_is_valid(&base))
				graphadj_collect_segment(index, &ref, direction,
					&base, GRAPHADJ_PAGE_PACKED_BASE, true, stats,
					tids, &candidates);
			if (graphadj_data_ref_is_valid(&sealed))
				graphadj_collect_segment(index, &ref, direction,
					&sealed, GRAPHADJ_PAGE_DELTA, true, stats,
					tids, &candidates);
			if (graphadj_data_ref_is_valid(&active))
				graphadj_collect_segment(index, &ref, direction,
					&active, GRAPHADJ_PAGE_DELTA, false, stats,
					tids, &candidates);
			if (candidates.count > 0)
			{
				LockBuffer(slot_buffer, BUFFER_LOCK_UNLOCK);
				INJECTION_POINT("graph-adj-info-direction-copied", NULL);
				if (!graphadj_check_candidates(index, &candidates, NULL))
					stats->opposite_refs_valid = false;
				LockBuffer(slot_buffer, BUFFER_LOCK_SHARE);
			}
			if (candidates.items)
				pfree(candidates.items);
		}
	}
	slot_block = graphadj_next_slot_page(index, slot_buffer);
	UnlockReleaseBuffer(slot_buffer);
	}
	graphadj_collect_data_page_stats(index, stats);

	stats->out_physical_entries = out_tids.count;
	stats->in_physical_entries = in_tids.count;
	tuple_slot = table_slot_create(heap, NULL);
	/* Ordinary index fetch has no SO_TYPE_* table-scan flag. */
	fetch = table_index_fetch_begin(heap, 0);
	for (i = 0; i < out_tids.count; i++)
	{
		ItemPointerData tid = out_tids.items[i];
		bool		call_again = false;

		do
		{
			if (table_index_fetch_tuple(fetch, &tid, SnapshotSelf, tuple_slot,
										&call_again, NULL))
				stats->out_visible_entries++;
			ExecClearTuple(tuple_slot);
		} while (call_again);
	}
	for (i = 0; i < in_tids.count; i++)
	{
		ItemPointerData tid = in_tids.items[i];
		bool		call_again = false;

		do
		{
			if (table_index_fetch_tuple(fetch, &tid, SnapshotSelf, tuple_slot,
										&call_again, NULL))
				stats->in_visible_entries++;
			ExecClearTuple(tuple_slot);
		} while (call_again);
	}
	table_index_fetch_end(fetch);
	ExecDropSingleTupleTableSlot(tuple_slot);
	table_close(heap, AccessShareLock);
	if (out_tids.items)
		pfree(out_tids.items);
	if (in_tids.items)
		pfree(in_tids.items);
}

static void
graphadj_fill_vacuum_stats(Relation index, IndexVacuumInfo *info,
						   IndexBulkDeleteResult *stats)
{
	GraphAdjStorageStats storage;

	graphadj_collect_storage_stats(index, &storage);
	if (storage.out_physical_entries != storage.in_physical_entries)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("graph_adj VACUUM found unbalanced OUT and IN entries")));
	stats->num_pages = RelationGetNumberOfBlocks(index);
	stats->estimated_count = info->estimated_count;
	stats->num_index_tuples = (double) storage.out_physical_entries;
	stats->pages_deleted = storage.free_pages;
	stats->pages_free = storage.free_pages;
}

/* Bounded planner metadata read; never enumerate slots/edges for a hint. */
uint64
graphadj_index_build_generation(Oid index_oid)
{
	Relation	index = index_open(index_oid, AccessShareLock);
	GraphAdjMetaData meta;

	if (strcmp(get_am_name(index->rd_rel->relam), "graph_adj") != 0)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("index is not a graph_adj index")));
	graphadj_read_meta(index, &meta, NULL);
	index_close(index, NoLock);
	return meta.build_generation;
}

Datum
graph_adj_index_info(PG_FUNCTION_ARGS)
{
	Oid			index_oid = PG_GETARG_OID(0);
	Relation	index;
	GraphAdjMetaData meta;
	GraphAdjStorageStats storage;
	uint64		allocation_generation;
	uint64		payload_occupancy_basis_points;
	uint64		reserved_occupancy_basis_points;
	uint32		slots_per_page;
	bool		bounds_checked;
	char	   *result;

	index = relation_open(index_oid, AccessShareLock);
	if (index->rd_rel->relkind != RELKIND_INDEX ||
		index->rd_rel->relam != GRAPH_ADJ_AM_OID)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" is not a graph_adj index",
						RelationGetRelationName(index))));
	graphadj_read_meta(index, &meta, &allocation_generation);
	if (graphadj_read_page_kind(index, meta.out_root,
			GRAPHADJ_DIRECTION_OUT) != GRAPHADJ_PAGE_DIR_LEAF &&
		graphadj_read_page_kind(index, meta.out_root,
			GRAPHADJ_DIRECTION_OUT) != GRAPHADJ_PAGE_DIR_INTERNAL)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("graph_adj OUT root has invalid page kind")));
	if (graphadj_read_page_kind(index, meta.in_root,
			GRAPHADJ_DIRECTION_IN) != GRAPHADJ_PAGE_DIR_LEAF &&
		graphadj_read_page_kind(index, meta.in_root,
			GRAPHADJ_DIRECTION_IN) != GRAPHADJ_PAGE_DIR_INTERNAL)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("graph_adj IN root has invalid page kind")));
	graphadj_read_page_identity(index, meta.slot_extent_head,
							GRAPHADJ_PAGE_ENDPOINT_SLOT, GRAPHADJ_DIRECTION_NONE);
	graphadj_collect_storage_stats(index, &storage);
	payload_occupancy_basis_points = storage.data_usable_bytes == 0 ? 0 :
		(storage.entry_payload_bytes * 10000) / storage.data_usable_bytes;
	reserved_occupancy_basis_points = storage.data_usable_bytes == 0 ? 0 :
		(storage.segment_reserved_bytes * 10000) / storage.data_usable_bytes;

	slots_per_page = (BLCKSZ - MAXALIGN(SizeOfPageHeaderData) -
					  MAXALIGN(GRAPHADJ_OPAQUE_SIZE) -
					  GRAPHADJ_SLOT_PAGE_HEADER_SIZE) /
		GRAPHADJ_ENDPOINT_SLOT_SIZE;
	bounds_checked = GraphAdjDataRefBoundsValid(
		GRAPHADJ_SLOT_EXTENT_BLKNO, MAXALIGN(SizeOfPageHeaderData), 1) &&
		!GraphAdjDataRefBoundsValid(InvalidBlockNumber,
									MAXALIGN(SizeOfPageHeaderData), 1) &&
		!GraphAdjDataRefBoundsValid(GRAPHADJ_SLOT_EXTENT_BLKNO,
									BLCKSZ, 1) &&
		!GraphAdjDataRefBoundsValid(GRAPHADJ_SLOT_EXTENT_BLKNO,
									MAXALIGN(SizeOfPageHeaderData), 0);

	result = psprintf(
		"{\"magic\":\"0x%08X\",\"format_version\":%u,"
		"\"build_generation\":%llu,\"allocation_generation\":%llu,"
		"\"out_root\":%u,\"in_root\":%u,\"slot_extent_head\":%u,"
		"\"retirement_head\":%u,\"free_head\":%u,"
		"\"slot_tail\":%u,\"pending_slot_id\":%llu,"
		"\"out_root_kind\":\"%s\",\"out_root_direction\":\"%s\","
		"\"in_root_kind\":\"%s\",\"in_root_direction\":\"%s\","
		"\"slot_page_kind\":\"%s\",\"slot_page_direction\":\"%s\","
		"\"directory_leaf_has_descriptor\":false,"
		"\"directory_leaf_item_bytes\":%u,\"descriptor_bytes\":%u,"
		"\"slot_bytes\":%u,\"slots_per_page\":%u,\"entry_bytes\":%u,"
		"\"segment_header_bytes\":%u,\"vertex_ref_bytes\":%u,"
		"\"data_ref_bytes\":%u,\"invalid_slot_id\":%llu,"
		"\"segment_bounds_checked\":%s,"
		"\"slot_count\":%u,\"out_directory_items\":%u,"
		"\"in_directory_items\":%u,"
		"\"out_directory_pages\":%u,\"in_directory_pages\":%u,"
		"\"out_directory_leaf_pages\":%u,"
		"\"in_directory_leaf_pages\":%u,"
		"\"directory_split_observed\":%s,"
		"\"out_physical_entries\":%llu,"
		"\"in_physical_entries\":%llu,\"out_visible_entries\":%llu,"
		"\"in_visible_entries\":%llu,\"data_pages\":%u,\"free_pages\":%u,"
		"\"reused_pages\":%u,"
		"\"referenced_segments\":%u,\"allocated_segments\":%u,"
		"\"unreferenced_segments\":%u,\"shared_data_pages\":%u,"
		"\"data_usable_bytes\":%llu,"
		"\"allocated_segment_reserved_bytes\":%llu,"
		"\"segment_reserved_bytes\":%llu,\"retired_segment_bytes\":%llu,"
		"\"maintenance_debt_bytes\":%llu,"
		"\"entry_payload_bytes\":%llu,"
		"\"payload_occupancy_basis_points\":%llu,"
		"\"reserved_occupancy_basis_points\":%llu,"
		"\"small_segment_capacity\":%u,\"packed_small_segments\":true,"
		"\"shared_page_reuse_enabled\":false,"
		"\"whole_page_reuse_enabled\":true,\"slot_reuse_enabled\":false,"
		"\"base_pages\":%u,\"sealed_pages\":%u,\"active_pages\":%u,"
		"\"base_entries\":%llu,\"sealed_entries\":%llu,"
		"\"active_entries\":%llu,"
		"\"inline_capacity_per_direction\":%u,\"inline_entries\":%llu,"
		"\"inline_directions\":%u,\"inline_reserved_bytes\":%llu,"
		"\"empty_direction_heads\":%u,"
		"\"canonical_mapping_valid\":%s,\"opposite_refs_valid\":%s,"
		"\"dual_direction_physical_balanced\":%s,"
		"\"all_mutable_pages_have_lsn\":%s}",
		GRAPHADJ_MAGIC, meta.format_version,
		(unsigned long long) meta.build_generation,
		(unsigned long long) allocation_generation,
		meta.out_root, meta.in_root, meta.slot_extent_head,
		meta.retirement_head, meta.free_head,
		meta.slot_tail, (unsigned long long) meta.pending_slot.slot_id,
		graphadj_kind_name(storage.out_root_kind),
		graphadj_direction_name(GRAPHADJ_DIRECTION_OUT),
		graphadj_kind_name(storage.in_root_kind),
		graphadj_direction_name(GRAPHADJ_DIRECTION_IN),
		graphadj_kind_name(GRAPHADJ_PAGE_ENDPOINT_SLOT),
		graphadj_direction_name(GRAPHADJ_DIRECTION_NONE),
		GRAPHADJ_DIRECTORY_LEAF_ITEM_SIZE, GRAPHADJ_DESCRIPTOR_SIZE,
		GRAPHADJ_ENDPOINT_SLOT_SIZE, slots_per_page, GRAPHADJ_ENTRY_SIZE,
		GRAPHADJ_SEGMENT_HEADER_SIZE, GRAPHADJ_VERTEX_REF_SIZE,
		GRAPHADJ_DATA_REF_SIZE,
		(unsigned long long) GRAPHADJ_INVALID_SLOT_ID,
		bounds_checked ? "true" : "false",
		storage.slot_count, storage.out_directory_items,
		storage.in_directory_items,
		storage.out_directory_pages, storage.in_directory_pages,
		storage.out_directory_leaf_pages, storage.in_directory_leaf_pages,
		(storage.out_root_kind == GRAPHADJ_PAGE_DIR_INTERNAL ||
		 storage.in_root_kind == GRAPHADJ_PAGE_DIR_INTERNAL) ? "true" : "false",
		(unsigned long long) storage.out_physical_entries,
		(unsigned long long) storage.in_physical_entries,
		(unsigned long long) storage.out_visible_entries,
		(unsigned long long) storage.in_visible_entries,
		storage.data_pages, storage.free_pages, storage.reused_pages,
		storage.referenced_segments,
		storage.allocated_segments, storage.unreferenced_segments,
		storage.shared_data_pages,
		(unsigned long long) storage.data_usable_bytes,
		(unsigned long long) storage.allocated_segment_reserved_bytes,
		(unsigned long long) storage.segment_reserved_bytes,
		(unsigned long long) storage.retired_segment_bytes,
		(unsigned long long) storage.retired_segment_bytes,
		(unsigned long long) storage.entry_payload_bytes,
		(unsigned long long) payload_occupancy_basis_points,
		(unsigned long long) reserved_occupancy_basis_points,
		GRAPHADJ_SMALL_SEGMENT_ITEMS,
		storage.base_pages, storage.sealed_pages,
		storage.active_pages,
		(unsigned long long) storage.base_entries,
		(unsigned long long) storage.sealed_entries,
		(unsigned long long) storage.active_entries,
		GRAPHADJ_INLINE_ITEMS,
		(unsigned long long) storage.inline_entries,
		storage.inline_directions,
		(unsigned long long) storage.slot_count * 2 * GRAPHADJ_INLINE_ITEMS * GRAPHADJ_ENTRY_SIZE,
		storage.empty_direction_heads,
		storage.canonical_mapping_valid ? "true" : "false",
		storage.opposite_refs_valid ? "true" : "false",
		storage.out_physical_entries == storage.in_physical_entries ?
		"true" : "false",
		storage.all_mutable_pages_have_lsn ? "true" : "false");
	relation_close(index, AccessShareLock);

	PG_RETURN_TEXT_P(cstring_to_text(result));
}

Datum
graph_adj_format_supported(PG_FUNCTION_ARGS)
{
	uint32		format = (uint32) PG_GETARG_INT32(0);

	PG_RETURN_BOOL(format == GRAPHADJ_FORMAT_VERSION);
}

static uint32
graphadj_read_segment_cutoff(Relation index,
							const GraphAdjVertexRefData *owner,
							GraphAdjDirection direction,
							const GraphAdjDataRefData *data_ref,
							GraphAdjPageKind expected_kind,
							bool expected_sealed)
{
	Buffer		buffer;
	Page		page;
	const unsigned char *segment;
	GraphAdjVertexRefData stored_owner;
	uint64		allocation_generation;
	uint32		published;
	uint32		capacity;

	if (!graphadj_data_ref_is_valid(data_ref))
		return 0;
	buffer = ReadBuffer(index, data_ref->block);
	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buffer);
	graphadj_read_opaque(index, page, expected_kind, direction,
						 &allocation_generation);
	segment = (const unsigned char *) page + data_ref->segment_offset;
	stored_owner = graphadj_get_vertex_ref(
		segment + GRAPHADJ_SEGMENT_OWNER_OFFSET);
	published = graphadj_get_u32(segment + GRAPHADJ_SEGMENT_PUBLISHED_OFFSET);
	capacity = graphadj_get_u32(segment + GRAPHADJ_SEGMENT_CAPACITY_OFFSET);
	if (allocation_generation != data_ref->segment_generation ||
		!graphadj_vertex_ref_equal(&stored_owner, owner) ||
		graphadj_get_u16(segment + GRAPHADJ_SEGMENT_DIRECTION_OFFSET) !=
		(uint16) direction ||
		(graphadj_get_u16(segment + GRAPHADJ_SEGMENT_SEALED_OFFSET) != 0) !=
		expected_sealed || published > capacity)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("graph_adj compaction input segment is corrupt")));
	UnlockReleaseBuffer(buffer);
	return published;
}

static bool
graphadj_prepare_compaction(Relation index, int64 key,
							GraphAdjDirection direction,
							bool vacuum_rewrite,
							GraphAdjCompactionResult *result)
{
	Buffer		slot_buffer;
	Page		slot_page;
	unsigned char *slot;
	unsigned char *descriptor;
	GraphAdjDataRefData active;
	uint32		ordinal;

	MemSet(result, 0, sizeof(*result));
	result->old_base = graphadj_invalid_data_ref();
	result->sealed = graphadj_invalid_data_ref();
	result->new_active = graphadj_invalid_data_ref();
	result->new_base = graphadj_invalid_data_ref();
	result->vacuum_rewrite = vacuum_rewrite;
	if (!graphadj_directory_lookup_relation(index,
			direction == GRAPHADJ_DIRECTION_OUT ?
			GRAPHADJ_OUT_ROOT_BLKNO : GRAPHADJ_IN_ROOT_BLKNO,
			direction, key, &result->owner))
		return false;
	ordinal = GraphAdjSlotIdGetOrdinal(result->owner.slot_id);
	slot_buffer = ReadBuffer(index,
		GraphAdjSlotIdGetBlock(result->owner.slot_id));
	LockBuffer(slot_buffer, BUFFER_LOCK_EXCLUSIVE);
	slot_page = BufferGetPage(slot_buffer);
	graphadj_read_opaque(index, slot_page, GRAPHADJ_PAGE_ENDPOINT_SLOT,
						 GRAPHADJ_DIRECTION_NONE, NULL);
	graphadj_validate_slot_identity(index, slot_buffer, key, &result->owner);
	slot = graphadj_slot(slot_page, ordinal);
	descriptor = graphadj_descriptor(slot, direction);
	result->old_base = graphadj_get_data_ref(
		descriptor + GRAPHADJ_DESC_BASE_OFFSET);
	result->sealed = graphadj_get_data_ref(
		descriptor + GRAPHADJ_DESC_SEALED_OFFSET);
	active = graphadj_get_data_ref(descriptor + GRAPHADJ_DESC_ACTIVE_OFFSET);
	result->view_generation_before = graphadj_get_u64(
		descriptor + GRAPHADJ_DESC_VIEW_GENERATION_OFFSET);

	if (graphadj_data_ref_is_valid(&result->sealed))
	{
		if (!graphadj_data_ref_is_valid(&active))
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("graph_adj sealed input has no current active segment")));
		result->new_active = active;
		result->view_generation_after_seal = result->view_generation_before;
		result->sealed_cutoff = graphadj_read_segment_cutoff(index,
			&result->owner, direction, &result->sealed,
			GRAPHADJ_PAGE_DELTA, true);
		result->resumed_sealed = true;
		UnlockReleaseBuffer(slot_buffer);
		return true;
	}
	if (!graphadj_data_ref_is_valid(&active))
	{
		/* VACUUM may need to refilter a Base even when there is no Delta. */
		if (vacuum_rewrite && graphadj_data_ref_is_valid(&result->old_base))
		{
			result->view_generation_after_seal =
				result->view_generation_before;
			UnlockReleaseBuffer(slot_buffer);
			return true;
		}
		UnlockReleaseBuffer(slot_buffer);
		return false;
	}
	else
	{
		Buffer		old_active_buffer;
		Buffer		new_active_buffer;
		Page		old_active_page;
		Page		new_active_page;
		const unsigned char *old_segment;
		GraphAdjVertexRefData stored_owner;
		uint64		allocation_generation;
		bool		new_page;
		GenericXLogState *state;

		old_active_buffer = ReadBuffer(index, active.block);
		LockBuffer(old_active_buffer, BUFFER_LOCK_EXCLUSIVE);
		old_active_page = BufferGetPage(old_active_buffer);
		graphadj_read_opaque(index, old_active_page, GRAPHADJ_PAGE_DELTA,
							 direction, &allocation_generation);
		old_segment = (const unsigned char *) old_active_page +
			active.segment_offset;
		stored_owner = graphadj_get_vertex_ref(
			old_segment + GRAPHADJ_SEGMENT_OWNER_OFFSET);
		if (allocation_generation != active.segment_generation ||
			!graphadj_vertex_ref_equal(&stored_owner, &result->owner) ||
			graphadj_get_u16(old_segment +
				GRAPHADJ_SEGMENT_DIRECTION_OFFSET) != (uint16) direction ||
			graphadj_get_u16(old_segment + GRAPHADJ_SEGMENT_SEALED_OFFSET) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("graph_adj active segment cannot be sealed")));
		result->sealed_cutoff = graphadj_get_u32(
			old_segment + GRAPHADJ_SEGMENT_PUBLISHED_OFFSET);

		result->sealed = active;
		new_active_buffer = graphadj_allocate_segment_buffer(index,
			GRAPHADJ_PAGE_DELTA, direction, GRAPHADJ_SMALL_SEGMENT_ITEMS,
			active.block, GraphAdjSlotIdGetBlock(result->owner.slot_id), &result->new_active, &new_page);

		state = GenericXLogStart(index);
		slot_page = GenericXLogRegisterBuffer(state, slot_buffer, 0);
		old_active_page = GenericXLogRegisterBuffer(state, old_active_buffer, 0);
		new_active_page = GenericXLogRegisterBuffer(state, new_active_buffer,
			new_page ? GENERIC_XLOG_FULL_IMAGE : 0);
		slot = graphadj_slot(slot_page, ordinal);
		descriptor = graphadj_descriptor(slot, direction);
		old_segment = (const unsigned char *) old_active_page +
			active.segment_offset;
		graphadj_put_u16((unsigned char *) old_segment +
			GRAPHADJ_SEGMENT_SEALED_OFFSET, 1);
		graphadj_initialize_segment(new_active_page, new_page,
			GRAPHADJ_PAGE_DELTA, direction, &result->new_active,
			&result->owner, GRAPHADJ_SMALL_SEGMENT_ITEMS, false);
		graphadj_put_data_ref(descriptor + GRAPHADJ_DESC_SEALED_OFFSET,
			&result->sealed);
		graphadj_put_data_ref(descriptor + GRAPHADJ_DESC_ACTIVE_OFFSET,
			&result->new_active);
		result->view_generation_after_seal =
			result->view_generation_before + 1;
		graphadj_put_u64(descriptor + GRAPHADJ_DESC_VIEW_GENERATION_OFFSET,
			result->view_generation_after_seal);
		GenericXLogFinish(state);
		graphadj_maybe_crash(index, key, "sealed_published");

		UnlockReleaseBuffer(new_active_buffer);
		UnlockReleaseBuffer(old_active_buffer);
		UnlockReleaseBuffer(slot_buffer);
		return true;
	}
}

static int
graphadj_candidate_compare(const void *left_pointer, const void *right_pointer)
{
	const GraphAdjCandidateData *left = left_pointer;
	const GraphAdjCandidateData *right = right_pointer;
	BlockNumber left_block;
	BlockNumber right_block;
	OffsetNumber left_offset;
	OffsetNumber right_offset;

	if (left->opposite_key < right->opposite_key)
		return -1;
	if (left->opposite_key > right->opposite_key)
		return 1;
	if (left->opposite_ref.slot_id < right->opposite_ref.slot_id)
		return -1;
	if (left->opposite_ref.slot_id > right->opposite_ref.slot_id)
		return 1;
	left_block = ItemPointerGetBlockNumber(&left->root_tid);
	right_block = ItemPointerGetBlockNumber(&right->root_tid);
	if (left_block < right_block)
		return -1;
	if (left_block > right_block)
		return 1;
	left_offset = ItemPointerGetOffsetNumber(&left->root_tid);
	right_offset = ItemPointerGetOffsetNumber(&right->root_tid);
	return left_offset < right_offset ? -1 : left_offset > right_offset ? 1 : 0;
}

static bool
graphadj_candidate_equal(const GraphAdjCandidateData *left,
						 const GraphAdjCandidateData *right)
{
	return ItemPointerEquals(&left->root_tid, &right->root_tid) &&
		left->opposite_key == right->opposite_key &&
		graphadj_vertex_ref_equal(&left->opposite_ref, &right->opposite_ref);
}

static void
graphadj_build_compacted_base(Relation index, int64 key,
							  GraphAdjDirection direction,
							  IndexBulkDeleteCallback callback,
							  void *callback_state,
							  GraphAdjCompactionResult *result)
{
	Buffer		slot_buffer;
	Page		slot_page;
	GraphAdjCandidateVector candidates = {0};
	Buffer		base_buffer;
	Page		base_page;
	GenericXLogState *state;
	unsigned char *segment;
	uint32		capacity;
	uint32		reserved_capacity;
	uint32		read_index;
	uint32		write_index;
	bool		new_page;
	bool		have_previous = false;
	GraphAdjCandidateData previous;

	slot_buffer = ReadBuffer(index,
		GraphAdjSlotIdGetBlock(result->owner.slot_id));
	LockBuffer(slot_buffer, BUFFER_LOCK_SHARE);
	slot_page = BufferGetPage(slot_buffer);
	graphadj_read_opaque(index, slot_page, GRAPHADJ_PAGE_ENDPOINT_SLOT,
		GRAPHADJ_DIRECTION_NONE, NULL);
	graphadj_validate_slot_identity(index, slot_buffer, key, &result->owner);
	graphadj_collect_data_candidates(index, slot_page, &result->owner,
		direction, &result->old_base, GRAPHADJ_PAGE_PACKED_BASE, true,
		false, 0, &candidates);
	graphadj_collect_data_candidates(index, slot_page, &result->owner,
		direction, &result->sealed, GRAPHADJ_PAGE_DELTA, true,
		false, 0, &candidates);
	UnlockReleaseBuffer(slot_buffer);
	if (!graphadj_check_candidates(index, &candidates, NULL))
		ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
						errmsg("graph_adj compaction found a conflicting persistent VertexRef")));
	result->base_input_items = candidates.count;

	if (candidates.count > 1)
		qsort(candidates.items, candidates.count, sizeof(candidates.items[0]),
			  graphadj_candidate_compare);
	write_index = 0;
	for (read_index = 0; read_index < candidates.count; read_index++)
	{
		GraphAdjCandidateData current = candidates.items[read_index];

		if (have_previous && graphadj_candidate_equal(&previous, &current))
			continue;
		previous = current;
		have_previous = true;
		if (callback != NULL && callback(&current.root_tid, callback_state))
		{
			result->removed_items++;
			continue;
		}
		if (write_index != read_index)
			candidates.items[write_index] = current;
		write_index++;
	}
	result->base_output_items = write_index;
	if (write_index == 0)
	{
		if (candidates.items)
			pfree(candidates.items);
		return;
	}

	reserved_capacity = Max((uint32) GRAPHADJ_SMALL_SEGMENT_ITEMS, write_index);
	base_buffer = graphadj_allocate_segment_buffer(index,
		GRAPHADJ_PAGE_PACKED_BASE, direction, reserved_capacity,
		InvalidBlockNumber, InvalidBlockNumber, &result->new_base, &new_page);
	state = GenericXLogStart(index);
	base_page = GenericXLogRegisterBuffer(state, base_buffer,
		new_page ? GENERIC_XLOG_FULL_IMAGE : 0);
	graphadj_initialize_segment(base_page, new_page,
		GRAPHADJ_PAGE_PACKED_BASE, direction, &result->new_base,
		&result->owner, reserved_capacity, true);
	segment = (unsigned char *) base_page + result->new_base.segment_offset;
	graphadj_put_u64(segment + GRAPHADJ_SEGMENT_VIEW_GENERATION_OFFSET,
		result->view_generation_after_seal + 1);
	capacity = graphadj_get_u32(segment + GRAPHADJ_SEGMENT_CAPACITY_OFFSET);
	if (write_index > capacity)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("graph_adj compacted base exceeds one prototype segment"),
				 errdetail("Base requires %u entries but capacity is %u.",
						   write_index, capacity),
				 errhint("Multi-segment Base chaining is not implemented yet.")));
	for (read_index = 0; read_index < write_index; read_index++)
	{
		GraphAdjCandidateData *candidate = &candidates.items[read_index];
		unsigned char *entry = segment + GRAPHADJ_SEGMENT_HEADER_SIZE +
			((Size) read_index * GRAPHADJ_ENTRY_SIZE);

		MemSet(entry, 0, GRAPHADJ_ENTRY_SIZE);
		graphadj_put_u64(entry + GRAPHADJ_ENTRY_KEY_OFFSET,
			(uint64) candidate->opposite_key);
		graphadj_put_vertex_ref(entry + GRAPHADJ_ENTRY_REF_OFFSET,
			&candidate->opposite_ref);
		graphadj_put_u32(entry + GRAPHADJ_ENTRY_TID_BLOCK_OFFSET,
			ItemPointerGetBlockNumber(&candidate->root_tid));
		graphadj_put_u16(entry + GRAPHADJ_ENTRY_TID_OFFSET_OFFSET,
			ItemPointerGetOffsetNumber(&candidate->root_tid));
		graphadj_put_u16(entry + GRAPHADJ_ENTRY_FLAGS_OFFSET, 0);
	}
	graphadj_put_u32(segment + GRAPHADJ_SEGMENT_PUBLISHED_OFFSET, write_index);
	GenericXLogFinish(state);
	graphadj_maybe_crash(index, key, "base_built");
	UnlockReleaseBuffer(base_buffer);
	if (candidates.items)
		pfree(candidates.items);
}

static void
graphadj_publish_compacted_base(Relation index, int64 key,
								GraphAdjDirection direction,
								GraphAdjCompactionResult *result)
{
	Buffer		slot_buffer;
	Page		slot_page;
	unsigned char *slot;
	unsigned char *descriptor;
	GraphAdjDataRefData current_base;
	GraphAdjDataRefData current_sealed;
	GraphAdjDataRefData current_active;
	GenericXLogState *state;
	uint32		ordinal = GraphAdjSlotIdGetOrdinal(result->owner.slot_id);

	slot_buffer = ReadBuffer(index,
		GraphAdjSlotIdGetBlock(result->owner.slot_id));
	LockBuffer(slot_buffer, BUFFER_LOCK_EXCLUSIVE);
	slot_page = BufferGetPage(slot_buffer);
	graphadj_read_opaque(index, slot_page, GRAPHADJ_PAGE_ENDPOINT_SLOT,
		GRAPHADJ_DIRECTION_NONE, NULL);
	graphadj_validate_slot_identity(index, slot_buffer, key, &result->owner);
	slot = graphadj_slot(slot_page, ordinal);
	descriptor = graphadj_descriptor(slot, direction);
	current_base = graphadj_get_data_ref(descriptor + GRAPHADJ_DESC_BASE_OFFSET);
	current_sealed = graphadj_get_data_ref(
		descriptor + GRAPHADJ_DESC_SEALED_OFFSET);
	current_active = graphadj_get_data_ref(
		descriptor + GRAPHADJ_DESC_ACTIVE_OFFSET);
	if (!graphadj_data_ref_equal(&current_base, &result->old_base) ||
		!graphadj_data_ref_equal(&current_sealed, &result->sealed) ||
		!graphadj_data_ref_equal(&current_active, &result->new_active))
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("graph_adj compaction inputs changed before publication")));
	result->active_items_at_publish = graphadj_read_segment_cutoff(index,
		&result->owner, direction, &current_active, GRAPHADJ_PAGE_DELTA, false);

	state = GenericXLogStart(index);
	slot_page = GenericXLogRegisterBuffer(state, slot_buffer, 0);
	slot = graphadj_slot(slot_page, ordinal);
	descriptor = graphadj_descriptor(slot, direction);
	graphadj_put_data_ref(descriptor + GRAPHADJ_DESC_BASE_OFFSET,
		&result->new_base);
	graphadj_put_invalid_data_ref(descriptor + GRAPHADJ_DESC_SEALED_OFFSET);
	if (result->vacuum_rewrite && result->active_items_at_publish == 0 &&
		graphadj_data_ref_is_valid(&current_active))
	{
		/* A later writer will install a fresh active segment. */
		graphadj_put_invalid_data_ref(descriptor + GRAPHADJ_DESC_ACTIVE_OFFSET);
		graphadj_put_u32(descriptor + GRAPHADJ_DESC_LANE_COUNT_OFFSET, 0);
		result->active_retired = true;
	}
	result->view_generation_after_publish = graphadj_get_u64(
		descriptor + GRAPHADJ_DESC_VIEW_GENERATION_OFFSET) + 1;
	graphadj_put_u64(descriptor + GRAPHADJ_DESC_VIEW_GENERATION_OFFSET,
		result->view_generation_after_publish);
	GenericXLogFinish(state);
	graphadj_maybe_crash(index, key, "base_published");
	UnlockReleaseBuffer(slot_buffer);
}

static void
graphadj_debug_require_index(Relation index)
{
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("graph_adj diagnostic functions require superuser")));
	if (index->rd_rel->relkind != RELKIND_INDEX ||
		index->rd_rel->relam != GRAPH_ADJ_AM_OID)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("relation \"%s\" is not a graph_adj index",
						RelationGetRelationName(index))));
}

static bool
graphadj_compact_endpoint(Relation index, int64 key,
						  GraphAdjDirection direction, int32 pause_ms,
						  IndexBulkDeleteCallback callback,
						  void *callback_state,
						  GraphAdjCompactionResult *result)
{
	GraphAdjVertexRefData owner;
	LOCKTAG		locktag;
	bool		prepared = false;

	MemSet(result, 0, sizeof(*result));
	if (!graphadj_directory_lookup_relation(index,
			direction == GRAPHADJ_DIRECTION_OUT ?
			GRAPHADJ_OUT_ROOT_BLKNO : GRAPHADJ_IN_ROOT_BLKNO,
			direction, key, &owner))
		return false;
	SET_LOCKTAG_ADVISORY(locktag, MyDatabaseId, RelationGetRelid(index),
		(uint32) (owner.slot_id ^ (owner.slot_id >> 32)), (uint32) direction);
	(void) LockAcquire(&locktag, ExclusiveLock, false, false);
	PG_TRY();
	{
		prepared = graphadj_prepare_compaction(index, key, direction,
			callback != NULL, result);
		if (prepared)
		{
			CHECK_FOR_INTERRUPTS();
			if (pause_ms > 0)
				pg_usleep((long) pause_ms * 1000L);
			CHECK_FOR_INTERRUPTS();
			graphadj_build_compacted_base(index, key, direction, callback,
				callback_state, result);
			graphadj_publish_compacted_base(index, key, direction, result);
		}
	}
	PG_FINALLY();
	{
		(void) LockRelease(&locktag, ExclusiveLock, false);
	}
	PG_END_TRY();
	return prepared;
}

Datum
graph_adj_debug_compact(PG_FUNCTION_ARGS)
{
	Oid			index_oid = PG_GETARG_OID(0);
	int64		key = PG_GETARG_INT64(1);
	GraphAdjDirection direction = PG_GETARG_BOOL(2) ?
		GRAPHADJ_DIRECTION_OUT : GRAPHADJ_DIRECTION_IN;
	int32		pause_ms = PG_GETARG_INT32(3);
	Relation	index;
	GraphAdjCompactionResult result;
	bool		found;
	bool		prepared;
	char	   *json;

	if (pause_ms < 0 || pause_ms > 10000)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("graph_adj compaction pause must be between 0 and 10000 ms")));
	index = relation_open(index_oid, AccessShareLock);
	graphadj_debug_require_index(index);
	prepared = graphadj_compact_endpoint(index, key, direction, pause_ms,
		NULL, NULL, &result);
	found = result.owner.slot_id != GRAPHADJ_INVALID_SLOT_ID;

	if (!found)
		json = pstrdup("{\"found\":false}");
	else if (!prepared)
		json = psprintf(
			"{\"found\":true,\"compacted\":false,\"endpoint_key\":%lld}",
			(long long) key);
	else
		json = psprintf(
			"{\"found\":true,\"compacted\":true,"
			"\"direction\":\"%s\",\"endpoint_key\":%lld,"
			"\"slot_id\":%llu,\"slot_generation\":%llu,"
			"\"sealed_block\":%u,\"sealed_cutoff\":%u,"
			"\"new_active_block\":%u,\"new_base_block\":%u,"
			"\"base_input_items\":%u,\"base_output_items\":%u,"
			"\"active_items_at_publish\":%u,\"resumed_sealed\":%s,"
			"\"view_generation_before\":%llu,"
			"\"view_generation_after_seal\":%llu,"
			"\"view_generation_after_publish\":%llu,"
			"\"descriptor_in_stable_slot\":true,"
			"\"new_active_preserved\":true}",
			graphadj_direction_name(direction), (long long) key,
			(unsigned long long) result.owner.slot_id,
			(unsigned long long) result.owner.slot_generation,
			result.sealed.block, result.sealed_cutoff,
			result.new_active.block, result.new_base.block,
			result.base_input_items, result.base_output_items,
			result.active_items_at_publish,
			result.resumed_sealed ? "true" : "false",
			(unsigned long long) result.view_generation_before,
			(unsigned long long) result.view_generation_after_seal,
			(unsigned long long) result.view_generation_after_publish);
	relation_close(index, AccessShareLock);
	PG_RETURN_TEXT_P(cstring_to_text(json));
}

static bool
graphadj_hint_ref_matches(Relation index, int64 key,
						 const GraphAdjVertexRefData *ref)
{
	BlockNumber block;
	Buffer		buffer;
	Page		page;
	const unsigned char *opaque;
	bool		matches = false;

	if (!GraphAdjSlotIdIsValid(ref->slot_id))
		return false;
	block = GraphAdjSlotIdGetBlock(ref->slot_id);
	if (block >= RelationGetNumberOfBlocks(index))
		return false;
	buffer = ReadBuffer(index, block);
	LockBuffer(buffer, BUFFER_LOCK_SHARE);
	page = BufferGetPage(buffer);
	if (!PageIsNew(page) &&
		PageGetSpecialSize(page) == MAXALIGN(GRAPHADJ_OPAQUE_SIZE))
	{
		opaque = (const unsigned char *) PageGetSpecialPointer(page);
		if (graphadj_get_u32(opaque + GRAPHADJ_OPAQUE_FORMAT_OFFSET) ==
			GRAPHADJ_FORMAT_VERSION &&
			graphadj_get_u16(opaque + GRAPHADJ_OPAQUE_KIND_OFFSET) ==
			GRAPHADJ_PAGE_ENDPOINT_SLOT &&
			graphadj_get_u16(opaque + GRAPHADJ_OPAQUE_DIRECTION_OFFSET) ==
			GRAPHADJ_DIRECTION_NONE)
			matches = graphadj_ref_matches_slot(page, block, key, ref);
	}
	UnlockReleaseBuffer(buffer);
	return matches;
}

static bool
graphadj_resolve_debug_handle(Relation index, GraphAdjDirection direction,
							  int64 key,
							  const GraphAdjScopedVertexRef *handle,
							  GraphAdjVertexRefData *result,
							  GraphAdjAccessCounters *counters,
							  const GraphAdjMetaData *meta)
{
	Oid			index_oid = RelationGetRelid(index);
	Oid			domain = index->rd_index->indrelid;
	bool		scope_matches;

	scope_matches = handle != NULL && handle->index_relid == index_oid &&
		handle->build_generation == meta->build_generation &&
		handle->vertex_ref.endpoint_domain == domain;
	if (scope_matches &&
		graphadj_hint_ref_matches(index, key, &handle->vertex_ref))
	{
		*result = handle->vertex_ref;
		counters->ref_direct_lookups++;
		return false;
	}
	if (scope_matches && handle->authoritative)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("graph_adj found a conflicting persistent VertexRef"),
				 errdetail("Index \"%s\" key %lld has an invalid Slot identity or generation.",
						   RelationGetRelationName(index), (long long) key)));
	if (handle != NULL)
		counters->ref_fallbacks++;
	counters->directory_lookups++;
	if (!graphadj_directory_lookup_relation(index,
			direction == GRAPHADJ_DIRECTION_OUT ?
			meta->out_root : meta->in_root,
			direction, key, result))
		return true;
	return false;
}

Datum
graph_adj_debug_ref_probe(PG_FUNCTION_ARGS)
{
	Oid			index_oid = PG_GETARG_OID(0);
	int64		key = PG_GETARG_INT64(1);
	GraphAdjScopedVertexRef handle = {
		.index_relid = PG_GETARG_OID(2),
		.build_generation = (uint64) PG_GETARG_INT64(3),
		.endpoint_key = key,
		.vertex_ref = {
			.slot_id = (uint64) PG_GETARG_INT64(5),
			.slot_generation = (uint64) PG_GETARG_INT64(6),
			.endpoint_domain = PG_GETARG_OID(4)
		},
		.authoritative = PG_GETARG_BOOL(7)
	};
	Relation	index;
	GraphAdjMetaData meta;
	GraphAdjVertexRefData resolved = {0};
	GraphAdjAccessCounters counters = {0};
	bool		not_found;
	char	   *result;

	index = relation_open(index_oid, AccessShareLock);
	graphadj_debug_require_index(index);
	graphadj_read_meta(index, &meta, NULL);
	not_found = graphadj_resolve_debug_handle(index,
		GRAPHADJ_DIRECTION_OUT, key, &handle, &resolved, &counters, &meta);
	if (!not_found)
		counters.slot_dereferences++;
	result = psprintf(
		"{\"found\":%s,\"mode\":\"%s\",\"index_oid\":%u,"
		"\"build_generation\":%llu,\"endpoint_key\":%lld,"
		"\"endpoint_domain\":%u,\"slot_id\":%llu,"
		"\"slot_generation\":%llu,\"directory_lookups\":%llu,"
		"\"ref_direct_lookups\":%llu,\"ref_fallbacks\":%llu,"
		"\"slot_dereferences\":%llu}",
		not_found ? "false" : "true",
		counters.ref_direct_lookups > 0 ? "ref_direct" : "key_fallback",
		index_oid, (unsigned long long) meta.build_generation,
		(long long) key, resolved.endpoint_domain,
		(unsigned long long) resolved.slot_id,
		(unsigned long long) resolved.slot_generation,
		(unsigned long long) counters.directory_lookups,
		(unsigned long long) counters.ref_direct_lookups,
		(unsigned long long) counters.ref_fallbacks,
		(unsigned long long) counters.slot_dereferences);
	relation_close(index, AccessShareLock);
	PG_RETURN_TEXT_P(cstring_to_text(result));
}

static void
graphadj_walk_append(GraphAdjWalkVector *vector, int64 key,
					 const GraphAdjVertexRefData *ref)
{
	GraphAdjWalkBinding *binding;

	if (vector->count == vector->capacity)
	{
		vector->capacity = vector->capacity == 0 ? 16 : vector->capacity * 2;
		if (vector->items == NULL)
			vector->items = palloc(sizeof(GraphAdjWalkBinding) * vector->capacity);
		else
			vector->items = repalloc(vector->items,
				sizeof(GraphAdjWalkBinding) * vector->capacity);
	}
	binding = &vector->items[vector->count++];
	binding->key = key;
	if (ref)
	{
		binding->ref = *ref;
		binding->has_ref = true;
	}
	else
	{
		MemSet(&binding->ref, 0, sizeof(binding->ref));
		binding->has_ref = false;
	}
}

static bool
graphadj_walk_binding_equal(const GraphAdjWalkBinding *left,
							const GraphAdjWalkBinding *right)
{
	if (left->key != right->key || left->has_ref != right->has_ref)
		return false;
	return !left->has_ref || graphadj_vertex_ref_equal(&left->ref, &right->ref);
}

Datum
graph_adj_debug_walk(PG_FUNCTION_ARGS)
{
	Oid			index_oid = PG_GETARG_OID(0);
	int64		start_key = PG_GETARG_INT64(1);
	int32		hops = PG_GETARG_INT32(2);
	Relation	index;
	Relation	heap;
	GraphAdjMetaData meta;
	GraphAdjAccessCounters counters = {0};
	GraphAdjWalkVector frontier = {0};
	IndexFetchTableData *fetch;
	TupleTableSlot *tuple_slot;
	Snapshot	snapshot;
	int32		hop;
	StringInfoData json;
	uint32		i;

	if (hops < 1 || hops > 8)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("graph_adj diagnostic walk requires 1 to 8 hops")));
	index = relation_open(index_oid, AccessShareLock);
	graphadj_debug_require_index(index);
	graphadj_read_meta(index, &meta, NULL);
	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	snapshot = GetActiveSnapshot();
	if (snapshot == NULL)
		elog(ERROR, "graph_adj diagnostic walk requires an active snapshot");
	/*
	 * This diagnostic walk models the direct-ref/batch provider path and does
	 * not enter through index_beginscan_internal().  Keep it under the same
	 * conservative SSI protocol as ordinary scans while ampredlocks=false.
	 * PredicateLockRelation is a nonblocking SIREAD and is a no-op outside a
	 * full SERIALIZABLE snapshot.
	 */
	PredicateLockRelation(index, snapshot);
	fetch = table_index_fetch_begin(heap, 0);
	tuple_slot = table_slot_create(heap, NULL);
	graphadj_walk_append(&frontier, start_key, NULL);

	for (hop = 0; hop < hops && frontier.count > 0; hop++)
	{
		GraphAdjWalkVector next = {0};

		for (i = 0; i < frontier.count; i++)
		{
			GraphAdjWalkBinding *owner = &frontier.items[i];
			GraphAdjVertexRefData owner_ref;
			GraphAdjScopedVertexRef handle;
			GraphAdjCandidateVector candidates = {0};
			Buffer		slot_buffer;
			Page		slot_page;
			uint32		parents = 1;
			uint32		j;

			for (j = 0; j < i; j++)
				if (graphadj_walk_binding_equal(owner, &frontier.items[j]))
					break;
			if (j < i)
				continue;
			for (j = i + 1; j < frontier.count; j++)
				if (graphadj_walk_binding_equal(owner, &frontier.items[j]))
					parents++;

			MemSet(&handle, 0, sizeof(handle));
			if (owner->has_ref)
			{
				handle.index_relid = index_oid;
				handle.build_generation = meta.build_generation;
				handle.endpoint_key = owner->key;
				handle.vertex_ref = owner->ref;
				handle.authoritative = true;
			}
			if (graphadj_resolve_debug_handle(index,
					GRAPHADJ_DIRECTION_OUT, owner->key,
					owner->has_ref ? &handle : NULL, &owner_ref,
					&counters, &meta))
				continue;

			slot_buffer = ReadBuffer(index,
				GraphAdjSlotIdGetBlock(owner_ref.slot_id));
			LockBuffer(slot_buffer, BUFFER_LOCK_SHARE);
			slot_page = BufferGetPage(slot_buffer);
			graphadj_read_opaque(index, slot_page,
				GRAPHADJ_PAGE_ENDPOINT_SLOT, GRAPHADJ_DIRECTION_NONE, NULL);
			graphadj_validate_slot_identity(index, slot_buffer, owner->key,
				&owner_ref);
			counters.slot_dereferences++;
			graphadj_collect_candidates(index, slot_buffer, &owner_ref,
				GRAPHADJ_DIRECTION_OUT, false, 0, &candidates, NULL);
			UnlockReleaseBuffer(slot_buffer);

			for (j = 0; j < candidates.count; j++)
			{
				GraphAdjCandidateData *candidate = &candidates.items[j];
				bool		call_again = false;
				bool		visible;

				counters.physical_candidates++;
				do
				{
					uint32		parent;

					counters.tableam_calls++;
					visible = table_index_fetch_tuple(fetch,
						&candidate->root_tid, snapshot, tuple_slot,
						&call_again, NULL);
					if (visible)
					{
						counters.visible_candidates++;
						for (parent = 0; parent < parents; parent++)
							graphadj_walk_append(&next,
								candidate->opposite_key,
								&candidate->opposite_ref);
						counters.output_bindings += parents;
					}
					ExecClearTuple(tuple_slot);
				} while (call_again);
			}
			if (candidates.items)
				pfree(candidates.items);
		}
		pfree(frontier.items);
		frontier = next;
	}

	initStringInfo(&json);
	appendStringInfo(&json,
		"{\"index_oid\":%u,\"build_generation\":%llu,"
		"\"start_key\":%lld,\"hops\":%d,\"final_keys\":[",
		index_oid, (unsigned long long) meta.build_generation,
		(long long) start_key, hops);
	for (i = 0; i < frontier.count; i++)
	{
		if (i > 0)
			appendStringInfoChar(&json, ',');
		appendStringInfo(&json, "%lld", (long long) frontier.items[i].key);
	}
	appendStringInfo(&json,
		"],\"final_binding_count\":%u,\"directory_lookups\":%llu,"
		"\"ref_direct_lookups\":%llu,\"ref_fallbacks\":%llu,"
		"\"slot_dereferences\":%llu,\"physical_candidates\":%llu,"
		"\"tableam_calls\":%llu,\"visible_candidates\":%llu,"
		"\"output_bindings\":%llu}",
		frontier.count,
		(unsigned long long) counters.directory_lookups,
		(unsigned long long) counters.ref_direct_lookups,
		(unsigned long long) counters.ref_fallbacks,
		(unsigned long long) counters.slot_dereferences,
		(unsigned long long) counters.physical_candidates,
		(unsigned long long) counters.tableam_calls,
		(unsigned long long) counters.visible_candidates,
		(unsigned long long) counters.output_bindings);

	if (frontier.items)
		pfree(frontier.items);
	table_index_fetch_end(fetch);
	ExecDropSingleTupleTableSlot(tuple_slot);
	table_close(heap, AccessShareLock);
	relation_close(index, AccessShareLock);
	PG_RETURN_TEXT_P(cstring_to_text(json.data));
}

static bool
graphadj_corrupt_first_out_ref(Relation index, int64 key)
{
	GraphAdjVertexRefData owner;
	GraphAdjDataRefData data_ref;
	GraphAdjPageKind expected_kind;
	GraphAdjDirection direction = GRAPHADJ_DIRECTION_OUT;
	Buffer		slot_buffer;
	Buffer		data_buffer;
	Page		slot_page;
	Page		data_page;
	const unsigned char *slot;
	const unsigned char *descriptor;
	unsigned char *segment;
	unsigned char *entry;
	uint64		allocation_generation;
	uint64		generation;
	uint32		published;
	uint32		ordinal;
	GenericXLogState *state;

	if (!graphadj_directory_lookup_relation(index, GRAPHADJ_OUT_ROOT_BLKNO,
			GRAPHADJ_DIRECTION_OUT, key, &owner))
		return false;
	ordinal = GraphAdjSlotIdGetOrdinal(owner.slot_id);
	slot_buffer = ReadBuffer(index, GraphAdjSlotIdGetBlock(owner.slot_id));
	LockBuffer(slot_buffer, BUFFER_LOCK_EXCLUSIVE);
	slot_page = BufferGetPage(slot_buffer);
	graphadj_read_opaque(index, slot_page, GRAPHADJ_PAGE_ENDPOINT_SLOT,
		GRAPHADJ_DIRECTION_NONE, NULL);
	graphadj_validate_slot_identity(index, slot_buffer, key, &owner);
	slot = graphadj_slot(slot_page, ordinal);
	descriptor = graphadj_descriptor((unsigned char *) slot, direction);
	if (graphadj_inline_count(descriptor) > 0)
	{
		entry = graphadj_inline_entries((unsigned char *) slot, direction);
		generation = graphadj_get_u64(entry + GRAPHADJ_ENTRY_REF_OFFSET +
			GRAPHADJ_REF_SLOT_GENERATION_OFFSET);
		if (generation == PG_UINT64_MAX)
			ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
							errmsg("graph_adj fault target generation is exhausted")));
		state = GenericXLogStart(index);
		slot_page = GenericXLogRegisterBuffer(state, slot_buffer, 0);
		entry = graphadj_inline_entries(graphadj_slot(slot_page, ordinal), direction);
		graphadj_put_u64(entry + GRAPHADJ_ENTRY_REF_OFFSET +
			GRAPHADJ_REF_SLOT_GENERATION_OFFSET, generation + 1);
		GenericXLogFinish(state);
		UnlockReleaseBuffer(slot_buffer);
		return true;
	}
	data_ref = graphadj_get_data_ref(descriptor + GRAPHADJ_DESC_ACTIVE_OFFSET);
	if (graphadj_data_ref_is_valid(&data_ref))
		expected_kind = GRAPHADJ_PAGE_DELTA;
	else
	{
		data_ref = graphadj_get_data_ref(
			descriptor + GRAPHADJ_DESC_SEALED_OFFSET);
		if (graphadj_data_ref_is_valid(&data_ref))
			expected_kind = GRAPHADJ_PAGE_DELTA;
		else
		{
			data_ref = graphadj_get_data_ref(
				descriptor + GRAPHADJ_DESC_BASE_OFFSET);
			expected_kind = GRAPHADJ_PAGE_PACKED_BASE;
		}
	}
	if (!graphadj_data_ref_is_valid(&data_ref))
	{
		UnlockReleaseBuffer(slot_buffer);
		return false;
	}

	data_buffer = ReadBuffer(index, data_ref.block);
	LockBuffer(data_buffer, BUFFER_LOCK_EXCLUSIVE);
	data_page = BufferGetPage(data_buffer);
	graphadj_read_opaque(index, data_page, expected_kind, direction,
		&allocation_generation);
	if (allocation_generation != data_ref.segment_generation)
		ereport(ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				 errmsg("graph_adj fault target contains a stale DataRef")));
	segment = (unsigned char *) data_page + data_ref.segment_offset;
	published = graphadj_get_u32(
		segment + GRAPHADJ_SEGMENT_PUBLISHED_OFFSET);
	if (published == 0)
		ereport(ERROR,
				(errcode(ERRCODE_NO_DATA_FOUND),
				 errmsg("graph_adj fault target has no published OUT entry")));

	state = GenericXLogStart(index);
	data_page = GenericXLogRegisterBuffer(state, data_buffer, 0);
	segment = (unsigned char *) data_page + data_ref.segment_offset;
	entry = segment + GRAPHADJ_SEGMENT_HEADER_SIZE;
	generation = graphadj_get_u64(entry + GRAPHADJ_ENTRY_REF_OFFSET +
		GRAPHADJ_REF_SLOT_GENERATION_OFFSET);
	if (generation == PG_UINT64_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("graph_adj fault target generation is exhausted")));
	graphadj_put_u64(entry + GRAPHADJ_ENTRY_REF_OFFSET +
		GRAPHADJ_REF_SLOT_GENERATION_OFFSET, generation + 1);
	GenericXLogFinish(state);

	UnlockReleaseBuffer(data_buffer);
	UnlockReleaseBuffer(slot_buffer);
	return true;
}

Datum
graph_adj_debug_fault(PG_FUNCTION_ARGS)
{
	Oid			index_oid = PG_GETARG_OID(0);
	int64		key = PG_GETARG_INT64(1);
	char	   *point = text_to_cstring(PG_GETARG_TEXT_PP(2));
	Relation	index;
	const char *const crash_points[] = {
		"directory_root_split",
		"directory_child_split",
		"slot_page_linked",
		"slot_initialized",
		"out_directory_published",
		"in_directory_published",
		"pending_cleared",
		"entry_published",
		"shared_segment_published",
		"out_entry_published",
		"sealed_published",
		"base_built",
		"base_published",
		"free_page_published",
		"free_page_popped"
	};
	bool		valid = false;
	bool		error_only = false;
	int			i;

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("graph_adj fault injection requires superuser")));
	if (strncmp(point, "error:", 6) == 0)
	{
		error_only = true;
		point += 6;
	}
	if (strcmp(point, "corrupt_out_ref") == 0)
	{
		bool		changed;

		index = relation_open(index_oid, AccessExclusiveLock);
		graphadj_debug_require_index(index);
		changed = graphadj_corrupt_first_out_ref(index, key);
		relation_close(index, AccessExclusiveLock);
		PG_RETURN_BOOL(changed);
	}

	for (i = 0; i < lengthof(crash_points); i++)
	{
		if (strcmp(point, crash_points[i]) == 0)
		{
			valid = true;
			break;
		}
	}
	if (!valid)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unknown graph_adj fault point \"%s\"", point)));

	index = relation_open(index_oid, AccessShareLock);
	graphadj_debug_require_index(index);
	relation_close(index, AccessShareLock);
	graphadj_fault_index_oid = index_oid;
	graphadj_fault_key = key;
	graphadj_fault_error = error_only;
	strlcpy(graphadj_fault_name, point, sizeof(graphadj_fault_name));
	PG_RETURN_BOOL(true);
}

/*-------------------------------------------------------------------------
 * Native bounded-batch provider
 *-------------------------------------------------------------------------
 */

#define GRAPHADJ_PROVIDER_CAPS \
	(GGC_CAP_BUILD_GENERATION | GGC_CAP_OUT | GGC_CAP_IN | \
	 GGC_CAP_COMPLETE_CANDIDATES | \
	 GGC_CAP_ROOT_TID | GGC_CAP_RESCAN | GGC_CAP_SSI | GGC_CAP_BATCH | \
	 GGC_CAP_PAUSE_RESUME | GGC_CAP_HANDLE_INPUT | GGC_CAP_HANDLE_OUTPUT | \
	 GGC_CAP_PATH_INDEPENDENT | GGC_CAP_STARTUP_REUSE | GGC_CAP_HEAP_STREAM)

struct GgcAdjScanV2
{
	MemoryContext context;
	MemoryContext am_context;
	MemoryContext request_context;
	const GgcAdjProviderV2 *provider;
	GgcOpenRequestV2 request;
	Relation	index;
	GraphAdjMetaData meta;
	GraphAdjCandidateVector candidates;
	uint32		endpoint;
	uint32		candidate_pos;
	uint64		ordinal;
	uint64		descriptor_generation;
	GraphAdjAccessCounters counters;
	GraphAdjRefCache ref_cache;
	BlockNumber known_blocks;
};

static bool
graphadj_provider_reject(GgcRejectReason *reason, GgcRejectReason value)
{
	if (reason)
		*reason = value;
	return false;
}

static bool
graphadj_provider_scope_valid(const GgcOpenRequestV2 *r,
							  GgcRejectReason *reason)
{
	Oid			role;
	int			security;
	MemoryContext ancestor;

	if (!r || r->struct_size != sizeof(*r) ||
		r->abi_version != GGC_ADJ_ABI_VERSION)
		return graphadj_provider_reject(reason, GGC_REJECT_ABI);
	if (!r->estate || !r->scan_context || !r->resource_owner ||
		r->resource_owner != CurrentResourceOwner)
		return graphadj_provider_reject(reason, GGC_REJECT_RESOURCE);
	for (ancestor = r->scan_context; ancestor;
		 ancestor = MemoryContextGetParent(ancestor))
		if (ancestor == r->estate->es_query_cxt)
			break;
	if (!ancestor)
		return graphadj_provider_reject(reason, GGC_REJECT_RESOURCE);
	if (!r->snapshot || !IsMVCCSnapshot(r->snapshot) ||
		r->snapshot->regd_count == 0 ||
		r->estate->es_snapshot != r->snapshot ||
		r->snapshot->curcid != r->command_id ||
		GetCurrentCommandId(false) != r->command_id)
		return graphadj_provider_reject(reason, GGC_REJECT_SNAPSHOT);
	GetUserIdAndSecContext(&role, &security);
	if (r->effective_role != role || r->security_context != security)
		return graphadj_provider_reject(reason, GGC_REJECT_SECURITY);
	return true;
}

static bool
graphadj_provider_relation_valid(const GgcOpenRequestV2 *r, Relation edge,
								 Relation index, GraphAdjMetaData *meta,
								 GgcRejectReason *reason)
{
	TupleDesc	desc = RelationGetDescr(edge);

	if (edge->rd_rel->relrowsecurity ||
		pg_class_aclcheck(r->edge_relid, r->effective_role,
						  ACL_SELECT) != ACLCHECK_OK)
		return graphadj_provider_reject(reason, GGC_REJECT_SECURITY);
	if (edge->rd_rel->relkind != RELKIND_RELATION ||
		edge->rd_rel->relam != HEAP_TABLE_AM_OID ||
		RELATION_IS_OTHER_TEMP(edge) ||
		index->rd_rel->relkind != RELKIND_INDEX ||
		index->rd_rel->relam != GRAPH_ADJ_AM_OID ||
		!index->rd_index || index->rd_index->indrelid != r->edge_relid ||
		!index->rd_index->indisvalid || !index->rd_index->indisready ||
		!index->rd_index->indislive || index->rd_index->indcheckxmin ||
		index->rd_index->indnkeyatts != 2 ||
		index->rd_index->indnatts != 2 ||
		index->rd_index->indkey.values[0] != r->source_attnum ||
		index->rd_index->indkey.values[1] != r->destination_attnum ||
		RelationGetIndexExpressions(index) != NIL ||
		RelationGetIndexPredicate(index) != NIL)
		return graphadj_provider_reject(reason, GGC_REJECT_SHAPE);
	if (r->source_attnum <= 0 || r->source_attnum > desc->natts ||
		r->destination_attnum <= 0 || r->destination_attnum > desc->natts ||
		r->source_attnum == r->destination_attnum)
		return graphadj_provider_reject(reason, GGC_REJECT_SHAPE);
	for (int i = 0; i < 2; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(desc,
			(i == 0 ? r->source_attnum : r->destination_attnum) - 1);

		if (attr->attisdropped || attr->atttypid != INT8OID || !attr->attnotnull)
			return graphadj_provider_reject(reason, GGC_REJECT_TYPE);
	}
	if (r->endpoint_type != INT8OID || OidIsValid(r->endpoint_collation) ||
		r->endpoint_domain != r->edge_relid ||
		!OidIsValid(r->endpoint_opfamily) ||
		(r->endpoint_opfamily != index->rd_opfamily[0] &&
		 r->endpoint_opfamily != index->rd_opfamily[1]))
		return graphadj_provider_reject(reason, GGC_REJECT_TYPE);
	graphadj_read_meta(index, meta, NULL);
	if (r->build_generation != meta->build_generation)
		return graphadj_provider_reject(reason, GGC_REJECT_GENERATION);
	return true;
}

static bool
graphadj_provider_request_valid(const GgcOpenRequestV2 *r,
							GgcRejectReason *reason)
{
	if (reason)
		*reason = GGC_ACCEPT;
	if (!graphadj_provider_scope_valid(r, reason))
		return false;
	if ((r->required_caps & ~GRAPHADJ_PROVIDER_CAPS) ||
		(r->direction != GGC_OUT && r->direction != GGC_IN))
		return graphadj_provider_reject(reason, GGC_REJECT_CAPABILITY);
	if (!GgcBatchBudgetIsBounded(&r->budget) || !r->requests ||
		r->request_count == 0 || r->request_count > r->budget.max_endpoints ||
		r->budget.max_pins < 4 || r->budget.max_inflight_pages < 4 ||
		r->budget.max_candidate_bytes < sizeof(GgcAdjItemV2) ||
		r->budget.max_restore_bytes <
			(Size) r->request_count * sizeof(GgcEndpointRequestV2))
		return graphadj_provider_reject(reason, GGC_REJECT_RESOURCE);
	for (uint32 i = 0; i < r->request_count; i++)
	{
		const GgcEndpointHandleV2 *handle = &r->requests[i].handle;

		if (handle->origin != GGC_HANDLE_NONE &&
			(handle->provider.struct_size != sizeof(GgcProviderRef) ||
			 handle->provider.abi_version != GGC_ADJ_ABI_VERSION))
			return graphadj_provider_reject(reason, GGC_REJECT_ABI);
		for (uint32 j = 0; j < i; j++)
			if (r->requests[i].request_id == r->requests[j].request_id)
				return graphadj_provider_reject(reason, GGC_REJECT_SHAPE);
	}
	return true;
}

static bool
graphadj_provider_validate_open(const GgcOpenRequestV2 *r, Relation edge,
								Relation index, GgcRejectReason *reason)
{
	GraphAdjMetaData meta;

	if (!graphadj_provider_request_valid(r, reason))
		return false;
	if (!edge || !index || RelationGetRelid(edge) != r->edge_relid ||
		RelationGetRelid(index) != r->index_relid)
		return graphadj_provider_reject(reason, GGC_REJECT_SHAPE);
	return graphadj_provider_relation_valid(r, edge, index, &meta, reason);
}

static bool
graphadj_provider_validate(const GgcOpenRequestV2 *r,
						   GgcRejectReason *reason)
{
	Relation	edge;
	Relation	index;
	GraphAdjMetaData meta;
	bool		valid;

	if (!graphadj_provider_request_valid(r, reason))
		return false;
	edge = table_open(r->edge_relid, AccessShareLock);
	index = index_open(r->index_relid, AccessShareLock);
	valid = graphadj_provider_relation_valid(r, edge, index, &meta, reason);
	index_close(index, AccessShareLock);
	table_close(edge, AccessShareLock);
	return valid;
}

static void
graphadj_provider_reset_candidates(GgcAdjScanV2 *scan)
{
	MemoryContextReset(scan->am_context);
	scan->candidates.items = NULL;
	scan->candidates.count = 0;
	scan->candidates.capacity = 0;
	scan->candidate_pos = 0;
	scan->descriptor_generation = 0;
}

static void
graphadj_provider_copy_request(GgcAdjScanV2 *scan,
						   const GgcOpenRequestV2 *r)
{
	MemoryContext old;
	GgcEndpointRequestV2 *requests;

	MemoryContextReset(scan->request_context);
	old = MemoryContextSwitchTo(scan->request_context);
	requests = palloc(sizeof(*requests) * r->request_count);
	memcpy(requests, r->requests, sizeof(*requests) * r->request_count);
	scan->request = *r;
	scan->request.requests = requests;
	MemoryContextSwitchTo(old);
	scan->endpoint = 0;
	scan->ordinal = 0;
	graphadj_provider_reset_candidates(scan);
}

static void
graphadj_provider_open(GgcAdjScanV2 *scan, const GgcOpenRequestV2 *r)
{
	GgcRejectReason reason = GGC_ACCEPT;
	Relation	edge;

	scan->index = index_open(r->index_relid, AccessShareLock);
	edge = table_open(r->edge_relid, AccessShareLock);
	if (!graphadj_provider_relation_valid(r, edge, scan->index,
			&scan->meta, &reason))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("graph_adj provider relation changed before begin (%d)",
						reason)));
	table_close(edge, NoLock);
	PredicateLockRelation(scan->index, r->snapshot);
	graphadj_provider_copy_request(scan, r);
}

static GgcAdjScanV2 *
graphadj_provider_begin(const GgcOpenRequestV2 *r)
{
	GgcAdjScanV2 *scan;
	GgcRejectReason reason = GGC_ACCEPT;
	MemoryContext context;

	if (!graphadj_provider_request_valid(r, &reason))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("graph_adj provider rejected request (%d)", reason)));
	context = AllocSetContextCreate(r->scan_context, "graph_adj provider",
		ALLOCSET_DEFAULT_SIZES);
	scan = MemoryContextAllocZero(context, sizeof(*scan));
	scan->context = context;
	/* The built-in descriptor has static lifetime.  Register its interfaces
	 * once for this scan, not again for every endpoint or candidate batch.
	 * Snapshot, security, resource and build checks remain per request. */
	scan->provider = GetGgcGraphAdjProvider();
	scan->am_context = AllocSetContextCreate(context,
		"graph_adj provider candidates", ALLOCSET_DEFAULT_SIZES);
	scan->request_context = AllocSetContextCreate(context,
		"graph_adj provider requests", ALLOCSET_DEFAULT_SIZES);
	graphadj_provider_open(scan, r);
	return scan;
}

static bool
graphadj_provider_load_endpoint(GgcAdjScanV2 *scan)
{
	const GgcEndpointRequestV2 *request =
		&scan->request.requests[scan->endpoint];
	const GgcAdjProviderV2 *provider = scan->provider;
	const GgcEndpointHandleV2 *public_handle = &request->handle;
	GraphAdjScopedVertexRef scoped_handle;
	GraphAdjVertexRefData owner_ref;
	GraphAdjScopedVertexRef *handle = NULL;
	GraphAdjDirection direction = (GraphAdjDirection) scan->request.direction;
	Buffer		slot_buffer;
	Page		slot_page;
	const unsigned char *slot;
	const unsigned char *descriptor;
	int64		key = DatumGetInt64(request->key);
	MemoryContext old;

	graphadj_provider_reset_candidates(scan);
	if (public_handle->origin != GGC_HANDLE_NONE)
	{
		if (GgcEndpointHandleScopeMatches(public_handle, &provider->identity,
				scan->request.index_relid, scan->meta.build_generation,
				scan->request.endpoint_domain))
		{
			MemSet(&scoped_handle, 0, sizeof(scoped_handle));
			scoped_handle.index_relid = public_handle->index_relid;
			scoped_handle.build_generation = public_handle->build_generation;
			scoped_handle.endpoint_key = key;
			scoped_handle.vertex_ref.slot_id = public_handle->slot_id;
			scoped_handle.vertex_ref.slot_generation =
				public_handle->slot_generation;
			scoped_handle.vertex_ref.endpoint_domain =
				public_handle->endpoint_domain;
			scoped_handle.authoritative =
				public_handle->origin == GGC_HANDLE_PERSISTED_ADJACENCY;
			handle = &scoped_handle;
		}
		else
			scan->counters.ref_fallbacks++;
	}
	if (handle && handle->authoritative)
	{
		BlockNumber block = GraphAdjSlotIdGetBlock(handle->vertex_ref.slot_id);

		/* Validate once under the very Slot lock used to read the descriptor,
		 * not through the debug probe followed by a second ReadBuffer/lock. */
		if (block >= scan->known_blocks)
			scan->known_blocks = RelationGetNumberOfBlocks(scan->index);
		if (!GraphAdjSlotIdIsValid(handle->vertex_ref.slot_id) || block >= scan->known_blocks)
			ereport(ERROR, (errcode(ERRCODE_INDEX_CORRUPTED),
							errmsg("graph_adj found an invalid persistent Slot address")));
		owner_ref = handle->vertex_ref;
		scan->counters.ref_direct_lookups++;
	}
	else if (graphadj_resolve_debug_handle(scan->index, direction, key, handle,
			&owner_ref, &scan->counters, &scan->meta))
		return false;

	slot_buffer = ReadBuffer(scan->index,
		GraphAdjSlotIdGetBlock(owner_ref.slot_id));
	LockBuffer(slot_buffer, BUFFER_LOCK_SHARE);
	slot_page = BufferGetPage(slot_buffer);
	graphadj_read_opaque(scan->index, slot_page,
		GRAPHADJ_PAGE_ENDPOINT_SLOT, GRAPHADJ_DIRECTION_NONE, NULL);
	if (handle && handle->authoritative)
	{
		if (!graphadj_ref_matches_slot(slot_page, BufferGetBlockNumber(slot_buffer), key, &owner_ref))
			ereport(ERROR,
					(errcode(ERRCODE_INDEX_CORRUPTED),
					 errmsg("graph_adj found a conflicting persistent VertexRef"),
					 errdetail("Index \"%s\" key %lld has an invalid Slot identity or generation.",
						RelationGetRelationName(scan->index), (long long) key)));
	}
	else
		graphadj_validate_slot_identity(scan->index, slot_buffer, key, &owner_ref);
	scan->counters.slot_dereferences++;
	slot = graphadj_slot(slot_page, GraphAdjSlotIdGetOrdinal(owner_ref.slot_id));
	descriptor = graphadj_descriptor((unsigned char *) slot, direction);
	scan->descriptor_generation = graphadj_get_u64(
		descriptor + GRAPHADJ_DESC_VIEW_GENERATION_OFFSET);
	old = MemoryContextSwitchTo(scan->am_context);
	graphadj_collect_candidates(scan->index, slot_buffer, &owner_ref,
		direction, false, 0, &scan->candidates, &scan->ref_cache);
	MemoryContextSwitchTo(old);
	UnlockReleaseBuffer(slot_buffer);
	if ((Size) scan->candidates.count * sizeof(GraphAdjCandidateData) >
		scan->request.budget.max_candidate_bytes)
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("one graph_adj endpoint exceeds the candidate byte budget")));
	return true;
}

static int
graphadj_provider_getnext(GgcAdjScanV2 *scan, GgcAdjItemV2 *items,
						  int capacity)
{
	GgcRejectReason reason = GGC_ACCEPT;
	Size		limit;
	int			count = 0;
	const GgcAdjProviderV2 *provider = scan->provider;

	if (!items || capacity <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("graph_adj batch capacity must be positive")));
	if (!graphadj_provider_scope_valid(&scan->request, &reason))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("graph_adj provider scope changed during scan (%d)",
						reason)));
	limit = Min((Size) capacity, (Size) scan->request.budget.max_candidates);
	limit = Min(limit,
		scan->request.budget.max_candidate_bytes / sizeof(*items));
	while (count < limit && scan->endpoint < scan->request.request_count)
	{
		const GgcEndpointRequestV2 *request =
			&scan->request.requests[scan->endpoint];

		CHECK_FOR_INTERRUPTS();
		if (scan->candidate_pos >= scan->candidates.count)
		{
			if (!graphadj_provider_load_endpoint(scan) ||
				scan->candidates.count == 0)
			{
				scan->endpoint++;
				continue;
			}
		}
		while (count < limit && scan->candidate_pos < scan->candidates.count)
		{
			GraphAdjCandidateData *candidate =
				&scan->candidates.items[scan->candidate_pos++];
			GgcAdjItemV2 *item = &items[count++];

			if (scan->ordinal == PG_UINT64_MAX)
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("graph_adj candidate ordinal overflow")));
			MemSet(item, 0, sizeof(*item));
			item->request_id = request->request_id;
			item->original_ordinal = scan->ordinal++;
			item->edge_relid = scan->request.edge_relid;
			item->root_tid = candidate->root_tid;
			item->opposite_key = Int64GetDatum(candidate->opposite_key);
			item->opposite_key_available = true;
			item->opposite_isnull = false;
			item->opposite_handle.provider = provider->identity;
			item->opposite_handle.index_relid = scan->request.index_relid;
			item->opposite_handle.build_generation =
				scan->meta.build_generation;
			item->opposite_handle.endpoint_domain =
				candidate->opposite_ref.endpoint_domain;
			item->opposite_handle.slot_id = candidate->opposite_ref.slot_id;
			item->opposite_handle.slot_generation =
				candidate->opposite_ref.slot_generation;
			item->opposite_handle.origin = GGC_HANDLE_PERSISTED_ADJACENCY;
			item->descriptor_generation = scan->descriptor_generation;
			scan->counters.physical_candidates++;
		}
		if (scan->candidate_pos >= scan->candidates.count)
			scan->endpoint++;
	}
	return count;
}

static void
graphadj_provider_close(GgcAdjScanV2 *scan)
{
	if (scan->index)
		index_close(scan->index, NoLock);
	scan->index = NULL;
	graphadj_provider_reset_candidates(scan);
}

static void
graphadj_provider_rescan(GgcAdjScanV2 *scan, const GgcOpenRequestV2 *r)
{
	GgcRejectReason reason = GGC_ACCEPT;

	if (!graphadj_provider_request_valid(r, &reason))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("graph_adj provider rejected rescan (%d)", reason)));
	if (r->estate != scan->request.estate ||
		r->snapshot != scan->request.snapshot ||
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
		r->build_generation != scan->request.build_generation ||
		r->build_generation != scan->meta.build_generation)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("graph_adj provider cannot change execution scope on rescan")));
	/*
	 * begin_batch retained AccessShareLock on the immutable relation scope and
	 * the same MVCC snapshot/owner is required above.  Reopening both
	 * relations and rereading block 0 for every bounded endpoint group adds no
	 * safety; DDL that could replace this build cannot pass the held locks.
	 */
	graphadj_provider_copy_request(scan, r);
}

static void
graphadj_provider_end(GgcAdjScanV2 *scan)
{
	MemoryContext context = scan->context;

	graphadj_provider_close(scan);
	MemoryContextDelete(context);
}

static bool
graphadj_provider_estimate(const GgcEstimateRequestV2 *r,
						   GgcEstimateV2 *estimate)
{
	MemSet(estimate, 0, sizeof(*estimate));
	estimate->uncertainty_unknown = true;
	return false;
}

static uint64
graphadj_provider_generation(Oid graph_oid, Oid edge_relid)
{
	/* The request's concrete index OID is required for authoritative lookup. */
	return 0;
}

static void
graphadj_provider_get_instrumentation(GgcAdjScanV2 *scan,
								  GgcProviderInstrumentation *instrumentation)
{
	MemSet(instrumentation, 0, sizeof(*instrumentation));
	instrumentation->directory_lookups = scan->counters.directory_lookups;
	instrumentation->ref_direct_lookups = scan->counters.ref_direct_lookups;
	instrumentation->ref_fallbacks = scan->counters.ref_fallbacks;
	instrumentation->slot_dereferences = scan->counters.slot_dereferences;
	instrumentation->physical_candidates = scan->counters.physical_candidates;
}

static uint64
graphadj_index_metadata_generation(Relation index)
{
	return graphadj_index_build_generation(RelationGetRelid(index));
}

const GgcAdjProviderV2 *
GetGgcGraphAdjProvider(void)
{
	static const GgcIndexMetadataOps metadata = {
		.struct_size = sizeof(GgcIndexMetadataOps),
		.version = GGC_INDEX_METADATA_VERSION,
		.build_generation = graphadj_index_metadata_generation
	};
	static const GgcProviderSetupOps setup = {
		.struct_size = sizeof(GgcProviderSetupOps),
		.version = GGC_PROVIDER_SETUP_VERSION,
		.validate_open = graphadj_provider_validate_open,
		.self_validating_begin = true
	};
	static const GgcAdjProviderV2 provider = {
		.struct_size = sizeof(GgcAdjProviderV2),
		.abi_version = GGC_ADJ_ABI_VERSION,
		.identity = {
			.struct_size = sizeof(GgcProviderRef),
			.abi_version = GGC_ADJ_ABI_VERSION,
			.kind = 5,
			.name = {"graph_adj"}
		},
		.capabilities = GRAPHADJ_PROVIDER_CAPS,
		.validate = graphadj_provider_validate,
		.estimate = graphadj_provider_estimate,
		.begin_batch = graphadj_provider_begin,
		.getnext_batch = graphadj_provider_getnext,
		.rescan = graphadj_provider_rescan,
		.end = graphadj_provider_end,
		.generation = graphadj_provider_generation,
		.get_instrumentation = graphadj_provider_get_instrumentation
	};

	GgcRegisterProviderSetup(&provider, &setup);
	GgcRegisterIndexMetadata(&provider, &metadata);
	return &provider;
}
