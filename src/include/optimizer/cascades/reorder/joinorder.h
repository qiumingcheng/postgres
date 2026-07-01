/*-------------------------------------------------------------------------
 *
 * joinorder.h
 *	  Data structures and functions for join order enumeration
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#ifndef CASCADES_JOINORDER_H
#define CASCADES_JOINORDER_H

#include "nodes/pg_list.h"
#include "nodes/bitmapset.h"
#include "nodes/relation.h"
#include "optimizer/cascades/reorder/multijoin.h"

/*
 * PgGroupInfo - 表示子join树的最优计划
 */
typedef struct PgGroupInfo
{
	Bitmapset  *tables;			/* 包含的表集合 */
	double		cost;			/* 最优成本 */
	double		rows;			/* 输出行数 */
	struct PgExpressionInfo *best_expr;	/* 最优表达式 */
} PgGroupInfo;

/*
 * PgExpressionInfo - 表示一个join表达式
 */
typedef struct PgExpressionInfo
{
	PgGroupInfo *left;			/* 左子树 */
	PgGroupInfo *right;			/* 右子树 */
	List	   *join_preds;		/* join条件列表 */
	JoinType	join_type;		/* JOIN类型 */
	double		cost;			/* 总成本 */
	double		rows;			/* 输出行数 */
} PgExpressionInfo;

/* GroupInfo创建和管理 */
extern PgGroupInfo *pg_group_info_create(Bitmapset *tables,
										 double cost,
										 double rows,
										 PgExpressionInfo *best_expr);

extern PgGroupInfo *pg_get_base_table_group(PlannerInfo *root,
											PgMultiJoinNode *mjn,
											int table_id);

/* Expression创建和成本计算 */
extern PgExpressionInfo *pg_build_join_expr(PlannerInfo *root,
											PgMultiJoinNode *mjn,
											PgGroupInfo *left,
											PgGroupInfo *right);

extern void pg_compute_cost(PlannerInfo *root,
							PgExpressionInfo *expr,
							PgMultiJoinNode *mjn);

extern double pg_estimate_join_cost(PlannerInfo *root,
									PgMultiJoinNode *mjn,
									PgGroupInfo *left,
									PgGroupInfo *right);

/* 成本估算辅助函数 */
extern double estimate_join_selectivity(PlannerInfo *root,
										List *join_preds,
										Bitmapset *left_tables,
										Bitmapset *right_tables);

extern bool is_equijoin_predicate(RestrictInfo *rinfo);

/* 常量定义 */
#define CROSS_JOIN_PENALTY		1000.0	/* Cross join惩罚系数 */
#define NESTED_LOOP_PENALTY		10.0	/* Nested loop惩罚 */

#endif							/* CASCADES_JOINORDER_H */
