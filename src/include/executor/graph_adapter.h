/*-------------------------------------------------------------------------
 * Catalog and result adapter ABI.  Both remain V1 while adjacency is V2.
 *-------------------------------------------------------------------------
 */
#ifndef GRAPH_ADAPTER_H
#define GRAPH_ADAPTER_H

#include "executor/graph_provider.h"

typedef struct GgcCatalogContext
{
	uint32		struct_size;
	uint32		abi_version;
	MemoryContext result_context;
	Snapshot	snapshot;
	Oid			effective_role;
	int			security_context;
} GgcCatalogContext;

typedef struct GgcCatalogProviderV1
{
	uint32		struct_size;
	uint32		abi_version;
	GgcProviderRef identity;
	bool		(*resolve_variables) (const GgcCatalogContext *, Oid graph_oid,
								  List *frontend_element_names, List **variables,
								  uint64 *generation, GgcRejectReason *reason);
	uint64		(*generation) (Oid graph_oid);
} GgcCatalogProviderV1;

typedef struct GgcVisibleValue
{
	GgcEntityKind kind;
	Oid			relation_oid;
	Datum		identity_value;
	Oid			identity_type;
	TupleTableSlot *visible_tuple;
} GgcVisibleValue;

typedef struct GgcVisiblePath
{
	uint32		vertex_count;
	uint32		edge_count;
	const GgcVisibleValue *vertices;
	const GgcVisibleValue *edges;
	const GgcDirection *directions;
} GgcVisiblePath;

typedef struct GgcResultContext
{
	uint32		struct_size;
	uint32		abi_version;
	EState	   *estate;
	ExprContext *expression_context;
	MemoryContext output_context;
	Oid			output_collation;
} GgcResultContext;

typedef struct GgcResultAdapterV1
{
	uint32		struct_size;
	uint32		abi_version;
	GgcProviderRef identity;
	TupleDesc	(*describe) (const GgcFixedProgram *, MemoryContext);
	Datum		(*materialize_entity) (const GgcResultContext *,
								  const GgcVisibleValue *, bool *isnull);
	Datum		(*materialize_path) (const GgcResultContext *,
								const GgcVisiblePath *, bool *isnull);
} GgcResultAdapterV1;

#endif							/* GRAPH_ADAPTER_H */
