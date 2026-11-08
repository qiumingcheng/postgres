/* SQL-only compatibility wrappers. Planning/execution lives in the kernel. */
#include "postgres.h"

#include "executor/graph_pgq.h"
#include "test_graph_provider.h"

void _PG_init(void);
void _PG_fini(void);

static const GgcGraphDiagnosticOps diagnostic_ops = {
	.struct_size = sizeof(GgcGraphDiagnosticOps),
	.version = GGC_GRAPH_DIAGNOSTIC_VERSION,
	.wrap = GetTestWrappedGraphProvider,
	.reset_counters = ResetTestWrappedGraphProviderCounters,
	.get_counters = GetTestWrappedGraphProviderCounters
};

void
_PG_init(void)
{
	GgcRegisterGraphDiagnostics(&diagnostic_ops);
}

void
_PG_fini(void)
{
	GgcUnregisterGraphDiagnostics(&diagnostic_ops);
	UnregisterTestWrappedGraphProvider();
}

#define GRAPH_DIAGNOSTIC_SQL(name, kind) \
	PG_FUNCTION_INFO_V1(name); \
	Datum name(PG_FUNCTION_ARGS) { return GgcGraphDiagnosticCall(kind, fcinfo); }

GRAPH_DIAGNOSTIC_SQL(test_graph_fixed_config, GGC_DIAGNOSTIC_FIXED_CONFIG)
GRAPH_DIAGNOSTIC_SQL(test_graph_fixed_tuning, GGC_DIAGNOSTIC_FIXED_TUNING)
GRAPH_DIAGNOSTIC_SQL(test_graph_fixed_factorize, GGC_DIAGNOSTIC_FIXED_FACTORIZE)
GRAPH_DIAGNOSTIC_SQL(test_graph_fixed_endpoint_set, GGC_DIAGNOSTIC_FIXED_ENDPOINT_SET)
GRAPH_DIAGNOSTIC_SQL(test_graph_lifecycle_config, GGC_DIAGNOSTIC_LIFECYCLE_CONFIG)
GRAPH_DIAGNOSTIC_SQL(test_graph_lifecycle_diagnostic, GGC_DIAGNOSTIC_LIFECYCLE)
GRAPH_DIAGNOSTIC_SQL(test_graph_cbo_config, GGC_DIAGNOSTIC_CBO_CONFIG)
GRAPH_DIAGNOSTIC_SQL(test_graph_cbo_diagnostic, GGC_DIAGNOSTIC_CBO)
