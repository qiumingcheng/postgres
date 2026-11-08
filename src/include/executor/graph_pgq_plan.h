/*-------------------------------------------------------------------------
 * Versioned SQL/PGQ plan metadata and validated runtime descriptions.
 *
 * Only ordinary Nodes are stored in CustomScan.custom_private.  These typed
 * structs are query-context runtime objects, never copied into a saved Plan.
 *-------------------------------------------------------------------------
 */
#ifndef GRAPH_PGQ_PLAN_H
#define GRAPH_PGQ_PLAN_H

#include "executor/graph_fixed.h"
#include "nodes/plannodes.h"
#include "statistics/graph_stats.h"

#define GGC_PGQ_PLAN_VERSION 1

typedef enum GgcPgqOutputKind
{
	GGC_PGQ_OUTPUT_VERTEX = 1,
	GGC_PGQ_OUTPUT_EDGE,
	GGC_PGQ_OUTPUT_PAYLOAD,
	GGC_PGQ_OUTPUT_FILTER_ONLY,
	GGC_PGQ_OUTPUT_PROPERTY
} GgcPgqOutputKind;

typedef struct GgcPgqOutputMapping
{
	GgcPgqOutputKind kind;
	int			position;		/* binding position, or property index */
	bool		typbyval;
	int16		typlen;
	AttrNumber	attribute;
} GgcPgqOutputMapping;

typedef struct GgcPgqLocalColumn
{
	AttrNumber	column;			/* one-based CustomScan slot attribute */
	AttrNumber	attribute;		/* one-based source relation attribute */
} GgcPgqLocalColumn;

typedef struct GgcPgqLocalMapping
{
	int			count;
	GgcPgqLocalColumn *columns;
} GgcPgqLocalMapping;

typedef struct GgcPgqPropertySpec
{
	Index		rti;
	Oid			relation;
	Oid			index;
	GgcPgqOutputKind kind;		/* vertex or edge */
	int			position;
	AttrNumber	key_attribute;
} GgcPgqPropertySpec;

typedef struct GgcPgqJoinMetadata
{
	GgcStatsState logical_state;
	GgcStatsState physical_state;
	GgcStatsState cost_state;
	bool		uncertainty_unknown;
	bool		calibrated;
	uint64		logical_generation;
	uint64		physical_generation;
	uint64		cost_generation;
	uint32		cost_profile_version;
	int			covered_relids;
	int			consumed_quals;
	int			required_outer;
	Index		request_rti;
	Index	   *stage_rtis;		/* stage_count */
	Index	   *vertex_rtis;		/* stage_count+1, or NULL for diagnostics */
	bool		has_binding_positions;
	uint32	   *input_positions;	/* stage_count */
	int32	   *match_positions;	/* stage_count */
	int			anchor_position;
	int			candidates;
	int			candidate_budget;
	bool		enumeration_limited;
} GgcPgqJoinMetadata;

typedef struct GgcPgqPlan
{
	uint32		version;
	bool		join_mode;
	bool		endpoint_set;
	int			pause_ms;
	int			wrapper_mode;
	bool		has_factor_budget;
	Size		factor_bytes;
	uint32		stage_count;
	uint32		stage_specs;		/* number of non-reference wire stages */
	GgcFixedStagePlan *stages;
	GgcFixedAccessPlan *reverse_access;
	bool		has_reverse;
	GgcFixedExecOptions options;
	GgcPgqJoinMetadata join;
	int			output_count;
	GgcPgqOutputMapping *outputs;
	int			local_count;
	GgcPgqLocalMapping *locals;
	int			property_count;
	GgcPgqPropertySpec *properties;
} GgcPgqPlan;

/* The planner may build legacy diagnostic shapes internally; every emitted
 * Plan must use this envelope.  Decoder accepts only the explicit version,
 * validates the complete wire payload before returning any runtime arrays,
 * and never opens a relation or weakens the runtime permission/MVCC checks.
 * Allocations belong to CurrentMemoryContext. */
extern List *GgcPgqEncodeFixedPlan(List *payload);
extern GgcPgqPlan *GgcPgqDecodeFixedPlan(const CustomScan *scan);

/* Native children retain ordinary PG Plans/expressions.  Only our binding
 * maps are decoded: columns are zero-based slot indexes, parameter IDs retain
 * the full int range (they are not AttrNumbers). */
typedef struct GgcPgqNativeParam
{
	int			id;
	int			column;
	Oid			type;
} GgcPgqNativeParam;

typedef struct GgcPgqNativeStage
{
	int			column_count;
	int		   *columns;
	int			parameter_count;
	GgcPgqNativeParam *parameters;
	List	   *quals;			/* ordinary expressions, owned by the Plan */
} GgcPgqNativeStage;

typedef struct GgcPgqNativePlan
{
	uint32		version;
	int			hops;
	int			stage_count;
	int			column_count;
	GgcPgqNativeStage *stages;
	int			parameter_count;
	GgcPgqNativeParam *parameters;	/* sorted by ID; unique public sources */
} GgcPgqNativePlan;

extern List *GgcPgqEncodeNativePlan(List *columns, List *parameters, int hops);
/* Decode a completed, post-setrefs Plan before framework slot initialization. */
extern GgcPgqNativePlan *GgcPgqDecodeNativePlan(const CustomScan *scan);
/* Called at Begin, when the statement's actual PARAM_EXEC slots exist. */
extern void GgcPgqValidateNativeParams(const GgcPgqNativePlan *plan,
									 List *param_exec_types);

#endif							/* GRAPH_PGQ_PLAN_H */
