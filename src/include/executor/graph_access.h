/* Shared integer-key and column-ACL contracts; no concrete provider links. */
#ifndef GRAPH_ACCESS_H
#define GRAPH_ACCESS_H

#include "access/nbtree.h"
#include "catalog/pg_type_d.h"
#include "miscadmin.h"
#include "nodes/bitmapset.h"
#include "utils/acl.h"
#include "utils/fmgroids.h"

static inline bool
GgcIntegerKeyType(Oid type)
{
	return type == INT8OID || type == INT4OID || type == INT2OID;
}

/* Widen values, never narrow a probe: an int8 outside the int4 range must
 * simply find no int4 row, not wrap or raise a cast overflow. */
static inline int64
GgcIntegerKeyValue(Datum value, Oid type)
{
	if (type == INT8OID)
		return DatumGetInt64(value);
	if (type == INT4OID)
		return DatumGetInt32(value);
	Assert(type == INT2OID);
	return DatumGetInt16(value);
}

static inline void
GgcIntegerScanKey(ScanKey key, Oid index_type, StrategyNumber strategy,
				  Datum value)
{
	Oid function;

	Assert(GgcIntegerKeyType(index_type));
	Assert(strategy == BTEqualStrategyNumber ||
		   strategy == BTGreaterEqualStrategyNumber ||
		   strategy == BTLessEqualStrategyNumber);
	if (index_type == INT8OID)
		function = strategy == BTEqualStrategyNumber ? F_INT8EQ :
			strategy == BTGreaterEqualStrategyNumber ? F_INT8GE : F_INT8LE;
	else if (index_type == INT4OID)
		function = strategy == BTEqualStrategyNumber ? F_INT48EQ :
			strategy == BTGreaterEqualStrategyNumber ? F_INT48GE : F_INT48LE;
	else
		function = strategy == BTEqualStrategyNumber ? F_INT28EQ :
			strategy == BTGreaterEqualStrategyNumber ? F_INT28GE : F_INT28LE;
	ScanKeyEntryInitialize(key, 0, 1, strategy, INT8OID, InvalidOid,
						   function, value);
}

static inline Datum
GgcIntegerKeyResult(int64 value, Oid type)
{
	if (type == INT8OID)
		return Int64GetDatum(value);
	if (type == INT4OID && value >= PG_INT32_MIN && value <= PG_INT32_MAX)
		return Int32GetDatum((int32) value);
	if (type == INT2OID && value >= PG_INT16_MIN && value <= PG_INT16_MAX)
		return Int16GetDatum((int16) value);
	elog(ERROR, "graph binding is outside its declared integer type");
	return (Datum) 0;
}

static inline bool
GgcSelectColumnAllowed(Oid relation, AttrNumber column, Oid role)
{
	return column > 0 &&
		(pg_class_aclcheck(relation, role, ACL_SELECT) == ACLCHECK_OK ||
		 pg_attribute_aclcheck(relation, column, role, ACL_SELECT) == ACLCHECK_OK);
}

/* Positive base attribute numbers (not RTE bitmap offsets). A NULL list is
 * an unbounded callback: retain the old whole-table permission contract. */
static inline bool
GgcSelectColumnsAllowed(Oid relation, const Bitmapset *columns, Oid role)
{
	int column = -1;

	if (pg_class_aclcheck(relation, role, ACL_SELECT) == ACLCHECK_OK)
		return true;
	if (bms_is_empty(columns))
		return false;
	while ((column = bms_next_member(columns, column)) >= 0)
		if (column <= 0 ||
			pg_attribute_aclcheck(relation, column, role, ACL_SELECT) != ACLCHECK_OK)
			return false;
	return true;
}

#endif
