/* Standalone Linux/GDB test support; never linked into the server/module. */
#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "access/table.h"
#include "access/visibilitymap.h"
#include "storage/bufmgr.h"
#ifdef __linux__
#include <sys/prctl.h>
#endif

PG_MODULE_MAGIC;
PG_FUNCTION_INFO_V1(graph_vertex_vm_allow_debugger);
PG_FUNCTION_INFO_V1(graph_vertex_vm_status);

Datum
graph_vertex_vm_allow_debugger(PG_FUNCTION_ARGS)
{
	int pid = PG_GETARG_INT32(0);

	if (!superuser())
		elog(ERROR, "graph VM debugger test requires superuser");
	if (pid <= 0)
		elog(ERROR, "a specific debugger PID is required");
#ifdef __linux__
	/* Yama permits only this debugger and its descendants, not arbitrary PIDs.
	 * The test closes this otherwise disposable backend immediately after use. */
	if (prctl(PR_SET_PTRACER, (unsigned long) pid, 0UL, 0UL, 0UL) != 0)
		elog(ERROR, "could not allow graph VM test debugger: %m");
#else
	elog(ERROR, "graph VM debugger test requires Linux");
#endif
	PG_RETURN_VOID();
}

Datum
graph_vertex_vm_status(PG_FUNCTION_ARGS)
{
	Relation rel = table_open(PG_GETARG_OID(0), AccessShareLock);
	BlockNumber block = (BlockNumber) PG_GETARG_INT64(1);
	Buffer buffer = InvalidBuffer;
	bool visible = VM_ALL_VISIBLE(rel, block, &buffer);

	if (BufferIsValid(buffer))
		ReleaseBuffer(buffer);
	table_close(rel, AccessShareLock);
	PG_RETURN_BOOL(visible);
}
