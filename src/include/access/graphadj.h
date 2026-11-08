/*-------------------------------------------------------------------------
 *
 * graphadj.h
 *    Fixed-width prototype format for the graph_adj index AM.
 *
 * The byte offsets below are the disk contract.  Backend C structure layout
 * is deliberately not used for persistent data.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GRAPHADJ_H
#define GRAPHADJ_H

#include "access/amapi.h"
#include "fmgr.h"
#include "storage/block.h"
#include "storage/bufpage.h"

#define GRAPHADJ_MAGIC ((uint32) 0x47474132U) /* ASCII "GGA2" */
/* V7 adds internal levels/fences; V6 is reserved by the separate chain trial. */
#define GRAPHADJ_FORMAT_VERSION ((uint32) 7U)
#define GRAPHADJ_BUILD_GENERATION UINT64CONST(1)
#define GRAPHADJ_FIRST_ALLOCATION_GENERATION UINT64CONST(10)

#define GRAPHADJ_META_BLKNO ((BlockNumber) 0)
#define GRAPHADJ_OUT_ROOT_BLKNO ((BlockNumber) 1)
#define GRAPHADJ_IN_ROOT_BLKNO ((BlockNumber) 2)
#define GRAPHADJ_SLOT_EXTENT_BLKNO ((BlockNumber) 3)

typedef enum GraphAdjPageKind
{
	GRAPHADJ_PAGE_META = 1,
	GRAPHADJ_PAGE_DIR_INTERNAL = 2,
	GRAPHADJ_PAGE_DIR_LEAF = 3,
	GRAPHADJ_PAGE_ENDPOINT_SLOT = 4,
	GRAPHADJ_PAGE_PACKED_BASE = 5,
	GRAPHADJ_PAGE_DELTA = 6,
	GRAPHADJ_PAGE_RETIRED = 7,
	GRAPHADJ_PAGE_FREE = 8
} GraphAdjPageKind;

typedef enum GraphAdjDirection
{
	GRAPHADJ_DIRECTION_NONE = 0,
	GRAPHADJ_DIRECTION_OUT = 1,
	GRAPHADJ_DIRECTION_IN = 2
} GraphAdjDirection;

/* Page special-space encoding. */
#define GRAPHADJ_OPAQUE_KIND_OFFSET 0
#define GRAPHADJ_OPAQUE_DIRECTION_OFFSET 2
#define GRAPHADJ_OPAQUE_FORMAT_OFFSET 4
#define GRAPHADJ_OPAQUE_ALLOCATION_GENERATION_OFFSET 8
#define GRAPHADJ_OPAQUE_RIGHTLINK_OFFSET 16
#define GRAPHADJ_OPAQUE_FLAGS_OFFSET 20
#define GRAPHADJ_OPAQUE_SIZE 24

/* Meta body encoding, beginning at PageGetContents(page). */
#define GRAPHADJ_META_MAGIC_OFFSET 0
#define GRAPHADJ_META_FORMAT_OFFSET 4
#define GRAPHADJ_META_BUILD_GENERATION_OFFSET 8
#define GRAPHADJ_META_OUT_ROOT_OFFSET 16
#define GRAPHADJ_META_IN_ROOT_OFFSET 20
#define GRAPHADJ_META_SLOT_EXTENT_OFFSET 24
#define GRAPHADJ_META_RETIREMENT_HEAD_OFFSET 28
#define GRAPHADJ_META_FREE_HEAD_OFFSET 32
#define GRAPHADJ_META_FLAGS_OFFSET 36
#define GRAPHADJ_META_SLOT_TAIL_OFFSET 40
#define GRAPHADJ_META_PENDING_SLOT_OFFSET 48
#define GRAPHADJ_META_SIZE 72

