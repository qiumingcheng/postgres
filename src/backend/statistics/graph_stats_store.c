/*-------------------------------------------------------------------------
 * graph_stats_store.c
 *   Independent graph statistics, typed codecs and transactional publication.
 *
 * All maintenance is explicit.  This module neither changes column statistics
 * nor runs graph scans in a planner or ANALYZE callback.
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/detoast.h"
#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_class.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_extension.h"
#include "catalog/pg_graph_statistic.h"
#include "catalog/pg_graph_statistic_data.h"
#include "catalog/pg_propgraph_element.h"
#include "catalog/pg_type.h"
#include "fmgr.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "statistics/graph_stats.h"
#include "storage/lmgr.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"

#define SCOPE_MAGIC 0x47475353U
#define METRIC_MAGIC 0x47475331U
#define DEPENDENCY_MAGIC 0x47475344U
#define SCOPE_BYTES 80
#define METRIC_BYTES 52

typedef struct ScopeField
{
	const char *name;
	Size		offset;
	bool		wide;
} ScopeField;

#define SCOPE32(field) {#field, offsetof(GgcStatsScope, field), false}
#define SCOPE64(field) {#field, offsetof(GgcStatsScope, field), true}
static const ScopeField scope_fields[] = {
	SCOPE32(catalog_provider_oid), SCOPE32(graph_oid),
	SCOPE32(edge_element_oid), SCOPE32(index_relid), SCOPE32(effective_role),
	SCOPE32(endpoint_domain), SCOPE32(scope_kind), SCOPE32(direction),
	SCOPE32(path_mode), SCOPE32(result_mode), SCOPE64(catalog_generation),
	SCOPE64(security_generation), SCOPE64(build_generation),
	SCOPE64(semantic_fingerprint)
};
#undef SCOPE32
#undef SCOPE64

typedef struct GraphStatsProfile
{
	int16		metric_kind;
	Jsonb	   *key;
	Jsonb	   *metadata;
	bytea	   *payload;
} GraphStatsProfile;

PG_FUNCTION_INFO_V1(pg_graph_statistic_scope);
PG_FUNCTION_INFO_V1(pg_graph_statistic_create);
PG_FUNCTION_INFO_V1(pg_graph_statistic_publish);
PG_FUNCTION_INFO_V1(pg_graph_statistic_read);
PG_FUNCTION_INFO_V1(pg_graph_statistic_drop);

static void
require_maintenance_privilege(void)
{
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("raw graph statistics diagnostics require superuser")));
}

static JsonbValue *
field(Jsonb *object, const char *name)
{
	JsonbValue key;
	JsonbValue *value;

	if (!JB_ROOT_IS_OBJECT(object))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("graph statistics value must be a JSON object")));
	key.type = jbvString;
	key.val.string.val = (char *) name;
	key.val.string.len = strlen(name);
	value = findJsonbValueFromContainer(&object->root, JB_FOBJECT, &key);
	if (value == NULL)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("missing graph statistics field \"%s\"", name)));
	return value;
}

static uint64
integer_field(Jsonb *object, const char *name, uint64 maximum)
{
	JsonbValue *value = field(object, name);
	int64		number;
	Numeric		integral;

	if (value->type != jbvNumeric)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("graph statistics field \"%s\" must be an integer", name)));
	number = DatumGetInt64(DirectFunctionCall1(numeric_int8,
											 NumericGetDatum(value->val.numeric)));
	integral = DatumGetNumeric(DirectFunctionCall1(int8_numeric, Int64GetDatum(number)));
	if (number < 0 || (uint64) number > maximum ||
		DatumGetInt32(DirectFunctionCall2(numeric_cmp,
									 NumericGetDatum(value->val.numeric),
									 NumericGetDatum(integral))) != 0)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("graph statistics integer \"%s\" is out of range", name)));
	return (uint64) number;
}

static bool
boolean_field(Jsonb *object, const char *name)
{
	JsonbValue *value = field(object, name);

	if (value->type != jbvBool)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("graph statistics field \"%s\" must be boolean", name)));
	return value->val.boolean;
}

static double
number_field(Jsonb *object, const char *name, bool known)
{
	JsonbValue *value = field(object, name);
	double		number;

	if (!known)
	{
		if (value->type != jbvNull)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							errmsg("unknown graph metric \"%s\" must be null", name)));
		return 0;
	}
	if (value->type != jbvNumeric)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("known graph metric \"%s\" must be numeric", name)));
	number = DatumGetFloat8(DirectFunctionCall1(numeric_float8,
											  NumericGetDatum(value->val.numeric)));
	if (!isfinite(number) || number < 0)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("graph metric \"%s\" must be finite and nonnegative", name)));
	return number;
}

static Jsonb *
object_field(Jsonb *object, const char *name)
{
	JsonbValue *value = field(object, name);
	Jsonb	   *result = JsonbValueToJsonb(value);

	if (!JB_ROOT_IS_OBJECT(result))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("graph statistics field \"%s\" must be an object", name)));
	return result;
}

static char *
string_field(Jsonb *object, const char *name)
{
	JsonbValue *value = field(object, name);

	if (value->type != jbvString || value->val.string.len == 0 ||
		value->val.string.len > 256)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("graph statistics field \"%s\" must be a bounded nonempty string", name)));
	return pnstrdup(value->val.string.val, value->val.string.len);
}

bool
graph_stats_anchor_condition(Jsonb *options, int64 *key)
{
	JsonbValue name;
	JsonbValue *value;
	Jsonb *condition;
	Numeric integral;

	if (!JB_ROOT_IS_OBJECT(options))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("graph statistics options must be an object")));
	name.type = jbvString;
	name.val.string.val = "anchor_condition";
	name.val.string.len = strlen(name.val.string.val);
	value = findJsonbValueFromContainer(&options->root, JB_FOBJECT, &name);
	if (value == NULL)
		return false;
	condition = JsonbValueToJsonb(value);
	if (!JB_ROOT_IS_OBJECT(condition) || JB_ROOT_COUNT(condition) != 2 ||
		integer_field(condition, "version", PG_UINT32_MAX) != 1)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("unsupported graph statistics anchor condition")));
	value = field(condition, "key");
	if (value->type != jbvNumeric)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("graph statistics anchor key must be a bigint")));
	*key = DatumGetInt64(DirectFunctionCall1(numeric_int8,
		NumericGetDatum(value->val.numeric)));
	integral = DatumGetNumeric(DirectFunctionCall1(int8_numeric, Int64GetDatum(*key)));
	if (DatumGetInt32(DirectFunctionCall2(numeric_cmp,
		NumericGetDatum(value->val.numeric), NumericGetDatum(integral))) != 0)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("graph statistics anchor key must be integral")));
	return true;
}

static void
validate_scope(const GgcStatsScope *scope)
{
	if (!OidIsValid(scope->effective_role) ||
		scope->scope_kind < GGC_STATS_LOGICAL || scope->scope_kind > GGC_STATS_COST ||
		scope->catalog_generation > PG_INT64_MAX ||
		scope->security_generation > PG_INT64_MAX ||
		scope->build_generation > PG_INT64_MAX ||
		scope->semantic_fingerprint > PG_INT64_MAX)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("invalid graph statistics scope/version")));
	if (scope->scope_kind == GGC_STATS_COST)
	{
		if (scope->graph_oid || scope->edge_element_oid || scope->index_relid ||
			scope->build_generation || scope->direction || scope->endpoint_domain)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							errmsg("cost statistics scope cannot name an edge or index build")));
		return;
	}
	if (!scope->graph_oid || !scope->edge_element_oid || !scope->endpoint_domain ||
		(scope->direction != 1 && scope->direction != 2) ||
		scope->path_mode < 1 || scope->path_mode > 4)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("invalid graph statistics topology scope")));
	if ((scope->scope_kind == GGC_STATS_LOGICAL &&
		 (scope->index_relid || scope->build_generation)) ||
		(scope->scope_kind == GGC_STATS_PHYSICAL &&
		 (!scope->index_relid || !scope->build_generation)))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("logical and physical graph statistics scope generations are distinct")));
}

static void
scope_from_json(Jsonb *object, GgcStatsScope *scope)
{
	int			i;

	if (!JB_ROOT_IS_OBJECT(object) || JB_ROOT_COUNT(object) != lengthof(scope_fields))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("graph statistics scope requires exactly its versioned fields")));
	MemSet(scope, 0, sizeof(*scope));
	for (i = 0; i < lengthof(scope_fields); i++)
	{
		const ScopeField *f = &scope_fields[i];
		uint64		value = integer_field(object, f->name,
										 f->wide ? PG_INT64_MAX : PG_UINT32_MAX);

		if (f->wide)
			*((uint64 *) ((char *) scope + f->offset)) = value;
		else
			*((uint32 *) ((char *) scope + f->offset)) = (uint32) value;
	}
	validate_scope(scope);
}

static Jsonb *
scope_to_json(const GgcStatsScope *scope)
{
	StringInfoData result;
	int			i;

	initStringInfo(&result);
	appendStringInfoChar(&result, '{');
	for (i = 0; i < lengthof(scope_fields); i++)
	{
		const ScopeField *f = &scope_fields[i];
		uint64		value = f->wide ?
			*((const uint64 *) ((const char *) scope + f->offset)) :
			*((const uint32 *) ((const char *) scope + f->offset));

		appendStringInfo(&result, "%s\"%s\":" UINT64_FORMAT,
						 i ? "," : "", f->name, value);
	}
	appendStringInfoChar(&result, '}');
	return DatumGetJsonbP(DirectFunctionCall1(jsonb_in, CStringGetDatum(result.data)));
}

bytea *
graph_stats_scope_encode(const GgcStatsScope *scope)
{
	StringInfoData buf;
	int			i;

	validate_scope(scope);
	pq_begintypsend(&buf);
	pq_sendint32(&buf, SCOPE_MAGIC);
	pq_sendint16(&buf, GGC_STATS_FORMAT_VERSION);
	pq_sendint16(&buf, SCOPE_BYTES);
	for (i = 0; i < lengthof(scope_fields); i++)
	{
		const ScopeField *f = &scope_fields[i];

		if (f->wide)
			pq_sendint64(&buf, *((const uint64 *) ((const char *) scope + f->offset)));
		else
			pq_sendint32(&buf, *((const uint32 *) ((const char *) scope + f->offset)));
	}
	return pq_endtypsend(&buf);
}

static StringInfoData
codec_buffer(const bytea *encoded, uint32 magic, int length)
{
	StringInfoData buf;

	MemSet(&buf, 0, sizeof(buf));
	buf.data = VARDATA_ANY(encoded);
	buf.len = VARSIZE_ANY_EXHDR(encoded);
	if (buf.len != length || (uint32) pq_getmsgint(&buf, 4) != magic ||
		pq_getmsgint(&buf, 2) != GGC_STATS_FORMAT_VERSION ||
		pq_getmsgint(&buf, 2) != length)
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
						errmsg("invalid graph statistics payload format"),
						errhint("Recollect the graph statistics.")));
	return buf;
}

void
graph_stats_scope_decode(const bytea *encoded, GgcStatsScope *scope)
{
	StringInfoData buf = codec_buffer(encoded, SCOPE_MAGIC, SCOPE_BYTES);
	int			i;

	MemSet(scope, 0, sizeof(*scope));
	for (i = 0; i < lengthof(scope_fields); i++)
	{
		const ScopeField *f = &scope_fields[i];

		if (f->wide)
			*((uint64 *) ((char *) scope + f->offset)) = pq_getmsgint64(&buf);
		else
			*((uint32 *) ((char *) scope + f->offset)) = pq_getmsgint(&buf, 4);
	}
	pq_getmsgend(&buf);
	validate_scope(scope);
}

static void
validate_metric(const GgcMetricEstimate *metric)
{
	if (metric->kind < GGC_METRIC_EXACT_AT_SAMPLE || metric->kind > GGC_METRIC_UNKNOWN ||
		metric->state < GGC_STATS_ABSENT || metric->state > GGC_STATS_INVALID ||
		metric->sample_count > PG_INT64_MAX ||
		!isfinite(metric->expected) || metric->expected < 0 ||
		!isfinite(metric->workload_p95) || metric->workload_p95 < 0 ||
		!isfinite(metric->uncertainty_low) || metric->uncertainty_low < 0 ||
		!isfinite(metric->uncertainty_high) || metric->uncertainty_high < 0 ||
		(metric->uncertainty_known && metric->uncertainty_low > metric->uncertainty_high) ||
		(metric->kind == GGC_METRIC_UNKNOWN &&
		 (metric->state == GGC_STATS_READY || metric->expected != 0 ||
		  metric->p95_known || metric->uncertainty_known)) ||
		(metric->kind == GGC_METRIC_LOWER_BOUND &&
		 (!metric->censored || metric->state != GGC_STATS_PARTIAL ||
		  metric->p95_known || metric->uncertainty_known)) ||
		(metric->censored && metric->kind != GGC_METRIC_LOWER_BOUND &&
		 metric->kind != GGC_METRIC_UNKNOWN) ||
		(!metric->p95_known && metric->workload_p95 != 0) ||
		(!metric->uncertainty_known &&
		 (metric->uncertainty_low != 0 || metric->uncertainty_high != 0)))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("invalid or contradictory graph metric estimate")));
}

static void
metric_from_json(Jsonb *object, GgcMetricEstimate *metric)
{
	MemSet(metric, 0, sizeof(*metric));
	metric->kind = integer_field(object, "kind", GGC_METRIC_UNKNOWN);
	metric->state = integer_field(object, "state", GGC_STATS_INVALID);
	metric->sample_count = integer_field(object, "sample_count", PG_INT64_MAX);
	metric->p95_known = boolean_field(object, "p95_known");
	metric->uncertainty_known = boolean_field(object, "uncertainty_known");
	metric->censored = boolean_field(object, "censored");
	metric->expected = number_field(object, "expected",
									metric->kind != GGC_METRIC_UNKNOWN &&
									metric->kind != GGC_METRIC_LOWER_BOUND);
	if (metric->kind == GGC_METRIC_LOWER_BOUND)
		metric->expected = number_field(object, "lower_bound", true);
	metric->workload_p95 = number_field(object, "workload_p95", metric->p95_known);
	metric->uncertainty_low = number_field(object, "uncertainty_low", metric->uncertainty_known);
	metric->uncertainty_high = number_field(object, "uncertainty_high", metric->uncertainty_known);
	validate_metric(metric);
}

bytea *
graph_stats_metric_encode(const GgcMetricEstimate *metric)
{
	StringInfoData buf;
	uint8		flags = (metric->p95_known ? 1 : 0) |
		(metric->uncertainty_known ? 2 : 0) | (metric->censored ? 4 : 0);

	validate_metric(metric);
	pq_begintypsend(&buf);
	pq_sendint32(&buf, METRIC_MAGIC);
	pq_sendint16(&buf, GGC_STATS_FORMAT_VERSION);
	pq_sendint16(&buf, METRIC_BYTES);
	pq_sendint8(&buf, metric->kind);
	pq_sendint8(&buf, metric->state);
	pq_sendint8(&buf, flags);
	pq_sendint8(&buf, 0);
	pq_sendfloat8(&buf, metric->expected);
	pq_sendfloat8(&buf, metric->workload_p95);
	pq_sendfloat8(&buf, metric->uncertainty_low);
	pq_sendfloat8(&buf, metric->uncertainty_high);
	pq_sendint64(&buf, metric->sample_count);
	return pq_endtypsend(&buf);
}

void
graph_stats_metric_decode(const bytea *encoded, GgcMetricEstimate *metric)
{
	StringInfoData buf = codec_buffer(encoded, METRIC_MAGIC, METRIC_BYTES);
	uint8		flags;

	MemSet(metric, 0, sizeof(*metric));
	metric->kind = pq_getmsgbyte(&buf);
	metric->state = pq_getmsgbyte(&buf);
	flags = pq_getmsgbyte(&buf);
	if ((flags & ~7) != 0 || pq_getmsgbyte(&buf) != 0)
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
						errmsg("invalid graph metric payload flags")));
	metric->p95_known = (flags & 1) != 0;
	metric->uncertainty_known = (flags & 2) != 0;
	metric->censored = (flags & 4) != 0;
	metric->expected = pq_getmsgfloat8(&buf);
	metric->workload_p95 = pq_getmsgfloat8(&buf);
	metric->uncertainty_low = pq_getmsgfloat8(&buf);
	metric->uncertainty_high = pq_getmsgfloat8(&buf);
	metric->sample_count = pq_getmsgint64(&buf);
	pq_getmsgend(&buf);
	validate_metric(metric);
}

static Jsonb *
metric_to_json(const GgcMetricEstimate *metric)
{
	StringInfoData result;
	bool		mean_known = metric->kind != GGC_METRIC_UNKNOWN &&
		metric->kind != GGC_METRIC_LOWER_BOUND;

	initStringInfo(&result);
	appendStringInfo(&result, "{\"kind\":%d,\"state\":%d,\"sample_count\":" UINT64_FORMAT
					 ",\"p95_known\":%s,\"uncertainty_known\":%s,\"censored\":%s,\"expected\":",
					 metric->kind, metric->state, metric->sample_count,
					 metric->p95_known ? "true" : "false",
					 metric->uncertainty_known ? "true" : "false",
					 metric->censored ? "true" : "false");
	if (mean_known)
		appendStringInfo(&result, "%.17g", metric->expected);
	else
		appendStringInfoString(&result, "null");
	if (metric->kind == GGC_METRIC_LOWER_BOUND)
		appendStringInfo(&result, ",\"lower_bound\":%.17g", metric->expected);
	appendStringInfoString(&result, ",\"workload_p95\":");
	if (metric->p95_known)
		appendStringInfo(&result, "%.17g", metric->workload_p95);
	else
		appendStringInfoString(&result, "null");
	appendStringInfoString(&result, ",\"uncertainty_low\":");
	if (metric->uncertainty_known)
		appendStringInfo(&result, "%.17g", metric->uncertainty_low);
	else
		appendStringInfoString(&result, "null");
	appendStringInfoString(&result, ",\"uncertainty_high\":");
	if (metric->uncertainty_known)
		appendStringInfo(&result, "%.17g", metric->uncertainty_high);
	else
		appendStringInfoString(&result, "null");
	appendStringInfoChar(&result, '}');
	return DatumGetJsonbP(DirectFunctionCall1(jsonb_in, CStringGetDatum(result.data)));
}

/* Resolve only catalog mapping, never graph data. */
static List *
scope_relations(const GgcStatsScope *scope, List **elements)
{
	List	   *relations = NIL;
	HeapTuple	tuple;
	Form_pg_propgraph_element edge;
	Oid			vertex_ids[2];
	Oid			edge_relid;
	int			i;

	*elements = NIL;
	if (scope->scope_kind == GGC_STATS_COST)
		return NIL;
	tuple = SearchSysCache1(PROPGRAPHELOID, ObjectIdGetDatum(scope->edge_element_oid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						errmsg("graph statistics edge mapping no longer exists")));
	edge = (Form_pg_propgraph_element) GETSTRUCT(tuple);
	if (edge->pgepgid != scope->graph_oid || edge->pgekind != 'e')
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("graph statistics scope does not match its edge mapping")));
	relations = list_make2_oid(scope->graph_oid, edge->pgerelid);
	edge_relid = edge->pgerelid;
	*elements = list_make1_oid(scope->edge_element_oid);
	vertex_ids[0] = edge->pgesrcvertexid;
	vertex_ids[1] = edge->pgedestvertexid;
	ReleaseSysCache(tuple);
	for (i = 0; i < 2; i++)
	{
		tuple = SearchSysCache1(PROPGRAPHELOID, ObjectIdGetDatum(vertex_ids[i]));
		if (!HeapTupleIsValid(tuple))
			ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
							errmsg("graph statistics vertex mapping no longer exists")));
		if (((Form_pg_propgraph_element) GETSTRUCT(tuple))->pgepgid != scope->graph_oid ||
			((Form_pg_propgraph_element) GETSTRUCT(tuple))->pgekind != PGEKIND_VERTEX)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							errmsg("graph statistics scope does not match its vertex mapping")));
		relations = list_append_unique_oid(relations,
										  ((Form_pg_propgraph_element) GETSTRUCT(tuple))->pgerelid);
		*elements = list_append_unique_oid(*elements, vertex_ids[i]);
		ReleaseSysCache(tuple);
	}
	if (scope->index_relid)
	{
		if (IndexGetRelation(scope->index_relid, false) != edge_relid)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							errmsg("physical graph statistics index belongs to another edge table")));
		relations = list_append_unique_oid(relations, scope->index_relid);
	}
	list_sort(relations, list_oid_cmp);
	list_sort(*elements, list_oid_cmp);
	return relations;
}

