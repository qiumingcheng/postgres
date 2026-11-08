/*-------------------------------------------------------------------------
 *
 * graphnodes.h
 *    Serializable common fixed-path graph nodes.
 *
 * Runtime provider callbacks and executor state deliberately live outside
 * these nodes.  Cached plans contain only stable provider identities.
 *
 *-------------------------------------------------------------------------
 */
#ifndef GRAPHNODES_H
#define GRAPHNODES_H

#include "nodes/bitmapset.h"
#include "nodes/primnodes.h"

#define GGC_GRAPH_SEMANTIC_VERSION 4U

typedef enum GgcEntityKind
{
	GGC_VERTEX = 1,
	GGC_EDGE = 2
} GgcEntityKind;

typedef enum GgcPathMode
{
	GGC_WALK = 1,
	GGC_TRAIL,
	GGC_ACYCLIC,
	GGC_SIMPLE
} GgcPathMode;

typedef enum GgcScopeKind
{
	GGC_SCOPE_PATH = 1,
	GGC_SCOPE_PATTERN,
	GGC_SCOPE_CLAUSE
} GgcScopeKind;

typedef enum GgcDirection
{
	GGC_OUT = 1,
	GGC_IN = 2,
	/* Logical IR only: providers still receive separate OUT/IN requests. */
	GGC_UNDIRECTED = 3
} GgcDirection;

typedef struct GgcProviderIdentity
{
	NodeTag		type;
	uint32		struct_size;
	uint32		abi_version;
	Oid			owner_extension_oid;
	uint32		kind;
	char	   *name;
} GgcProviderIdentity;

typedef struct GgcVariable
{
	NodeTag		type;
	int			variable_id;
	GgcEntityKind entity_kind;
	Oid			relation_oid;
	Oid			element_oid;	/* exact PGQ element, not the underlying table */
	Index		fallback_rti;
	AttrNumber	identity_attnum;
	AttrNumber	source_attnum;
	AttrNumber	destination_attnum;
	/* Full catalog mappings; scalar attnums above are Invalid for composites. */
	List	   *identity_attnums;
	List	   *source_attnums;
	List	   *destination_attnums;
	Oid			key_type;
	Oid			key_opfamily;
	Oid			key_collation;
	bool		user_visible;
} GgcVariable;

typedef struct GgcQual
{
	NodeTag		type;
	Expr	   *expression;
	Bitmapset  *required_variables pg_node_attr(custom_query_jumble);
	uint32		security_level;
	bool		is_security_qual;
} GgcQual;

typedef struct GgcPathConstraint
{
	NodeTag		type;
	int			constraint_id;
	GgcPathMode mode;
	GgcScopeKind scope_kind;
	Bitmapset  *edge_variables pg_node_attr(custom_query_jumble);
	Bitmapset  *vertex_variables pg_node_attr(custom_query_jumble);
	int			first_vertex;
	int			last_vertex;
} GgcPathConstraint;

typedef struct GgcFixedHop
{
	NodeTag		type;
	int			stage_id;
	int			input_variable;
	int			edge_variable;
	int			output_variable;
	GgcDirection direction;
	Bitmapset  *required_variables pg_node_attr(custom_query_jumble);
	List	   *quals;
	List	   *constraint_ids;
} GgcFixedHop;

typedef struct GgcOutput
{
	NodeTag		type;
	AttrNumber	result_attnum;
	Expr	   *fallback_expression;
	Oid			result_type;
	int32		result_typmod;
	Oid			result_collation;
	Bitmapset  *required_variables pg_node_attr(custom_query_jumble);
} GgcOutput;

typedef struct GgcFixedProgram
{
	NodeTag		type;
	uint32		semantic_version;
	uint32		frontend_kind;
	GgcProviderIdentity *frontend_provider;
	GgcProviderIdentity *catalog_provider;
	GgcProviderIdentity *result_provider;
	Oid			graph_oid;
	Index		graph_rti;
	List	   *variables;
	List	   *hops;
	List	   *path_constraints;
	List	   *stage_order;
	List	   *outputs;
	Bitmapset  *required_outer pg_node_attr(custom_query_jumble);
	/* General native access is needed; integer-provider proofs do not apply. */
	bool		native_access;
	uint64		semantic_fingerprint pg_node_attr(query_jumble_ignore);
	uint64		catalog_generation pg_node_attr(query_jumble_ignore);
} GgcFixedProgram;

#endif							/* GRAPHNODES_H */
