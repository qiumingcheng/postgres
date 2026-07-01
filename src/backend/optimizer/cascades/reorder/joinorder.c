/*-------------------------------------------------------------------------
 *
 * joinorder.c
 *	  Join order enumeration framework
 *
 * 功能：
 *   1. GroupInfo和ExpressionInfo管理
 *   2. Join成本计算
 *   3. 选择率估算
 *   4. 工具函数
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "optimizer/cascades/reorder/joinorder.h"
#include "optimizer/cascades/reorder/multijoin.h"
#include "optimizer/pathnode.h"
#include "optimizer/cost.h"
#include "optimizer/clauses.h"
#include "nodes/relation.h"
#include "utils/memutils.h"

/* 静态函数声明 */
static RelOptInfo *find_rel_by_id(PlannerInfo *root, PgMultiJoinNode *mjn, int table_id);
static double estimate_output_rows(PlannerInfo *root, PgGroupInfo *left, 
									PgGroupInfo *right, List *join_preds);

/*
 * pg_group_info_create - 创建GroupInfo
 */
PgGroupInfo *
pg_group_info_create(Bitmapset *tables,
					 double cost,
					 double rows,
					 PgExpressionInfo *best_expr)
{
	PgGroupInfo *group = (PgGroupInfo *) palloc(sizeof(PgGroupInfo));

	group->tables = bms_copy(tables);
	group->cost = cost;
	group->rows = rows;
	group->best_expr = best_expr;

	return group;
}

/*
 * pg_get_base_table_group - 创建基表的GroupInfo
 */
PgGroupInfo *
pg_get_base_table_group(PlannerInfo *root,
						PgMultiJoinNode *mjn,
						int table_id)
{
	RelOptInfo *rel = find_rel_by_id(root, mjn, table_id);
	PgGroupInfo *group;

	if (rel == NULL)
	{
		elog(ERROR, "Cannot find relation with id %d", table_id);
	}

	group = (PgGroupInfo *) palloc(sizeof(PgGroupInfo));
	group->tables = bms_make_singleton(table_id);
	group->cost = rel->rows;		/* 基表成本 = 行数 */
	group->rows = rel->rows;
	group->best_expr = NULL;		/* 基表无表达式 */

	return group;
}

/*
 * pg_build_join_expr - 构建join表达式
 */
PgExpressionInfo *
pg_build_join_expr(PlannerInfo *root,
				   PgMultiJoinNode *mjn,
				   PgGroupInfo *left,
				   PgGroupInfo *right)
{
	PgExpressionInfo *expr;

	expr = (PgExpressionInfo *) palloc(sizeof(PgExpressionInfo));
	expr->left = left;
	expr->right = right;
	expr->join_type = JOIN_INNER;

	/* 获取join条件 */
	expr->join_preds = pg_multijoin_get_join_predicates(mjn,
														left->tables,
														right->tables);

	/* 计算成本和行数 */
	pg_compute_cost(root, expr, mjn);

	return expr;
}

/*
 * pg_compute_cost - 计算join成本（核心函数）
 */