List *
graph_stats_relation_oids(const GgcStatsScope *scope)
{
	List *elements;
	List *relations = scope_relations(scope, &elements);

	list_free(elements);
	return relations;
}

/* An index has no independent SELECT ACL. Its base table supplies access.
 * Never use owner/superuser RLS bypass to claim a globally visible sample.
 * The collectors scan these exact heaps, not inheritance/partition trees. */
static bool
scope_access_allowed(const GgcStatsScope *scope, bool maintenance)
{
	List *relations;
	bool allowed = true;

	if (scope->effective_role != GetUserId())
		return false;
	if (scope->scope_kind == GGC_STATS_COST)
		return !maintenance || superuser();
	relations = graph_stats_relation_oids(scope);
	foreach_oid(relid, relations)
	{
		Relation rel = relation_open(relid, AccessShareLock);

		if (relid == scope->index_relid)
		{
			allowed = rel->rd_rel->relkind == RELKIND_INDEX &&
				rel->rd_index->indisvalid && rel->rd_index->indisready;
		}
		else
		{
			allowed = !rel->rd_rel->relrowsecurity &&
				pg_class_aclcheck(relid, GetUserId(), ACL_SELECT) == ACLCHECK_OK;
			if (relid == scope->graph_oid)
				allowed = allowed && rel->rd_rel->relkind == RELKIND_PROPGRAPH;
			else
				allowed = allowed && rel->rd_rel->relkind == RELKIND_RELATION &&
					rel->rd_rel->relam == HEAP_TABLE_AM_OID &&
					rel->rd_rel->relpersistence == RELPERSISTENCE_PERMANENT &&
					!rel->rd_rel->relhassubclass &&
					(!maintenance ||
					 object_ownercheck(RelationRelationId, relid, GetUserId()) ||
					 pg_class_aclcheck(relid, GetUserId(), ACL_MAINTAIN) == ACLCHECK_OK);
		}
		relation_close(rel, NoLock);
		if (!allowed)
			break;
	}
	list_free(relations);
	return allowed;
}

