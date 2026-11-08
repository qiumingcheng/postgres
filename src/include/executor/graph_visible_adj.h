/* Experimental, transaction-scoped visible adjacency delivery.
 * Not the candidate/TID provider ABI. Only qualified endpoint-set WALK plans
 * may use it. The provider certifies BOTH edge and vertex visibility for the
 * exact executor snapshot and security context; absence must be complete.
 * open/visit/visit_batch must ERROR outside their supported scope, never
 * silently retry. Visits are synchronous: the provider must not retain keys,
 * the emit callback or its argument. The consumer cannot execute user code or
 * change transaction, snapshot or security context inside a visit, allowing
 * the provider to validate that scope once per bounded batch. Interrupt and
 * resource-budget checks remain required while delivering the batch.
 */
#ifndef GRAPH_VISIBLE_ADJ_H
#define GRAPH_VISIBLE_ADJ_H
#include "executor/graph_fixed.h"
extern PGDLLIMPORT bool enable_graph_age_adjacency;
#define GGC_VISIBLE_ADJ_ABI 2U
#define GGC_VISIBLE_ADJ_RENDEZVOUS "ggc.experimental.visible_adj.v2"
#define GGC_VISIBLE_ADJ_BATCH_MAX 128U
typedef void (*GgcVisibleEmit)(void *arg, int64 endpoint);
typedef struct GgcVisibleAdjOps
{
	uint32 abi;
	Size struct_size;
	const char *name;
	void *(*open)(EState *estate, const GgcFixedStagePlan *stage);
	void (*visit)(void *context, int64 endpoint, GgcDirection direction,
				  GgcVisibleEmit emit, void *arg);
	/* Optional. Emit in key order, exactly as consecutive scalar visits. */
	void (*visit_batch)(void *context, const int64 *endpoints, uint32 count,
						GgcDirection direction, GgcVisibleEmit emit, void *arg);
} GgcVisibleAdjOps;
#endif
