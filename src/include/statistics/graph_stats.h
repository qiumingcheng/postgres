/* Graph statistics format V1.  Runtime values are never a disk struct image. */
#ifndef GRAPH_STATS_H
#define GRAPH_STATS_H

#include "utils/jsonb.h"
#include "nodes/pg_list.h"

#define GGC_STATS_FORMAT_VERSION 1U
#define GGC_STATS_MAX_PROFILES 64
#define GGC_STATS_MAX_PAYLOAD_BYTES (1024 * 1024)
#define GGC_STATS_MAX_PROFILE_KEY_BYTES 512
#define GGC_STATS_MAX_METADATA_BYTES (64 * 1024)

/* Stable logical single-hop metric identifiers. */
typedef enum GgcSingleHopMetricId
{
	GGC_METRIC_VERTEX_POPULATION = 100,
	GGC_METRIC_VISIBLE_EDGES,
	GGC_METRIC_MEAN_DEGREE,
	GGC_METRIC_ZERO_DEGREE_FRACTION,
	GGC_METRIC_DEGREE_P50,
	GGC_METRIC_DEGREE_P95,
	GGC_METRIC_DEGREE_P99,
	GGC_METRIC_OBSERVED_SAMPLE_MAX,
	GGC_METRIC_SELF_LOOPS,
	GGC_METRIC_PARALLEL_EXTRA_EDGES,
	GGC_METRIC_STRATUM_SURVEY_WEIGHT,
	GGC_METRIC_DISCOVERED_HEAVY_DEGREE
} GgcSingleHopMetricId;

/* Fixed-path logical profiles.  Stage and template live in profile_key. */
typedef enum GgcPathMetricId
{
	GGC_METRIC_PATH_FRONTIER = 200,
	GGC_METRIC_PATH_BINDINGS,
	GGC_METRIC_PATH_REQUESTS,
	GGC_METRIC_PATH_LOGICAL_EDGES,
	GGC_METRIC_PATH_ACCEPTED_EDGES,
	GGC_METRIC_PATH_NEXT_BINDINGS,
	GGC_METRIC_PATH_OVERLAP,
	GGC_METRIC_PATH_NEXT_DISTINCT_RATIO,
	GGC_METRIC_PATH_ARRIVAL_DEGREE,
	GGC_METRIC_PATH_JOINT_VECTOR,
	/* Additive metric; emitted only by endpoint-set collection. */
	GGC_METRIC_PATH_NEXT_ENDPOINTS,
	GGC_METRIC_PHYSICAL_ENTRIES = 220,
	GGC_METRIC_EDGE_FETCHES,
	GGC_METRIC_HEAP_PAGE_GROUPS,
	GGC_METRIC_TABLEAM_CALLS,
	GGC_METRIC_ZERO_DEGREE_REQUESTS,
	GGC_METRIC_PEAK_RESTORE_BYTES
} GgcPathMetricId;

/* Separate populations: bag profiles must not price endpoint-set execution. */
#define GGC_STATS_ALL_BINDINGS 1U
#define GGC_STATS_ENDPOINT_SETS 2U

typedef enum GgcStatsScopeKind
{
	GGC_STATS_LOGICAL = 1,
	GGC_STATS_PHYSICAL = 2,
	GGC_STATS_COST = 3
} GgcStatsScopeKind;

typedef enum GgcStatsState
{
	GGC_STATS_ABSENT = 0,
	GGC_STATS_READY,
	GGC_STATS_PARTIAL,
	GGC_STATS_STALE,
	GGC_STATS_INVALID
} GgcStatsState;

typedef enum GgcMetricKind
{
	GGC_METRIC_EXACT_AT_SAMPLE = 0,
	GGC_METRIC_ESTIMATED,
	GGC_METRIC_LOWER_BOUND,
	GGC_METRIC_UNKNOWN
} GgcMetricKind;

typedef struct GgcMetricEstimate
{
	/* LOWER_BOUND uses this as a tagged lower bound, never as a mean. */
	double		expected;
	double		workload_p95;
	double		uncertainty_low;
	double		uncertainty_high;
	uint64		sample_count;
	GgcMetricKind kind;
	GgcStatsState state;
	bool		p95_known;
	bool		uncertainty_known;
	bool		censored;
} GgcMetricEstimate;

typedef struct GgcStatsScope
{
	/* Extension OID, or InvalidOid for a built-in catalog provider. */
	Oid			catalog_provider_oid;
	Oid			graph_oid;
	Oid			edge_element_oid;
	Oid			index_relid;
	Oid			effective_role;
	uint32		endpoint_domain;
	uint32		scope_kind;
	uint32		direction;
	uint32		path_mode;
	uint32		result_mode;
	uint64		catalog_generation;
	uint64		security_generation;
	uint64		build_generation;
	uint64		semantic_fingerprint;
} GgcStatsScope;

extern bytea *graph_stats_scope_encode(const GgcStatsScope *scope);
extern void graph_stats_scope_decode(const bytea *encoded, GgcStatsScope *scope);
extern bytea *graph_stats_metric_encode(const GgcMetricEstimate *metric);
extern void graph_stats_metric_decode(const bytea *encoded, GgcMetricEstimate *metric);
extern bool graph_stats_dependencies_current(const GgcStatsScope *scope,
											 const bytea *stored);
extern List *graph_stats_relation_oids(const GgcStatsScope *scope);
extern void graph_stats_check_collect_privileges(const GgcStatsScope *scope);
extern int64 graph_stats_publish_collected(Oid stats_oid, Jsonb *profiles);
extern char *graph_stats_counter_baseline(const GgcStatsScope *scope);
extern GgcStatsState graph_stats_counter_state(Jsonb *baseline, Jsonb *current);
/* Compatibility entry point: max_age_ms is ignored; age is not freshness. */
extern bool graph_stats_counters_unchanged(Jsonb *baseline, Jsonb *current,
										 uint32 max_age_ms);
extern void RemoveGraphStatisticsById(Oid stats_oid);
extern void ReassignGraphStatisticsOwner(Oid stats_oid, Oid new_owner);

/* Optional, explicitly registered singleton population. Absent means the
 * historical uniform-vertex population, never an arbitrary query anchor. */
extern bool graph_stats_anchor_condition(Jsonb *options, int64 *key);

/* Explicit collection requires full SELECT and owner/MAINTAIN on every base
 * table, plus graph SELECT. Raw payload diagnostics remain superuser-only.
 * Consumption matches the full role/security scope and rejects RLS globals.
 * Counters are asynchronous observations, not a committed-change log. No
 * hooks in ANALYZE, pgstat, per-edge DML, or a frontend-specific module. */
#endif