void
graph_stats_check_collect_privileges(const GgcStatsScope *scope)
{
	if (!scope_access_allowed(scope, true))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to maintain graph statistics for this scope"),
				 errdetail("The collection role must match the current role, with graph SELECT and full SELECT plus ownership or MAINTAIN on every base table. Only logged heaps without RLS or inheritance are supported.")));
}

/* Observe existing pgstat only: no flush/reset or per-edge update hooks.
 * Counters are asynchronous hints, not an MVCC change log. Missing entries
 * are explicit and must never be interpreted as a zero-change baseline.
 */
char *
graph_stats_counter_baseline(const GgcStatsScope *scope)
{
	List	   *elements;
	List	   *relations = scope_relations(scope, &elements);
	StringInfoData output;
	bool		first = true;

	initStringInfo(&output);
	appendStringInfo(&output,
		"{\"version\":2,\"sampled_at_us\":" INT64_FORMAT ",\"relations\":[",
		GetCurrentTimestamp());
	foreach_oid(relid, relations)
	{
		HeapTuple tuple = SearchSysCache1(RELOID, ObjectIdGetDatum(relid));
		PgStat_StatTabEntry *stats;
		PgStat_TableStatus *pending;
		RelFileNumber filenode;
		char kind;

		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for relation %u", relid);
		kind = ((Form_pg_class) GETSTRUCT(tuple))->relkind;
		filenode = ((Form_pg_class) GETSTRUCT(tuple))->relfilenode;
		ReleaseSysCache(tuple);
		if (kind != RELKIND_RELATION && kind != RELKIND_PARTITIONED_TABLE)
			continue;
		stats = pgstat_fetch_stat_tabentry(relid);
		/* Include this backend's unflushed and active-subtransaction DML.
		 * Reads performed by collection do not change these counters. Keeping
		 * sums (rather than separate pending fields) avoids invalidation solely
		 * because this backend flushed between collection and planning.
		 * Respect stats_fetch_consistency; do not clear a user's stats snapshot.
		 * Other backends' unflushed writes remain an explicit pgstat limitation. */
		pending = find_tabstat_entry(relid);
		appendStringInfo(&output, "%s{\"relid\":%u,\"relfilenode\":%u,\"available\":%s",
			first ? "" : ",", relid, filenode,
			stats && pgstat_track_counts ? "true" : "false");
		first = false;
		if (stats)
			appendStringInfo(&output,
				",\"inserted\":" INT64_FORMAT ",\"updated\":" INT64_FORMAT
				",\"deleted\":" INT64_FORMAT
				",\"reset_at_us\":" INT64_FORMAT ",\"analyze_at_us\":" INT64_FORMAT
				",\"autoanalyze_at_us\":" INT64_FORMAT,
				stats->tuples_inserted + (pending ? pending->counts.tuples_inserted : 0),
				stats->tuples_updated + (pending ? pending->counts.tuples_updated : 0),
				stats->tuples_deleted + (pending ? pending->counts.tuples_deleted : 0),
				stats->stat_reset_time,
				stats->last_analyze_time, stats->last_autoanalyze_time);
		if (pending)
			pfree(pending);
		appendStringInfoChar(&output, '}');
	}
	appendStringInfoString(&output, "]}");
	list_free(elements);
	list_free(relations);
	return output.data;
}