/* Stable logical encodings used by subsequent storage tasks. */
#define GRAPHADJ_VERTEX_REF_SIZE 24
#define GRAPHADJ_DATA_REF_SIZE 16
#define GRAPHADJ_DESCRIPTOR_SIZE 64
#define GRAPHADJ_INLINE_ITEMS 8
#define GRAPHADJ_SLOT_OUT_INLINE_OFFSET 152
#define GRAPHADJ_SLOT_IN_INLINE_OFFSET (152 + GRAPHADJ_INLINE_ITEMS * GRAPHADJ_ENTRY_SIZE)
#define GRAPHADJ_ENDPOINT_SLOT_SIZE (152 + 2 * GRAPHADJ_INLINE_ITEMS * GRAPHADJ_ENTRY_SIZE)
#define GRAPHADJ_DIRECTORY_LEAF_ITEM_SIZE 40
#define GRAPHADJ_ENTRY_SIZE 40
#define GRAPHADJ_SEGMENT_HEADER_SIZE 64
#define GRAPHADJ_SLOT_PAGE_HEADER_SIZE 8
#define GRAPHADJ_DIRECTORY_PAGE_HEADER_SIZE 8

/* Scratch-array upper bound; actual splitting uses each leaf's byte capacity. */
#define GRAPHADJ_DIRECTORY_SPLIT_ITEMS (BLCKSZ / GRAPHADJ_DIRECTORY_LEAF_ITEM_SIZE)
#define GRAPHADJ_SMALL_SEGMENT_ITEMS 8
#define GRAPHADJ_DIRECTORY_INTERNAL_ITEM_SIZE 16
#define GRAPHADJ_DIR_INTERNAL_HIGH_KEY_OFFSET 0
#define GRAPHADJ_DIR_INTERNAL_CHILD_OFFSET 8
#define GRAPHADJ_DIR_INTERNAL_RESERVED_OFFSET 12

/* Directory leaf item: size/kind/reserved, key, then a 24-byte VertexRef. */
#define GRAPHADJ_DIR_ITEM_SIZE_OFFSET 0
#define GRAPHADJ_DIR_ITEM_KIND_OFFSET 4
#define GRAPHADJ_DIR_ITEM_KEY_OFFSET 8
#define GRAPHADJ_DIR_ITEM_REF_OFFSET 16
#define GRAPHADJ_DIR_ITEM_KIND_ENDPOINT 1

/* VertexRef: stable slot address, slot generation, endpoint domain. */
#define GRAPHADJ_REF_SLOT_ID_OFFSET 0
#define GRAPHADJ_REF_SLOT_GENERATION_OFFSET 8
#define GRAPHADJ_REF_DOMAIN_OFFSET 16
#define GRAPHADJ_REF_RESERVED_OFFSET 20

/* Endpoint Slot: identity followed by OUT and IN descriptors. */
#define GRAPHADJ_SLOT_GENERATION_OFFSET 0
#define GRAPHADJ_SLOT_DOMAIN_OFFSET 8
#define GRAPHADJ_SLOT_STATE_OFFSET 12
#define GRAPHADJ_SLOT_KEY_OFFSET 16
#define GRAPHADJ_SLOT_OUT_DESCRIPTOR_OFFSET 24
#define GRAPHADJ_SLOT_IN_DESCRIPTOR_OFFSET 88
#define GRAPHADJ_SLOT_STATE_LIVE 2

/* Format 4 descriptor: generation, three external DataRefs, lane/inline counts. */
#define GRAPHADJ_DESC_VIEW_GENERATION_OFFSET 0
#define GRAPHADJ_DESC_BASE_OFFSET 8
#define GRAPHADJ_DESC_SEALED_OFFSET 24
#define GRAPHADJ_DESC_ACTIVE_OFFSET 40
#define GRAPHADJ_DESC_LANE_COUNT_OFFSET 56
#define GRAPHADJ_DESC_INLINE_COUNT_OFFSET 60

/* DataRef: block, stable segment offset, allocation/segment generation. */
#define GRAPHADJ_DATA_REF_BLOCK_OFFSET 0
#define GRAPHADJ_DATA_REF_SEGMENT_OFFSET 4
#define GRAPHADJ_DATA_REF_GENERATION_OFFSET 8

/* A DataRef addresses one of the independently owned segments packed in a page. */
#define GRAPHADJ_SEGMENT_OWNER_OFFSET 0
#define GRAPHADJ_SEGMENT_VIEW_GENERATION_OFFSET 24
#define GRAPHADJ_SEGMENT_NEXT_OFFSET 32
#define GRAPHADJ_SEGMENT_LANE_OFFSET 48
#define GRAPHADJ_SEGMENT_PUBLISHED_OFFSET 52
#define GRAPHADJ_SEGMENT_CAPACITY_OFFSET 56
#define GRAPHADJ_SEGMENT_DIRECTION_OFFSET 60
#define GRAPHADJ_SEGMENT_SEALED_OFFSET 62

