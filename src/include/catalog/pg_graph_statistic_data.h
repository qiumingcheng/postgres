/* Typed, versioned payloads.  One active generation is published atomically. */
#ifndef PG_GRAPH_STATISTIC_DATA_H
#define PG_GRAPH_STATISTIC_DATA_H

#include "catalog/genbki.h"
#include "catalog/pg_graph_statistic_data_d.h"

BEGIN_CATALOG_STRUCT

CATALOG(pg_graph_statistic_data,8485,GraphStatisticDataRelationId)
{
	Oid			gstdstatid BKI_LOOKUP(pg_graph_statistic);
	int64		gstdgeneration;
	int16		gstdmetric;
	int16		gstdformat;

#ifdef CATALOG_VARLEN
	jsonb		gstdkey BKI_FORCE_NOT_NULL;
	jsonb		gstdmetadata BKI_FORCE_NOT_NULL;
	bytea		gstdpayload BKI_FORCE_NOT_NULL;
#endif
} FormData_pg_graph_statistic_data;

END_CATALOG_STRUCT

typedef FormData_pg_graph_statistic_data *Form_pg_graph_statistic_data;

DECLARE_TOAST(pg_graph_statistic_data, 8487, 8488);
DECLARE_UNIQUE_INDEX_PKEY(pg_graph_statistic_data_profile_index, 8486, GraphStatisticDataProfileIndexId, pg_graph_statistic_data, btree(gstdstatid oid_ops, gstdgeneration int8_ops, gstdmetric int2_ops, gstdkey jsonb_ops));

#endif