GgcStatsState
graph_stats_counter_state(Jsonb *baseline, Jsonb *current)
{
	JsonbIterator *iterator;
	JsonbValue value;
	JsonbIteratorToken token;
	int			available = 0;
	JsonbValue version_key;
	JsonbValue *version;

	if (!baseline || !JB_ROOT_IS_OBJECT(baseline) ||
		!current || !JB_ROOT_IS_OBJECT(current))
		return GGC_STATS_PARTIAL;
	version_key.type = jbvString;
	version_key.val.string.val = "version";
	version_key.val.string.len = strlen("version");
	/* V1 did not reconcile pending DML. It cannot establish freshness. */
	for (int i = 0; i < 2; i++)
	{
		Jsonb *counters = i == 0 ? baseline : current;

		version = findJsonbValueFromContainer(&counters->root, JB_FOBJECT,
			&version_key);
		if (!version || version->type != jbvNumeric ||
			DatumGetInt32(DirectFunctionCall2(numeric_cmp,
				NumericGetDatum(version->val.numeric),
				DirectFunctionCall1(int4_numeric, Int32GetDatum(2)))) != 0)
			return GGC_STATS_PARTIAL;
	}
	/* Equality of two missing counter sets does not establish freshness. */
	for (int i = 0; i < 2; i++)
	{
		available = 0;
		iterator = JsonbIteratorInit(i == 0 ? &baseline->root : &current->root);
		while ((token = JsonbIteratorNext(&iterator, &value, false)) != WJB_DONE)
		{
			if (token == WJB_KEY && value.val.string.len == 9 &&
				memcmp(value.val.string.val, "available", 9) == 0)
			{
				(void) JsonbIteratorNext(&iterator, &value, false);
				if (value.type != jbvBool || !value.val.boolean)
					return GGC_STATS_PARTIAL;
				available++;
			}
		}
		if (available == 0)
			return GGC_STATS_PARTIAL;
	}
	return DatumGetBool(DirectFunctionCall2(jsonb_eq,
		DirectFunctionCall2(jsonb_delete, JsonbPGetDatum(baseline),
			CStringGetTextDatum("sampled_at_us")),
		DirectFunctionCall2(jsonb_delete, JsonbPGetDatum(current),
			CStringGetTextDatum("sampled_at_us")))) ? GGC_STATS_READY : GGC_STATS_STALE;
}

bool
graph_stats_counters_unchanged(Jsonb *baseline, Jsonb *current, uint32 max_age_ms)
{
	/* Retained for existing diagnostics. Elapsed time is not evidence of DML. */
	return graph_stats_counter_state(baseline, current) == GGC_STATS_READY;
}

static void
record_reference(Oid stats_oid, Oid classid, Oid objectid, int32 subid,
				 DependencyType dependency_type)
{
	ObjectAddress myself;
	ObjectAddress referenced;

	ObjectAddressSet(myself, GraphStatisticRelationId, stats_oid);
	ObjectAddressSubSet(referenced, classid, objectid, subid);
	recordDependencyOn(&myself, &referenced, dependency_type);
}

/* Store a canonical dependency description, with fixed-order relation locks.
 * Ordinary DML remains possible.  Logical scopes ignore relfilenumber, while
 * physical scopes validate it for both heap and index (including REINDEX).
 * We deliberately reject global RLS profiles until per-role collection exists.
 */
