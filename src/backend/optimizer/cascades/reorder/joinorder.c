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
	group->rel_info = (best_expr != NULL) ? best_expr->rel_info : NULL;

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
	group->rel_info = rel;			/* 关键：保存base RelOptInfo */

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
	RelOptInfo *left_rel;
	RelOptInfo *right_rel;

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

	/*
	 * 关键新增：构建真正的PostgreSQL RelOptInfo
	 * 这样Cascades就能获得完整的join信息（restrictinfo等）
	 */
	left_rel = left->rel_info;
	right_rel = right->rel_info;

	if (left_rel != NULL && right_rel != NULL)
	{
		Bitmapset *joinrelids;
		List *restrictlist = NIL;
		SpecialJoinInfo sjinfo_data;
		SpecialJoinInfo *sjinfo;

		elog(DEBUG1, "DP: Building join RelOptInfo, left has %d rels, right has %d rels",
			 bms_num_members(left_rel->relids), bms_num_members(right_rel->relids));

		joinrelids = bms_union(left_rel->relids, right_rel->relids);

		/* 为inner join创建SpecialJoinInfo */
		memset(&sjinfo_data, 0, sizeof(SpecialJoinInfo));
		sjinfo = &sjinfo_data;
		sjinfo->type = T_SpecialJoinInfo;
		sjinfo->min_lefthand = left_rel->relids;
		sjinfo->min_righthand = right_rel->relids;
		sjinfo->syn_lefthand = left_rel->relids;
		sjinfo->syn_righthand = right_rel->relids;
		sjinfo->jointype = JOIN_INNER;
		sjinfo->lhs_strict = false;
		sjinfo->delay_upper_joins = false;
		sjinfo->join_quals = NIL;

		elog(DEBUG1, "DP: Calling build_join_rel...");

		/* 调用PostgreSQL的标准API构建join RelOptInfo */
		PG_TRY();
		{
			expr->rel_info = build_join_rel(root,
											 joinrelids,
											 left_rel,
											 right_rel,
											 sjinfo,
											 &restrictlist);

			if (expr->rel_info != NULL)
			{
				elog(DEBUG1, "DP: build_join_rel succeeded, rows=%.0f, pathlist_len=%d",
					 expr->rel_info->rows,
					 list_length(expr->rel_info->pathlist));

				/*
				 * 关键修复：参考PostgreSQL的make_join_rel，在build_join_rel之后
				 * 需要调用add_paths_to_joinrel来生成访问路径（HashJoin、NestLoop、MergeJoin等）。
				 *
				 * 重要：需要临时开启所有join类型，确保生成完整的paths。
				 * Cascades的Task Scheduler可能选择任何类型的join，我们必须为所有类型
				 * 提供IMPORTED_PATH，否则Plan Building会失败。
				 *
				 * 注意：需要考虑两个方向（left+right 和 right+left），
				 * 就像PostgreSQL标准实现一样。
				 */
				if (list_length(expr->rel_info->pathlist) == 0)
				{
					bool save_enable_mergejoin = enable_mergejoin;
					bool save_enable_hashjoin = enable_hashjoin;
					bool save_enable_nestloop = enable_nestloop;

					elog(DEBUG1, "DP: Generating paths for join rel (both directions)...");

					/* 临时开启所有join类型，确保生成完整的paths */
					enable_mergejoin = true;
					enable_hashjoin = true;
					enable_nestloop = true;

					/* 方向1: left join right */
					add_paths_to_joinrel(root,
										 expr->rel_info,
										 left_rel,
										 right_rel,
										 JOIN_INNER,
										 sjinfo,
										 restrictlist);

					/* 方向2: right join left (考虑commutative joins) */
					add_paths_to_joinrel(root,
										 expr->rel_info,
										 right_rel,
										 left_rel,
										 JOIN_INNER,
										 sjinfo,
										 restrictlist);

					/* 恢复原始配置 */
					enable_mergejoin = save_enable_mergejoin;
					enable_hashjoin = save_enable_hashjoin;
					enable_nestloop = save_enable_nestloop;

					/* 选择最便宜的路径 */
					set_cheapest(expr->rel_info);

					elog(DEBUG1, "DP: After add_paths_to_joinrel, pathlist_len=%d, cheapest_total_cost=%.2f",
						 list_length(expr->rel_info->pathlist),
						 expr->rel_info->cheapest_total_path ? expr->rel_info->cheapest_total_path->total_cost : 0.0);
				}
			}
			else
			{
				elog(WARNING, "DP: build_join_rel returned NULL");
			}
		}
		PG_CATCH();
		{
			elog(WARNING, "DP: build_join_rel threw an exception!");
			FlushErrorState();
			expr->rel_info = NULL;
		}
		PG_END_TRY();

		bms_free(joinrelids);
	}
	else
	{
		expr->rel_info = NULL;
		elog(DEBUG2, "Cannot build join RelOptInfo: left_rel=%p, right_rel=%p",
			 left_rel, right_rel);
	}

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