void
pg_compute_cost(PlannerInfo *root,
				PgExpressionInfo *expr,
				PgMultiJoinNode *mjn)
{
	double		left_cost = expr->left->cost;
	double		right_cost = expr->right->cost;
	double		left_rows = expr->left->rows;
	double		right_rows = expr->right->rows;
	double		total_cost;
	double		output_rows;

	/* 基础成本：子树成本之和 */
	total_cost = left_cost + right_cost;

	/* 计算输出行数 */
	if (list_length(expr->join_preds) == 0)
	{
		/* Cross join - 笛卡尔积 */
		output_rows = left_rows * right_rows;
		
		/* 应用惩罚系数 */
		total_cost += output_rows * CROSS_JOIN_PENALTY;
		
		elog(DEBUG2, "Cross join: left_rows=%.0f, right_rows=%.0f, "
			 "output=%.0f, penalty_cost=%.0f",
			 left_rows, right_rows, output_rows,
			 output_rows * CROSS_JOIN_PENALTY);
	}
	else
	{
		/* 有join条件：估算选择率 */
		double		selectivity = estimate_join_selectivity(root,
														   expr->join_preds,
														   expr->left->tables,
														   expr->right->tables);
		
		output_rows = left_rows * right_rows * selectivity;
		
		/* Join成本 = 输出行数 */
		total_cost += output_rows;
		
		/* 如果没有等值条件，应用nested loop惩罚 */
		bool has_eq = false;
		ListCell *lc;
		foreach(lc, expr->join_preds)
		{
			if (is_equijoin_predicate((RestrictInfo *) lfirst(lc)))
			{
				has_eq = true;
				break;
			}
		}
		
		if (!has_eq)
		{
			total_cost *= NESTED_LOOP_PENALTY;
			elog(DEBUG2, "No equijoin, applying nested loop penalty");
		}
		
		elog(DEBUG2, "Join: left_rows=%.0f, right_rows=%.0f, "
			 "selectivity=%.4f, output=%.0f",
			 left_rows, right_rows, selectivity, output_rows);
	}

	expr->cost = total_cost;
	expr->rows = output_rows;
}

/*
 * pg_estimate_join_cost - 快速估算join成本（用于剪枝4）
 */
double
pg_estimate_join_cost(PlannerInfo *root,
					  PgMultiJoinNode *mjn,
					  PgGroupInfo *left,
					  PgGroupInfo *right)
{
	double		child_cost = left->cost + right->cost;
	List	   *join_preds;

	/* 检查是否有join条件 */
	join_preds = pg_multijoin_get_join_predicates(mjn,
												  left->tables,
												  right->tables);

	if (list_length(join_preds) == 0)
	{
		/* Cross join：上界很大 */
		return child_cost + left->rows * right->rows * CROSS_JOIN_PENALTY;
	}
	else
	{
		/* 有join条件：乐观估算（下界） */
		double		estimated_output = left->rows * right->rows * 0.1;
		return child_cost + estimated_output;
	}
}

/*
 * estimate_join_selectivity - 估算join选择率
 */
double
estimate_join_selectivity(PlannerInfo *root,
						  List *join_preds,
						  Bitmapset *left_tables,
						  Bitmapset *right_tables)
{
	int			num_eq_preds = 0;
	double		selectivity = 1.0;
	ListCell   *lc;

	/* 简化实现：统计等值条件数量 */
	foreach(lc, join_preds)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
		if (is_equijoin_predicate(rinfo))
			num_eq_preds++;
	}

	if (num_eq_preds == 0)
		return 1.0;				/* 无等值条件 */

	/* 每个等值条件假设0.1选择率 */
	for (int i = 0; i < num_eq_preds; i++)
		selectivity *= 0.1;

	/* 下限保护 */
	if (selectivity < 0.0001)
		selectivity = 0.0001;

	return selectivity;
}

/*
 * is_equijoin_predicate - 判断是否是等值join条件
 */
bool
is_equijoin_predicate(RestrictInfo *rinfo)
{
	Expr	   *clause = rinfo->clause;

	if (IsA(clause, OpExpr))
	{
		OpExpr	   *opexpr = (OpExpr *) clause;

		/* 检查是否是二元操作符 */
		if (list_length(opexpr->args) == 2)
		{
			/* 简化：假设二元操作符都是等值比较 */
			return true;
		}
	}

	return false;
}

/*
 * find_rel_by_id - 从MultiJoinNode查找RelOptInfo
 */
static RelOptInfo *
find_rel_by_id(PlannerInfo *root, PgMultiJoinNode *mjn, int table_id)
{
	ListCell   *lc;

	foreach(lc, mjn->atoms)
	{
		RelOptInfo *rel = (RelOptInfo *) lfirst(lc);
		if (rel->relid == table_id)
			return rel;
	}

	return NULL;
}