static bytea *
capture_dependencies(const GgcStatsScope *scope, Oid new_stats_oid)
{
	StringInfoData buf;
	List	   *elements;
	List	   *relations = scope_relations(scope, &elements);
	ListCell   *cell;
	Relation	mapping;

	pq_begintypsend(&buf);
	pq_sendint32(&buf, DEPENDENCY_MAGIC);
	pq_sendint16(&buf, GGC_STATS_FORMAT_VERSION);
	pq_sendint16(&buf, list_length(relations));
	foreach(cell, relations)
	{
		Oid			relid = lfirst_oid(cell);
		Relation	relation = relation_open(relid, AccessShareLock);
		TupleDesc	descriptor = RelationGetDescr(relation);
		int			i;

		CHECK_FOR_INTERRUPTS();
		if (relid == scope->graph_oid && relation->rd_rel->relkind != RELKIND_PROPGRAPH)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							errmsg("graph statistics scope must name a property graph")));
		if (relation->rd_rel->relrowsecurity)
			ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
							errmsg("global graph statistics cannot be used for an RLS relation")));
		if (relid == scope->index_relid &&
			(relation->rd_rel->relkind != RELKIND_INDEX ||
			 !relation->rd_index->indisvalid || !relation->rd_index->indisready))
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							errmsg("physical graph statistics require a valid index")));
		if (relid != scope->index_relid &&
			pg_class_aclcheck(relid, GetUserId(), ACL_SELECT) != ACLCHECK_OK)
			aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLE, RelationGetRelationName(relation));
		pq_sendint32(&buf, relid);
		pq_sendint8(&buf, relation->rd_rel->relkind);
		pq_sendint32(&buf, relation->rd_rel->relowner);
		pq_sendint32(&buf, scope->scope_kind == GGC_STATS_PHYSICAL ? relation->rd_locator.relNumber : 0);
		pq_sendint16(&buf, descriptor->natts);
		if (OidIsValid(new_stats_oid))
			record_reference(new_stats_oid, RelationRelationId, relid, 0, DEPENDENCY_AUTO);
		for (i = 0; i < descriptor->natts; i++)
		{
			Form_pg_attribute attr = TupleDescAttr(descriptor, i);

			CHECK_FOR_INTERRUPTS();
			pq_sendint16(&buf, attr->attnum);
			pq_sendint32(&buf, attr->atttypid);
			pq_sendint32(&buf, attr->atttypmod);
			pq_sendint32(&buf, attr->attcollation);
			pq_sendint8(&buf, attr->attisdropped);
			pq_sendint8(&buf, attr->attnotnull);
			pq_sendstring(&buf, NameStr(attr->attname));
			if (buf.len > GGC_STATS_MAX_METADATA_BYTES)
				ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
								errmsg("graph statistics dependency description exceeds its budget")));
			if (new_stats_oid && !attr->attisdropped)
			{
				record_reference(new_stats_oid, RelationRelationId, relid, attr->attnum, DEPENDENCY_AUTO);
				record_reference(new_stats_oid, TypeRelationId, attr->atttypid, 0, DEPENDENCY_NORMAL);
				if (attr->attcollation)
					record_reference(new_stats_oid, CollationRelationId, attr->attcollation, 0, DEPENDENCY_NORMAL);
			}
		}
		relation_close(relation, NoLock);
	}

	/* Exact mapping bytes, not just a fingerprint or caller-supplied generation. */
	mapping = table_open(PropgraphElementRelationId, AccessShareLock);
	pq_sendint16(&buf, list_length(elements));
	foreach(cell, elements)
	{
		Oid			element = lfirst_oid(cell);
		HeapTuple	tuple = SearchSysCache1(PROPGRAPHELOID, ObjectIdGetDatum(element));
		Form_pg_propgraph_element form;
		int			attnum;

		if (!HeapTupleIsValid(tuple))
			ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
							errmsg("graph statistics mapping changed during publication")));
		form = (Form_pg_propgraph_element) GETSTRUCT(tuple);
		pq_sendint32(&buf, element);
		pq_sendint32(&buf, form->pgepgid);
		pq_sendint32(&buf, form->pgerelid);
		pq_sendstring(&buf, NameStr(form->pgealias));
		pq_sendint8(&buf, form->pgekind);
		pq_sendint32(&buf, form->pgesrcvertexid);
		pq_sendint32(&buf, form->pgedestvertexid);
		for (attnum = Anum_pg_propgraph_element_pgekey;
			 attnum <= Natts_pg_propgraph_element; attnum++)
		{
			bool		isnull;
			Datum		value = heap_getattr(tuple, attnum, RelationGetDescr(mapping), &isnull);

			if (isnull)
				pq_sendint32(&buf, 0);
			else
			{
				bytea	   *binary;

				if (toast_raw_datum_size(value) > GGC_STATS_MAX_METADATA_BYTES)
					ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
									errmsg("graph statistics mapping exceeds its byte budget")));
				/* array_send needs a real FmgrInfo for its element-type cache. */
				binary = OidSendFunctionCall(F_ARRAY_SEND, value);
				if ((Size) buf.len + VARSIZE_ANY_EXHDR(binary) + 4 > GGC_STATS_MAX_METADATA_BYTES)
					ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
									errmsg("graph statistics dependency description exceeds its budget")));
				pq_sendint32(&buf, VARSIZE_ANY_EXHDR(binary));
				pq_sendbytes(&buf, VARDATA_ANY(binary), VARSIZE_ANY_EXHDR(binary));
			}
		}
		if (new_stats_oid)
			record_reference(new_stats_oid, PropgraphElementRelationId, element, 0, DEPENDENCY_AUTO);
		ReleaseSysCache(tuple);
	}
	table_close(mapping, AccessShareLock);
	if (scope->catalog_provider_oid)
	{
		HeapTuple	extension = SearchSysCache1(EXTENSIONOID, ObjectIdGetDatum(scope->catalog_provider_oid));

		if (!HeapTupleIsValid(extension))
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							errmsg("graph statistics catalog provider extension does not exist")));
		ReleaseSysCache(extension);
		if (new_stats_oid)
			record_reference(new_stats_oid, ExtensionRelationId, scope->catalog_provider_oid, 0, DEPENDENCY_NORMAL);
	}
	if (buf.len > GGC_STATS_MAX_METADATA_BYTES)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						errmsg("graph statistics dependency description exceeds its budget")));
	return pq_endtypsend(&buf);
}

/* Check the uncompressed size before detoasting a catalog value. */
static Datum
bounded_tuple_value(Relation relation, HeapTuple tuple, int attnum, Size maximum)
{
	bool		isnull;
	Datum		value = heap_getattr(tuple, attnum, RelationGetDescr(relation), &isnull);

	if (isnull)
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED),
						errmsg("missing graph statistics scope/payload")));
	if (toast_raw_datum_size(value) > maximum)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						errmsg("graph statistics catalog value exceeds its byte budget")));
	return value;
}

static bytea *
tuple_bytea(Relation relation, HeapTuple tuple, int attnum, Size maximum)
{
	return DatumGetByteaPP(bounded_tuple_value(relation, tuple, attnum, maximum));
}

static bool
bytes_equal(const bytea *left, const bytea *right)
{
	return VARSIZE_ANY_EXHDR(left) == VARSIZE_ANY_EXHDR(right) &&
		memcmp(VARDATA_ANY(left), VARDATA_ANY(right), VARSIZE_ANY_EXHDR(left)) == 0;
}

bool
graph_stats_dependencies_current(const GgcStatsScope *scope,
								 const bytea *stored)
{
	bytea	   *current;
	bool		matches;

	/* Permission/RLS changes invalidate the sample, not the user's query.
	 * Never open payloads after losing complete population visibility. */
	if (!scope_access_allowed(scope, false))
		return false;
	/* Planner callers only compare the canonical description; they never add
	 * dependency rows or retain a catalog varlena pointer. */
	current = capture_dependencies(scope, InvalidOid);
	matches = bytes_equal(stored, current);
	pfree(current);
	return matches;
}

static void
validate_metadata(Jsonb *metadata)
{
	Jsonb	   *resources;
	char	   *start;
	char	   *finish;
	TimestampTz start_time;
	TimestampTz finish_time;

	if (VARSIZE_ANY(metadata) > GGC_STATS_MAX_METADATA_BYTES)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						errmsg("graph statistics metadata exceeds its budget")));
	start = string_field(metadata, "started_at");
	finish = string_field(metadata, "finished_at");
	start_time = DatumGetTimestampTz(DirectFunctionCall3(timestamptz_in, CStringGetDatum(start), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1)));
	finish_time = DatumGetTimestampTz(DirectFunctionCall3(timestamptz_in, CStringGetDatum(finish), ObjectIdGetDatum(InvalidOid), Int32GetDatum(-1)));
	if (start_time > finish_time)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						errmsg("graph statistics collection finish precedes its start")));
	(void) string_field(metadata, "snapshot_source");
	(void) string_field(metadata, "sample_method");
	(void) string_field(metadata, "truncation_reason");
	(void) integer_field(metadata, "seed", PG_INT64_MAX);
	(void) integer_field(metadata, "population", PG_INT64_MAX);
	(void) integer_field(metadata, "sample_count", PG_INT64_MAX);
	(void) number_field(metadata, "effective_sample_size", true);
	(void) integer_field(metadata, "complete_depth", 4);
	(void) integer_field(metadata, "layout_version", PG_UINT32_MAX);
	(void) integer_field(metadata, "model_version", PG_UINT32_MAX);
	(void) object_field(metadata, "change_baseline");
	resources = object_field(metadata, "resources");
	(void) integer_field(resources, "scan_pages", PG_INT64_MAX);
	(void) integer_field(resources, "examined_entries", PG_INT64_MAX);
	(void) integer_field(resources, "peak_bytes", PG_INT64_MAX);
}

