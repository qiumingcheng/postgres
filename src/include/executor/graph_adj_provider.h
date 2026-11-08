/*-------------------------------------------------------------------------
 * graph_adj_provider.h
 *
 * The native graph_adj access method's public bounded-batch provider.
 *-------------------------------------------------------------------------
 */
#ifndef GRAPH_ADJ_PROVIDER_H
#define GRAPH_ADJ_PROVIDER_H

#include "executor/graph_provider.h"

extern const GgcAdjProviderV2 *GetGgcGraphAdjProvider(void);

#endif /* GRAPH_ADJ_PROVIDER_H */
