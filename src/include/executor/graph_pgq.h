/* SQL/PGQ backend entry and explicit, backend-local regression bridge. */
#ifndef GRAPH_PGQ_H
#define GRAPH_PGQ_H

#include "executor/graph_provider.h"
#include "fmgr.h"
#include "nodes/pathnodes.h"

extern void GgcInitializeGraphPlanner(void);
extern void GgcGraphJoinRows(PlannerInfo *root, RelOptInfo *rel,
							SpecialJoinInfo *sjinfo);

/* Diagnostic functions are not installed in pg_proc by the kernel. Only the
 * optional test module supplies SQL wrappers and fault-injection callbacks.
 * The core never links to that module; these pointers are never plan data. */
#define GGC_GRAPH_DIAGNOSTIC_VERSION 1U
typedef struct GgcGraphDiagnosticOps
{
	uint32 struct_size;
	uint32 version;
	const GgcAdjProviderV2 *(*wrap) (const GgcAdjProviderV2 *, int, int);
	void (*reset_counters) (void);
	void (*get_counters) (uint64 *, uint64 *, uint64 *, uint64 *, int *);
} GgcGraphDiagnosticOps;

typedef enum GgcGraphDiagnosticCallKind
{
	GGC_DIAGNOSTIC_FIXED_CONFIG,
	GGC_DIAGNOSTIC_FIXED_TUNING,
	GGC_DIAGNOSTIC_FIXED_FACTORIZE,
	GGC_DIAGNOSTIC_FIXED_ENDPOINT_SET,
	GGC_DIAGNOSTIC_LIFECYCLE_CONFIG,
	GGC_DIAGNOSTIC_LIFECYCLE,
	GGC_DIAGNOSTIC_CBO_CONFIG,
	GGC_DIAGNOSTIC_CBO
} GgcGraphDiagnosticCallKind;

extern void GgcRegisterGraphDiagnostics(const GgcGraphDiagnosticOps *ops);
extern void GgcUnregisterGraphDiagnostics(const GgcGraphDiagnosticOps *ops);
extern Datum GgcGraphDiagnosticCall(GgcGraphDiagnosticCallKind kind,
								   FunctionCallInfo fcinfo);

#endif
