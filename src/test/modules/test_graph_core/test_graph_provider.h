/* Shared diagnostics: select an installed AM, never a concrete provider. */
#ifndef TEST_GRAPH_PROVIDER_H
#define TEST_GRAPH_PROVIDER_H

#include "executor/graph_provider.h"

extern const GgcAdjProviderV2 *GetTestIndexProvider(Oid indexid);
extern const GgcAdjProviderV2 *GetTestWrappedGraphProvider(
	const GgcAdjProviderV2 *inner, int mode, int pause_ms);
extern void UnregisterTestWrappedGraphProvider(void);
extern void ResetTestWrappedGraphProviderCounters(void);
extern void GetTestWrappedGraphProviderCounters(uint64 *validate_calls,
	uint64 *begin_calls, uint64 *getnext_calls, uint64 *end_calls, int *active);

#endif