/* Entry: opposite key/ref, heap root TID and flags. */
#define GRAPHADJ_ENTRY_KEY_OFFSET 0
#define GRAPHADJ_ENTRY_REF_OFFSET 8
#define GRAPHADJ_ENTRY_TID_BLOCK_OFFSET 32
#define GRAPHADJ_ENTRY_TID_OFFSET_OFFSET 36
#define GRAPHADJ_ENTRY_FLAGS_OFFSET 38

#define GRAPHADJ_INVALID_SLOT_ID UINT64CONST(0)
#define GRAPHADJ_FIRST_SLOT_ORDINAL ((uint32) 1U)

/* In-memory values.  Persistent representations always use the codecs. */
typedef struct GraphAdjVertexRefData
{
	uint64		slot_id;
	uint64		slot_generation;
	uint32		endpoint_domain;
} GraphAdjVertexRefData;

typedef struct GraphAdjScopedVertexRef
{
	Oid			index_relid;
	uint64		build_generation;
	int64		endpoint_key;
	GraphAdjVertexRefData vertex_ref;
	bool		authoritative;
} GraphAdjScopedVertexRef;

typedef struct GraphAdjAccessCounters
{
	uint64		directory_lookups;
	uint64		ref_direct_lookups;
	uint64		ref_fallbacks;
	uint64		slot_dereferences;
	uint64		physical_candidates;
	uint64		tableam_calls;
	uint64		visible_candidates;
	uint64		output_bindings;
} GraphAdjAccessCounters;

/* A slot id decodes directly: upper 32 bits block, lower 32 bits ordinal. */
static inline uint64
GraphAdjMakeSlotId(BlockNumber block, uint32 ordinal)
{
	Assert(block != InvalidBlockNumber);
	Assert(block != GRAPHADJ_META_BLKNO);
	Assert(ordinal >= GRAPHADJ_FIRST_SLOT_ORDINAL);
	return (((uint64) block) << 32) | ordinal;
}

static inline BlockNumber
GraphAdjSlotIdGetBlock(uint64 slot_id)
{
	return (BlockNumber) (slot_id >> 32);
}

static inline uint32
GraphAdjSlotIdGetOrdinal(uint64 slot_id)
{
	return (uint32) slot_id;
}

static inline bool
GraphAdjSlotIdIsValid(uint64 slot_id)
{
	return slot_id != GRAPHADJ_INVALID_SLOT_ID &&
		GraphAdjSlotIdGetBlock(slot_id) != InvalidBlockNumber &&
		GraphAdjSlotIdGetBlock(slot_id) != GRAPHADJ_META_BLKNO &&
		GraphAdjSlotIdGetOrdinal(slot_id) >= GRAPHADJ_FIRST_SLOT_ORDINAL;
}

static inline bool
GraphAdjDataRefBoundsValid(BlockNumber block, uint32 segment_offset,
						   uint64 segment_generation)
{
	Size		limit = BLCKSZ - MAXALIGN(GRAPHADJ_OPAQUE_SIZE);

	return block != InvalidBlockNumber && block != GRAPHADJ_META_BLKNO &&
		segment_generation != 0 &&
		segment_offset >= MAXALIGN(SizeOfPageHeaderData) &&
		segment_offset % MAXIMUM_ALIGNOF == 0 &&
		segment_offset <= limit - GRAPHADJ_SEGMENT_HEADER_SIZE;
}

extern Datum graphadjhandler(PG_FUNCTION_ARGS);
extern Datum graph_adj_index_info(PG_FUNCTION_ARGS);
extern uint64 graphadj_index_build_generation(Oid index_oid);
extern Datum graph_adj_format_supported(PG_FUNCTION_ARGS);
extern Datum graph_adj_debug_ref_probe(PG_FUNCTION_ARGS);
extern Datum graph_adj_debug_walk(PG_FUNCTION_ARGS);
extern Datum graph_adj_debug_compact(PG_FUNCTION_ARGS);
extern Datum graph_adj_debug_fault(PG_FUNCTION_ARGS);

#endif /* GRAPHADJ_H */