static GraphStatsProfile *
profiles_from_json(Jsonb *input, int *count)
{
	GraphStatsProfile *profiles;
	JsonbIterator *iterator;
	JsonbValue	value;
	JsonbIteratorToken token;
	Size		bytes = 0;
	int			i = 0;

	if (!JB_ROOT_IS_ARRAY(input) || JB_ROOT_IS_SCALAR(input) ||
		JB_ROOT_COUNT(input) == 0 || JB_ROOT_COUNT(input) > GGC_STATS_MAX_PROFILES ||
		VARSIZE_ANY(input) > GGC_STATS_MAX_PAYLOAD_BYTES)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						errmsg("graph statistics publication exceeds its profile/byte budget")));
	*count = JB_ROOT_COUNT(input);
	profiles = palloc0(sizeof(GraphStatsProfile) * *count);
	iterator = JsonbIteratorInit(&input->root);
	while ((token = JsonbIteratorNext(&iterator, &value, true)) != WJB_DONE)
	{
		Jsonb	   *profile;
		Jsonb	   *estimate;
		GgcMetricEstimate metric;

		if (token != WJB_ELEM)
			continue;
		profile = JsonbValueToJsonb(&value);
		profiles[i].metric_kind = integer_field(profile, "metric_kind", PG_INT16_MAX);
		if (profiles[i].metric_kind == 0)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("graph metric identifier must be positive")));
		profiles[i].key = object_field(profile, "profile_key");
		profiles[i].metadata = object_field(profile, "metadata");
		if (VARSIZE_ANY(profiles[i].key) > GGC_STATS_MAX_PROFILE_KEY_BYTES)
			ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
							errmsg("graph statistics profile key exceeds its budget"),
							errdetail("Metric %d key uses %zu bytes; the maximum is %d bytes.",
									  profiles[i].metric_kind,
									  (Size) VARSIZE_ANY(profiles[i].key),
									  GGC_STATS_MAX_PROFILE_KEY_BYTES)));
		validate_metadata(profiles[i].metadata);
		estimate = object_field(profile, "estimate");
		metric_from_json(estimate, &metric);
		profiles[i].payload = graph_stats_metric_encode(&metric);
		bytes += VARSIZE_ANY(profiles[i].key) + VARSIZE_ANY(profiles[i].metadata) +
			VARSIZE_ANY(profiles[i].payload);
		if (bytes > GGC_STATS_MAX_PAYLOAD_BYTES)
			ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
							errmsg("graph statistics payload exceeds its byte budget")));
		i++;
		CHECK_FOR_INTERRUPTS();
	}
	Assert(i == *count);
	return profiles;
}

static void
remove_superseded_profiles(Relation data, Oid stats_oid, int64 generation)
{
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple tuple;

	ScanKeyInit(&key, Anum_pg_graph_statistic_data_gstdstatid,
				BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(stats_oid));
	scan = systable_beginscan(data, GraphStatisticDataProfileIndexId, true,
							 NULL, 1, &key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		if (((Form_pg_graph_statistic_data) GETSTRUCT(tuple))->gstdgeneration < generation)
			CatalogTupleDelete(data, &tuple->t_self);
		CHECK_FOR_INTERRUPTS();
	}
	systable_endscan(scan);
}

Datum
pg_graph_statistic_scope(PG_FUNCTION_ARGS)
{
	GgcStatsScope scope;
	GgcStatsScope decoded;
	bytea	   *encoded;

	scope_from_json(PG_GETARG_JSONB_P(0), &scope);
	encoded = graph_stats_scope_encode(&scope);
	graph_stats_scope_decode(encoded, &decoded);
	PG_RETURN_JSONB_P(scope_to_json(&decoded));
}

Datum
pg_graph_statistic_create(PG_FUNCTION_ARGS)
{
	GgcStatsScope scope;
	Jsonb	   *options;
	Relation	relation;
	Oid			stats_oid;
	Datum		values[Natts_pg_graph_statistic];
	bool		nulls[Natts_pg_graph_statistic] = {false};
	HeapTuple	tuple;

	scope_from_json(PG_GETARG_JSONB_P(0), &scope);
	if (scope.effective_role != GetUserId())
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						errmsg("graph statistics collection role must match the current role")));
	graph_stats_check_collect_privileges(&scope);
	options = PG_GETARG_JSONB_P(1);
	if (!JB_ROOT_IS_OBJECT(options) || VARSIZE_ANY(options) > 8192)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("graph statistics options must be a bounded object")));
	{
		int64 anchor;

		if (graph_stats_anchor_condition(options, &anchor) && scope.scope_kind == GGC_STATS_COST)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							errmsg("cost profiles cannot register an anchor population")));
	}
	relation = table_open(GraphStatisticRelationId, RowExclusiveLock);
	stats_oid = GetNewOidWithIndex(relation, GraphStatisticOidIndexId, Anum_pg_graph_statistic_oid);
	values[Anum_pg_graph_statistic_oid - 1] = ObjectIdGetDatum(stats_oid);
	values[Anum_pg_graph_statistic_gstowner - 1] = ObjectIdGetDatum(GetUserId());
	values[Anum_pg_graph_statistic_gstkind - 1] = Int16GetDatum(scope.scope_kind);
	values[Anum_pg_graph_statistic_gstprovider - 1] = ObjectIdGetDatum(scope.catalog_provider_oid);
	values[Anum_pg_graph_statistic_gstgraphid - 1] = ObjectIdGetDatum(scope.graph_oid);
	values[Anum_pg_graph_statistic_gstedgeid - 1] = ObjectIdGetDatum(scope.edge_element_oid);
	values[Anum_pg_graph_statistic_gstindexid - 1] = ObjectIdGetDatum(scope.index_relid);
	values[Anum_pg_graph_statistic_gstrole - 1] = ObjectIdGetDatum(scope.effective_role);
	values[Anum_pg_graph_statistic_gstgeneration - 1] = Int64GetDatum(0);
	values[Anum_pg_graph_statistic_gstscope - 1] = PointerGetDatum(graph_stats_scope_encode(&scope));
	values[Anum_pg_graph_statistic_gstdependencies - 1] = PointerGetDatum(capture_dependencies(&scope, stats_oid));
	values[Anum_pg_graph_statistic_gstoptions - 1] = JsonbPGetDatum(options);
	tuple = heap_form_tuple(RelationGetDescr(relation), values, nulls);
	CatalogTupleInsert(relation, tuple);
	heap_freetuple(tuple);
	recordDependencyOnOwner(GraphStatisticRelationId, stats_oid, GetUserId());
	table_close(relation, RowExclusiveLock);
	/* Previously absent scopes also affect cached plans for this graph. */
	if (scope.graph_oid)
		CacheInvalidateRelcacheByRelid(scope.graph_oid);
	else if (scope.scope_kind == GGC_STATS_COST)
		CacheInvalidateRelcacheByRelid(GraphStatisticRelationId);
	CommandCounterIncrement();
	PG_RETURN_OID(stats_oid);
}

