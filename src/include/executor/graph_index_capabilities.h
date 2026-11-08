/*-------------------------------------------------------------------------
 * graph_index_capabilities.h
 *    Access-shape contracts shared by graph costing and execution.
 *
 * This is deliberately independent of a concrete graph provider.  Native
 * PostgreSQL B-tree supports more array shapes; the graph reader currently
 * restores request-major delivery only for these ordered single-key scans.
 *-------------------------------------------------------------------------
 */
#ifndef GRAPH_INDEX_CAPABILITIES_H
#define GRAPH_INDEX_CAPABILITIES_H

static inline bool
GgcBtreeArrayIndexEligible(int nkeycolumns, bool descending)
{
	return nkeycolumns == 1 && !descending;
}

#endif							/* GRAPH_INDEX_CAPABILITIES_H */
