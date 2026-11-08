/* Ordinary non-covering endpoint B-tree provider. */
#ifndef GRAPH_BTREE_PROVIDER_H
#define GRAPH_BTREE_PROVIDER_H

#include "executor/graph_provider.h"

#define GGC_BTREE_MAX_ENDPOINTS 1024U
#define GGC_BTREE_MIN_WORK_BYTES (128 * 1024U)

extern const GgcAdjProviderV2 *GetGgcBtreeAdjProvider(void);
extern uint64 GgcBtreeGetArrayScans(const GgcAdjScanV2 *scan);
/* Fresh owned request only; verifies a contiguous set, retains public AM. */
extern bool GgcBtreeSetRangeProbe(GgcAdjScanV2 *scan, bool enabled);

#endif