Datum
pg_graph_statistic_publish(PG_FUNCTION_ARGS)
{
	require_maintenance_privilege();
	PG_RETURN_INT64(graph_stats_publish_collected(PG_GETARG_OID(0),
		PG_GETARG_JSONB_P(1)));
}

/* C-only entry point: ordinary users may publish only collector output.
 * Do not expose this function as an SQL-callable arbitrary-payload writer. */
int64
graph_stats_publish_collected(Oid stats_oid, Jsonb *profile_json)
{
	GraphStatsProfile *profiles;
	int			count;
	int			i;
	Relation	head;
	Relation	data;
	HeapTuple	old_tuple;
	HeapTuple	new_tuple;
	GgcStatsScope scope;
	Form_pg_graph_statistic form;
	int64		generation;
	Datum		replacements[Natts_pg_graph_statistic] = {0};
	bool		replace_nulls[Natts_pg_graph_statistic] = {false};
	bool		do_replace[Natts_pg_graph_statistic] = {false};

	profiles = profiles_from_json(profile_json, &count);
	/* Held until transaction end; errors/rollback release it automatically. */
	LockDatabaseObject(GraphStatisticRelationId, stats_oid, 0, ExclusiveLock);
	head = table_open(GraphStatisticRelationId, RowExclusiveLock);
	old_tuple = SearchSysCache1(GRAPHSTATISTICOID, ObjectIdGetDatum(stats_oid));
	if (!HeapTupleIsValid(old_tuple))
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("graph statistics object %u does not exist", stats_oid)));
	form = (Form_pg_graph_statistic) GETSTRUCT(old_tuple);
	graph_stats_scope_decode(tuple_bytea(head, old_tuple, Anum_pg_graph_statistic_gstscope,
										 SCOPE_BYTES + VARHDRSZ), &scope);
	if (scope.effective_role != GetUserId() || form->gstowner != GetUserId())
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("graph statistics publisher must match its collection role")));
	graph_stats_check_collect_privileges(&scope);
	if (!bytes_equal(tuple_bytea(head, old_tuple, Anum_pg_graph_statistic_gstdependencies,
										GGC_STATS_MAX_METADATA_BYTES),
					 capture_dependencies(&scope, InvalidOid)))
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						errmsg("graph statistics dependencies changed; recreate the scope")));
	if (form->gstgeneration == PG_INT64_MAX)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("graph statistics generation exhausted")));
	{
		int64 registered_key = 0;
		bool conditional;
		Jsonb *options = DatumGetJsonbP(bounded_tuple_value(head, old_tuple,
			Anum_pg_graph_statistic_gstoptions, 8192));

		conditional = graph_stats_anchor_condition(options, &registered_key);
		for (i = 0; i < count; i++)
		{
			int64 profile_key = 0;
			bool profile_conditional = graph_stats_anchor_condition(profiles[i].metadata, &profile_key);

			if (conditional != profile_conditional ||
				(conditional && registered_key != profile_key))
				ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
								errmsg("graph statistics profile anchor population does not match registration")));
		}
	}
	generation = form->gstgeneration + 1;
	data = table_open(GraphStatisticDataRelationId, RowExclusiveLock);
	for (i = 0; i < count; i++)
	{
		Datum		values[Natts_pg_graph_statistic_data];
		bool		nulls[Natts_pg_graph_statistic_data] = {false};
		HeapTuple	tuple;

		CHECK_FOR_INTERRUPTS();
		values[Anum_pg_graph_statistic_data_gstdstatid - 1] = ObjectIdGetDatum(stats_oid);
		values[Anum_pg_graph_statistic_data_gstdgeneration - 1] = Int64GetDatum(generation);
		values[Anum_pg_graph_statistic_data_gstdmetric - 1] = Int16GetDatum(profiles[i].metric_kind);
		values[Anum_pg_graph_statistic_data_gstdformat - 1] = Int16GetDatum(GGC_STATS_FORMAT_VERSION);
		values[Anum_pg_graph_statistic_data_gstdkey - 1] = JsonbPGetDatum(profiles[i].key);
		values[Anum_pg_graph_statistic_data_gstdmetadata - 1] = JsonbPGetDatum(profiles[i].metadata);
		values[Anum_pg_graph_statistic_data_gstdpayload - 1] = PointerGetDatum(profiles[i].payload);
		tuple = heap_form_tuple(RelationGetDescr(data), values, nulls);
		CatalogTupleInsert(data, tuple);
		heap_freetuple(tuple);
	}
	/* The pointer is switched only after all profiles have been inserted. */
	replacements[Anum_pg_graph_statistic_gstgeneration - 1] = Int64GetDatum(generation);
	do_replace[Anum_pg_graph_statistic_gstgeneration - 1] = true;
	new_tuple = heap_modify_tuple(old_tuple, RelationGetDescr(head),
								 replacements, replace_nulls, do_replace);
	CatalogTupleUpdate(head, &old_tuple->t_self, new_tuple);
	/* These are ordinary MVCC deletions.  Old snapshots retain their old
	 * payloads; aborted publication restores them; VACUUM later reclaims them. */
	remove_superseded_profiles(data, stats_oid, generation);
	if (scope.graph_oid)
		CacheInvalidateRelcacheByRelid(scope.graph_oid);
	heap_freetuple(new_tuple);
	ReleaseSysCache(old_tuple);
	table_close(data, RowExclusiveLock);
	table_close(head, RowExclusiveLock);
	CommandCounterIncrement();
	return generation;
}

