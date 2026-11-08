/*-------------------------------------------------------------------------
 * graphstats.h
 *
 * Bounded planner consumption and costing for graph statistics.  These are
 * runtime structs only; catalog payloads remain owned by graph_stats.h.
 *-------------------------------------------------------------------------
 */
#ifndef OPTIMIZER_GRAPHSTATS_H
#define OPTIMIZER_GRAPHSTATS_H

#include "nodes/pathnodes.h"
#include "statistics/graph_stats.h"

#define GGC_GRAPH_COST_PROFILE_VERSION 1U
#define GGC_GRAPH_STATS_DEFAULT_MAX_PROFILES 64U
#define GGC_GRAPH_STATS_DEFAULT_MAX_BYTES (1024 * 1024)

typedef enum GgcGraphCostMetricId
{
	GGC_METRIC_COST_BATCH_STARTUP = 300,
	GGC_METRIC_COST_KEY_PROBE,
	GGC_METRIC_COST_REF_DEREF,
	GGC_METRIC_COST_ADJ_ITEM,
	GGC_METRIC_COST_HEAP_FETCH,
	GGC_METRIC_COST_HEAP_PAGE_GROUP,
	GGC_METRIC_COST_BINDING_EXTEND
} GgcGraphCostMetricId;

/* Only predicates executed by this stage belong here.  Selectivities exclude
 * already-bound vertex restrictions; their recheck CPU cost can remain.
 * A nonzero sampled_edge_clause_id identifies the exact edge predicate already
 * represented by matching logical samples, never merely a similar predicate. */
typedef struct GgcGraphStageQualEstimate
{
	Selectivity edge_selectivity;
	Selectivity vertex_selectivity;
	Selectivity binding_selectivity;
	QualCost	edge_cost;
	QualCost	vertex_cost;
	QualCost	binding_cost;
	uint32		sampled_edge_clause_id;
} GgcGraphStageQualEstimate;

typedef struct GgcGraphEstimateRequest
{
	PlannerInfo *planner_root;	/* optional plan-cache dependency sink */
	GgcStatsScope logical_scope;
	GgcStatsScope physical_scope;
	GgcStatsScope cost_scope;
	uint32		hops;
	uint32		predicate_clause_id;
	double		anchor_bindings;
	double		anchor_distinct;	/* distinct endpoint keys after anchor quals */
	const double *ref_fractions;	/* hops entries, derived by GraphProgram */
	/* Optional ordinary-PG-statistics fallback; no catalog writes. Zero or
	 * absent fanout entries mean unknown. NDV bounds are estimates, never
	 * uniqueness proofs. Endpoint sets are local to each anchor binding. */
	const double *fallback_fanouts;
	const double *endpoint_ndistinct;
	const GgcGraphStageQualEstimate *stage_quals; /* optional, hops entries */
	bool		endpoint_sets;
	uint32		max_profiles;
	Size		max_decoded_bytes;
	uint32		max_statistics_age_ms; /* deprecated/ignored; counters establish freshness */
} GgcGraphEstimateRequest;

typedef struct GgcGraphCostProfile
{
	double		batch_startup;
	double		key_probe;
	double		ref_deref;
	double		adj_item;
	double		heap_fetch;
	double		heap_page_group;
	double		binding_extend;
	uint32		version;
	uint64		statistics_generation;
	bool		calibrated;
} GgcGraphCostProfile;

typedef struct GgcGraphStageCost
{
	double		frontier;
	double		bindings;
	double		requests;
	double		logical_edges;
	double		accepted_edges;
	double		vertex_probes; /* after edge quals, before vertex/binding quals */
	double		binding_qual_rows; /* after local quals, before prefix quals */
	double		next_bindings;
	double		next_endpoints;	/* estimated per-anchor sets, summed over anchors */
	double		physical_entries;
	double		edge_heap_fetches;
	double		heap_page_groups;
	double		peak_restore_bytes;
	double		key_requests;
	double		ref_requests;
	Cost		startup_cost;
	Cost		total_cost;
	Cost		qual_startup_cost;
	Cost		qual_cost;
	bool		logical_fallback;
	bool		physical_fallback;
} GgcGraphStageCost;

typedef struct GgcGraphPathCost
{
	uint32		stage_count;
	GgcGraphStageCost *stages;
	Cost		startup_cost;
	Cost		total_cost;
	uint64		logical_generation;
	uint64		physical_generation;
	GgcGraphCostProfile cost_profile;
	GgcStatsState logical_state;
	GgcStatsState physical_state;
	GgcStatsState cost_state;
	uint32		loaded_profiles;
	Size		decoded_bytes;
	bool		uncertainty_unknown;
} GgcGraphPathCost;

/* Internal handoff between bounded catalog loading and cost conversion. */
typedef struct GgcGraphLoadedValue
{
	double		value;
	uint64		sample_count;
	uint64		collection_samples;
	GgcMetricKind kind;
	GgcStatsState state;
	bool		present;
	bool		censored;
} GgcGraphLoadedValue;

typedef struct GgcGraphLoadedStage
{
	GgcGraphLoadedValue frontier;
	GgcGraphLoadedValue bindings;
	GgcGraphLoadedValue requests;
	GgcGraphLoadedValue logical_edges;
	GgcGraphLoadedValue accepted_edges;
	GgcGraphLoadedValue next_bindings;
	GgcGraphLoadedValue next_endpoints;
	GgcGraphLoadedValue physical_entries;
	GgcGraphLoadedValue edge_fetches;
	GgcGraphLoadedValue heap_page_groups;
	GgcGraphLoadedValue peak_restore_bytes;
} GgcGraphLoadedStage;

typedef struct GgcGraphLoadedPath
{
	bool		counter_freshness_unknown;
	bool		metric_uncertainty_unknown;
	GgcGraphLoadedStage *stages;
	uint32		stage_count;
	uint64		logical_generation;
	uint64		physical_generation;
	GgcGraphCostProfile cost_profile;
	GgcStatsState logical_state;
	GgcStatsState physical_state;
	GgcStatsState cost_state;
	uint32		loaded_profiles;
	Size		decoded_bytes;
} GgcGraphLoadedPath;

extern void GgcLoadGraphPathStatistics(const GgcGraphEstimateRequest *request,
									   MemoryContext planner_context,
									   GgcGraphLoadedPath *loaded);

extern bool GgcEstimateGraphPath(const GgcGraphEstimateRequest *request,
								 MemoryContext planner_context,
								 GgcGraphPathCost *result);

/* Additive APIs; existing global callers stay
 * unchanged. The caller proves a singleton bigint anchor predicate; both
 * registered scope and full signed key must match, otherwise stats are absent.
 * This is conditional estimation, NOT authorization to select factor mode. */
extern void GgcLoadGraphPathStatisticsForAnchor(const GgcGraphEstimateRequest *request,
												  int64 anchor_key, MemoryContext planner_context,
												  GgcGraphLoadedPath *loaded);
extern bool GgcEstimateGraphPathForAnchor(const GgcGraphEstimateRequest *request,
										   int64 anchor_key, MemoryContext planner_context,
										   GgcGraphPathCost *result);

/* Relational cardinality estimation needs no concrete adjacency provider or
 * physical/cost profile. Same bounded, security-scoped logical lookup. */
extern void GgcLoadGraphLogicalStatisticsForAnchor(const GgcGraphEstimateRequest *request,
													 int64 anchor_key, MemoryContext planner_context,
													 GgcGraphLoadedPath *loaded);

#endif							/* OPTIMIZER_GRAPHSTATS_H */
