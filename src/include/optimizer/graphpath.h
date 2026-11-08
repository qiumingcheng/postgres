/*-------------------------------------------------------------------------
 * graphpath.h
 *
 * Common admission and construction contract for graph CustomPaths.  A
 * frontend identifies an equivalent relational subproblem; this helper
 * verifies its planner boundaries, applies graph statistics, and constructs
 * an ordinary CustomPath which competes through add_path().
 *-------------------------------------------------------------------------
 */
#ifndef OPTIMIZER_GRAPHPATH_H
#define OPTIMIZER_GRAPHPATH_H

#include "nodes/extensible.h"
#include "optimizer/graphstats.h"

/* Opt-in PGQ candidate generation; explicit routes have separate semantics. */
extern PGDLLIMPORT bool enable_graph_btree;
extern PGDLLIMPORT int graph_btree_max_candidates;

/* Filter before IndexOptInfo/path generation, never after add_path pruning. */
typedef bool (*GgcGraphIndexAllowedHook) (Oid relation_oid, Oid index_oid,
										Oid access_method_oid);
extern PGDLLIMPORT GgcGraphIndexAllowedHook ggc_graph_index_allowed_hook;

/*
 * Optional planning policy for retaining a PGQ region as a subquery.  A true
 * result prevents both pullup and outer qual pushdown; the latter quals still
 * execute in the ordinary SubqueryScan.  This is not a security barrier or a
 * materialization request.  Callers must restore statement-local hooks on
 * ERROR as well as success.  No concrete provider is required by this API.
 */
typedef bool (*GgcGraphSubqueryBoundaryHook) (PlannerInfo *root, Query *subquery);
extern PGDLLIMPORT GgcGraphSubqueryBoundaryHook ggc_graph_subquery_boundary_hook;
extern bool GgcGraphKeepSubqueryBoundary(PlannerInfo *root, Query *subquery);

/* A forced graph frontend may need separate logical vertex bindings even
 * when their key equality would let relational self-join elimination merge
 * the aliases. Automatic/ordinary planning must retain the normal PG rule. */
typedef bool (*GgcGraphKeepBindingsHook) (PlannerInfo *root);
extern PGDLLIMPORT GgcGraphKeepBindingsHook ggc_graph_keep_bindings_hook;
extern bool GgcGraphKeepVariableBindings(PlannerInfo *root);

typedef struct GgcGraphPathRequest
{
	PlannerInfo *root;
	RelOptInfo *joinrel;
	JoinType	jointype;
	SpecialJoinInfo *sjinfo;

	/* Exact semantic boundary represented by the graph path. */
	Relids		covered_relids;
	List	   *available_quals;	/* all internal RestrictInfos at the boundary */
	List	   *consumed_quals;	/* RestrictInfos implemented by the graph path */
	Relids		required_outer;

	/* Executed child and paths used for standard ParamPathInfo derivation. */
	Path	   *anchor_path;
	Path	   *ppi_outer_path;
	Path	   *ppi_inner_path;

	GgcGraphEstimateRequest estimate;
	/* Proven equality on the anchor key, never inferred from estimated rows. */
	bool		has_anchor_key;
	int64		anchor_key;
	List	   *custom_private;
	const CustomPathMethods *methods;
} GgcGraphPathRequest;

typedef enum GgcGraphPathRejectReason
{
	GGC_GRAPH_PATH_ACCEPTED = 0,
	GGC_GRAPH_PATH_NOT_INNER_JOIN,
	GGC_GRAPH_PATH_RELIDS_MISMATCH,
	GGC_GRAPH_PATH_QUALS_MISMATCH,
	GGC_GRAPH_PATH_PARAMETERIZATION_MISMATCH,
	GGC_GRAPH_PATH_INVALID_CHILD,
	GGC_GRAPH_PATH_INVALID_ESTIMATE
} GgcGraphPathRejectReason;

typedef struct GgcGraphPathResult
{
	CustomPath *path;
	GgcGraphPathCost cost;
	GgcGraphPathRejectReason reject_reason;
} GgcGraphPathResult;

extern bool GgcCreateGraphPath(const GgcGraphPathRequest *request,
								MemoryContext planner_context,
								GgcGraphPathResult *result);
extern const char *GgcGraphPathRejectReasonName(GgcGraphPathRejectReason reason);

#endif							/* OPTIMIZER_GRAPHPATH_H */