Datum
pg_graph_statistic_read(PG_FUNCTION_ARGS)
{
	Oid			stats_oid = PG_GETARG_OID(0);
	GgcStatsScope scope;
	bytea	   *wanted_scope;
	Relation	head;
	Relation	data;
	ScanKeyData head_key;
	ScanKeyData data_keys[2];
	SysScanDesc head_scan;
	SysScanDesc data_scan;
	HeapTuple	tuple;
	int64		generation;
	Snapshot	snapshot;
	StringInfoData result;
	int			count = 0;
	Size		decoded_bytes = 0;

	require_maintenance_privilege();
	scope_from_json(PG_GETARG_JSONB_P(1), &scope);
	if (scope.effective_role != GetUserId())
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE), errmsg("graph statistics read role differs from its security scope")));
	wanted_scope = graph_stats_scope_encode(&scope);
	snapshot = GetActiveSnapshot();
	/* Bypass catcache for the reader: both catalogs use this one MVCC snapshot.
	 * A repeatable-read backend can keep reading its old committed generation. */
	head = table_open(GraphStatisticRelationId, AccessShareLock);
	ScanKeyInit(&head_key, Anum_pg_graph_statistic_oid, BTEqualStrategyNumber,
				F_OIDEQ, ObjectIdGetDatum(stats_oid));
	head_scan = systable_beginscan(head, GraphStatisticOidIndexId, true,
								 snapshot, 1, &head_key);
	tuple = systable_getnext(head_scan);
	if (!HeapTupleIsValid(tuple) ||
		!bytes_equal(wanted_scope, tuple_bytea(head, tuple, Anum_pg_graph_statistic_gstscope,
												 SCOPE_BYTES + VARHDRSZ)))
	{
		systable_endscan(head_scan);
		table_close(head, AccessShareLock);
		PG_RETURN_NULL();
	}
	if (!bytes_equal(tuple_bytea(head, tuple, Anum_pg_graph_statistic_gstdependencies,
										GGC_STATS_MAX_METADATA_BYTES),
					 capture_dependencies(&scope, InvalidOid)))
	{
		systable_endscan(head_scan);
		table_close(head, AccessShareLock);
		PG_RETURN_NULL();
	}
	generation = ((Form_pg_graph_statistic) GETSTRUCT(tuple))->gstgeneration;
	systable_endscan(head_scan);
	table_close(head, AccessShareLock);
	initStringInfo(&result);
	appendStringInfo(&result, "{\"scope\":%s,\"generation\":" INT64_FORMAT ",\"profiles\":[",
					 JsonbToCString(NULL, &scope_to_json(&scope)->root, 0), generation);
	data = table_open(GraphStatisticDataRelationId, AccessShareLock);
	ScanKeyInit(&data_keys[0], Anum_pg_graph_statistic_data_gstdstatid,
				BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(stats_oid));
	ScanKeyInit(&data_keys[1], Anum_pg_graph_statistic_data_gstdgeneration,
				BTEqualStrategyNumber, F_INT8EQ, Int64GetDatum(generation));
	data_scan = systable_beginscan(data, GraphStatisticDataProfileIndexId, true,
								 snapshot, 2, data_keys);
	while (HeapTupleIsValid(tuple = systable_getnext(data_scan)))
	{
		Form_pg_graph_statistic_data form = (Form_pg_graph_statistic_data) GETSTRUCT(tuple);
		GgcMetricEstimate metric;
		Jsonb	   *key;
		Jsonb	   *metadata;
		bytea	   *payload;

		CHECK_FOR_INTERRUPTS();
		if (++count > GGC_STATS_MAX_PROFILES || form->gstdformat != GGC_STATS_FORMAT_VERSION)
			ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("graph statistics profile count/format is invalid")));
		key = DatumGetJsonbP(bounded_tuple_value(data, tuple,
													   Anum_pg_graph_statistic_data_gstdkey,
													   GGC_STATS_MAX_PROFILE_KEY_BYTES));
		metadata = DatumGetJsonbP(bounded_tuple_value(data, tuple,
															Anum_pg_graph_statistic_data_gstdmetadata,
															GGC_STATS_MAX_METADATA_BYTES));
		payload = tuple_bytea(data, tuple, Anum_pg_graph_statistic_data_gstdpayload,
							  METRIC_BYTES + VARHDRSZ);
		decoded_bytes += VARSIZE_ANY(key) + VARSIZE_ANY(metadata) + VARSIZE_ANY(payload);
		if (decoded_bytes > GGC_STATS_MAX_PAYLOAD_BYTES ||
			VARSIZE_ANY(key) > GGC_STATS_MAX_PROFILE_KEY_BYTES ||
			VARSIZE_ANY(metadata) > GGC_STATS_MAX_METADATA_BYTES)
			ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("graph statistics read exceeds its byte budget")));
		graph_stats_metric_decode(payload, &metric);
		appendStringInfo(&result,
						 "%s{\"metric_kind\":%d,\"profile_key\":%s,\"metadata\":%s,\"estimate\":%s}",
						 count > 1 ? "," : "", form->gstdmetric,
						 JsonbToCString(NULL, &key->root, 0),
						 JsonbToCString(NULL, &metadata->root, 0),
						 JsonbToCString(NULL, &metric_to_json(&metric)->root, 0));
	}
	systable_endscan(data_scan);
	table_close(data, AccessShareLock);
	if (generation > 0 && count == 0)
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("published graph statistics generation has no payload")));
	appendStringInfoString(&result, "]}");
	PG_RETURN_JSONB_P(DatumGetJsonbP(DirectFunctionCall1(jsonb_in, CStringGetDatum(result.data))));
}

void
RemoveGraphStatisticsById(Oid stats_oid)
{
	Relation	head = table_open(GraphStatisticRelationId, RowExclusiveLock);
	Relation	data = table_open(GraphStatisticDataRelationId, RowExclusiveLock);
	ScanKeyData key;
	SysScanDesc scan;
	HeapTuple	tuple;
	HeapTuple	head_tuple;

	head_tuple = SearchSysCache1(GRAPHSTATISTICOID, ObjectIdGetDatum(stats_oid));
	if (!HeapTupleIsValid(head_tuple))
		elog(ERROR, "cache lookup failed for graph statistics object %u", stats_oid);
	ScanKeyInit(&key, Anum_pg_graph_statistic_data_gstdstatid,
				BTEqualStrategyNumber, F_OIDEQ, ObjectIdGetDatum(stats_oid));
	scan = systable_beginscan(data, GraphStatisticDataProfileIndexId, true, NULL, 1, &key);
	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
		CatalogTupleDelete(data, &tuple->t_self);
	systable_endscan(scan);
	CatalogTupleDelete(head, &head_tuple->t_self);
	if (((Form_pg_graph_statistic) GETSTRUCT(head_tuple))->gstgraphid)
		CacheInvalidateRelcacheByRelid(((Form_pg_graph_statistic) GETSTRUCT(head_tuple))->gstgraphid);
	else if (((Form_pg_graph_statistic) GETSTRUCT(head_tuple))->gstkind == GGC_STATS_COST)
		CacheInvalidateRelcacheByRelid(GraphStatisticRelationId);
	ReleaseSysCache(head_tuple);
	table_close(data, RowExclusiveLock);
	table_close(head, RowExclusiveLock);
}

/*
 * Graph statistics are derived, role-scoped samples, not transferable data.
 * REASSIGN OWNED invalidates them: changing only gstowner strands gstrole,
 * while rewriting the encoded scope could both relabel another role's sample
 * and collide with an existing scope belonging to the destination role.
 *
 * Normal dependency deletion removes the scope, its payload and owner
 * dependency atomically.  DROP_RESTRICT cannot cascade into business objects;
 * references to graphs, tables and indexes point from this derived object to
 * those objects, not the other way around.  A new owner must collect afresh.
 */
void
ReassignGraphStatisticsOwner(Oid stats_oid, Oid new_owner)
{
	HeapTuple	tuple;
	Oid			owner;
	ObjectAddress address;

	LockDatabaseObject(GraphStatisticRelationId, stats_oid, 0, ExclusiveLock);
	tuple = SearchSysCache1(GRAPHSTATISTICOID, ObjectIdGetDatum(stats_oid));
	if (!HeapTupleIsValid(tuple))
		return;					/* A concurrent deletion already invalidated it. */
	owner = ((Form_pg_graph_statistic) GETSTRUCT(tuple))->gstowner;
	ReleaseSysCache(tuple);

	/* REASSIGN OWNED BY role TO role must preserve its samples. */
	if (owner == new_owner)
		return;

	ObjectAddressSet(address, GraphStatisticRelationId, stats_oid);
	performDeletion(&address, DROP_RESTRICT, 0);
}

Datum
pg_graph_statistic_drop(PG_FUNCTION_ARGS)
{
	ObjectAddress address;
	Oid stats_oid = PG_GETARG_OID(0);
	HeapTuple tuple;
	Form_pg_graph_statistic form;

	LockDatabaseObject(GraphStatisticRelationId, stats_oid, 0, ExclusiveLock);
	tuple = SearchSysCache1(GRAPHSTATISTICOID, ObjectIdGetDatum(stats_oid));
	if (!HeapTupleIsValid(tuple))
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
						errmsg("graph statistics object %u does not exist", stats_oid)));
	form = (Form_pg_graph_statistic) GETSTRUCT(tuple);
	/* Permit cleanup after table privileges were revoked. No payload is read. */
	if (!superuser() &&
		(form->gstowner != GetUserId() || form->gstrole != GetUserId()))
		ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
						errmsg("must own the graph statistics scope to drop it")));
	ReleaseSysCache(tuple);
	ObjectAddressSet(address, GraphStatisticRelationId, stats_oid);
	performDeletion(&address, DROP_RESTRICT, 0);
	PG_RETURN_VOID();
}
