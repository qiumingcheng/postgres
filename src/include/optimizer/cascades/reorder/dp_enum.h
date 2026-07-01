/*-------------------------------------------------------------------------
 *
 * dp_enum.h
 *	  DP recursive enumeration with 4-layer pruning
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#ifndef CASCADES_DP_ENUM_H
#define CASCADES_DP_ENUM_H

#include "optimizer/cascades/reorder/multijoin.h"
#include "optimizer/cascades/reorder/joinorder.h"

/* DP枚举主入口 */
extern PgGroupInfo *pg_join_reorder_dp(PlannerInfo *root,
									   PgMultiJoinNode *mjn);

/* Partition生成 */
extern List *generate_partitions(Bitmapset *tables);
extern List *generate_partitions_optimized(Bitmapset *tables,
										   PgMultiJoinNode *mjn,
										   PlannerInfo *root);

/* 核心递归枚举 */
extern PgGroupInfo *pg_get_best_expr(PlannerInfo *root,
									PgMultiJoinNode *mjn,
									Bitmapset *tables);

#endif							/* CASCADES_DP_ENUM_H */
