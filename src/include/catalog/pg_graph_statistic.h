/* Independent graph statistics definitions; data lives in a separate catalog. */
#ifndef PG_GRAPH_STATISTIC_H
#define PG_GRAPH_STATISTIC_H

#include "catalog/genbki.h"
#include "catalog/pg_graph_statistic_d.h"

BEGIN_CATALOG_STRUCT

CATALOG(pg_graph_statistic,8480,GraphStatisticRelationId)
{
	Oid			oid;
	Oid			gstowner BKI_LOOKUP(pg_authid);
	int16		gstkind;
	Oid			gstprovider BKI_LOOKUP_OPT(pg_extension);
	Oid			gstgraphid BKI_LOOKUP_OPT(pg_class);
	Oid			gstedgeid BKI_LOOKUP_OPT(pg_propgraph_element);
	Oid			gstindexid BKI_LOOKUP_OPT(pg_class);
	Oid			gstrole BKI_LOOKUP(pg_authid);
	int64		gstgeneration;

#ifdef CATALOG_VARLEN
	bytea		gstscope BKI_FORCE_NOT_NULL;
	bytea		gstdependencies BKI_FORCE_NOT_NULL;
	jsonb		gstoptions BKI_FORCE_NOT_NULL;
#endif
} FormData_pg_graph_statistic;

END_CATALOG_STRUCT

typedef FormData_pg_graph_statistic *Form_pg_graph_statistic;

DECLARE_TOAST(pg_graph_statistic, 8483, 8484);
DECLARE_UNIQUE_INDEX_PKEY(pg_graph_statistic_oid_index, 8481, GraphStatisticOidIndexId, pg_graph_statistic, btree(oid oid_ops));
DECLARE_UNIQUE_INDEX(pg_graph_statistic_scope_index, 8482, GraphStatisticScopeIndexId, pg_graph_statistic, btree(gstscope bytea_ops));
MAKE_SYSCACHE(GRAPHSTATISTICOID, pg_graph_statistic_oid_index, 4);

#endif
