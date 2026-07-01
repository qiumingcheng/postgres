/*-------------------------------------------------------------------------
 *
 * dp_enum.c
 *	  DP recursive enumeration with 4-layer pruning (CORE MODULE)
 *
 * 功能：
 *   1. DP递归枚举所有join顺序
 *   2. 四层剪枝优化
 *   3. Memoization缓存
 *   4. 全局upper bound动态更新
 *
 * 四层剪枝：
 *   - 剪枝1：左子树成本太贵
 *   - 剪枝2：右子树成本太贵
 *   - 剪枝3：子树成本和太贵
 *   - 剪枝4：估算join成本太贵
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include <float.h>

#include "optimizer/cascades/reorder/dp_enum.h"
#include "optimizer/cascades/reorder/joinorder.h"
#include "optimizer/cascades/reorder/multijoin.h"
#include "utils/memutils.h"
#include "utils/hsearch.h"
#include "nodes/bitmapset.h"

/* Memo hash表的key */
typedef struct MemoHashKey
{
	Bitmapset  *tables;
} MemoHashKey;

/* Memo hash表的entry */
typedef struct MemoHashEntry
{
	MemoHashKey key;
	PgGroupInfo *group_info;
} MemoHashEntry;

/* 全局变量（per-query） */
static HTAB *best_plan_memo = NULL;
static double global_best_cost = 0;
static int prune_count[4] = {0, 0, 0, 0};
static int total_enumerations = 0;
static int memo_hits = 0;

/* 静态函数声明 */
static void init_dp_context(PgMultiJoinNode *mjn);
static void cleanup_dp_context(void);
static uint32 bitmapset_hash_func(const void *key, Size keysize);
static int bitmapset_compare_func(const void *key1, const void *key2, Size keysize);

/*
 * pg_join_reorder_dp - DP枚举主入口
 */
PgGroupInfo *
pg_join_reorder_dp(PlannerInfo *root, PgMultiJoinNode *mjn)
{
	PgGroupInfo *result;

	elog(DEBUG1, "Starting DP enumeration for %d tables", mjn->num_tables);

	/* 初始化上下文 */
	init_dp_context(mjn);

	/* 开始递归枚举 */
	result = pg_get_best_expr(root, mjn, mjn->table_ids);

	/* 输出统计信息 */
	elog(NOTICE, "DP enumeration completed: "
		 "tables=%d, enumerations=%d, memo_hits=%d, "
		 "pruning=[L1=%d, L2=%d, L3=%d, L4=%d], "
		 "best_cost=%.2f",
		 mjn->num_tables, total_enumerations, memo_hits,
		 prune_count[0], prune_count[1], prune_count[2], prune_count[3],
		 result ? result->cost : -1.0);

	/* 计算剪枝率 */
	if (total_enumerations > 0)
	{
		int		total_pruned = prune_count[0] + prune_count[1] +
		prune_count[2] + prune_count[3];
		double	prune_rate = (double) total_pruned /
		(total_enumerations + total_pruned) * 100.0;

		elog(NOTICE, "Pruning efficiency: %.1f%% (%d/%d)",
			 prune_rate, total_pruned,
			 total_enumerations + total_pruned);
	}

	/* 清理 */
	cleanup_dp_context();

	return result;
}

/*
 * pg_get_best_expr - 核心递归函数 + 四层剪枝
 *
 * 这是整个DP算法的核心，实现：
 * 1. 递归枚举所有bipartition
 * 2. 四层剪枝
 * 3. Memoization
 * 4. Upper bound动态更新
 */
PgGroupInfo *
pg_get_best_expr(PlannerInfo *root,
				 PgMultiJoinNode *mjn,
				 Bitmapset *tables)
{
	int			num_tables = bms_num_members(tables);
	PgGroupInfo *result;

	/* ========== 基本情况：单表 ========== */
	if (num_tables == 1)
	{
		int			table_id = bms_first_member(bms_copy(tables));
		return pg_get_base_table_group(root, mjn, table_id);
	}

	/* ========== 检查Memo缓存 ========== */
	{
		MemoHashKey key;
		MemoHashEntry *entry;
		bool		found;

		key.tables = tables;
		entry = (MemoHashEntry *) hash_search(best_plan_memo,
											   &key,
											   HASH_FIND,
											   &found);
		if (found)
		{
			memo_hits++;
			return entry->group_info;
		}
	}

	/* ========== 枚举所有bipartition ========== */
	{
		double		best_cost = DBL_MAX;
		PgExpressionInfo *best_expr = NULL;
		List	   *partitions;
		ListCell   *lc;

		/* 生成partition列表（优化版） */
		partitions = generate_partitions_optimized(tables, mjn, root);

		foreach(lc, partitions)
		{
			Bitmapset  *left_tables = (Bitmapset *) lfirst(lc);
			Bitmapset  *right_tables = bms_difference(tables, left_tables);
			PgGroupInfo *left_group;
			PgGroupInfo *right_group;
			PgExpressionInfo *join_expr;
			double		child_lb;
			double		join_lb;

			total_enumerations++;

			/* === 递归获取左子树 === */
			left_group = pg_get_best_expr(root, mjn, left_tables);

			/* === 剪枝1：左子树太贵 === */
			if (left_group->cost > best_cost)
			{
				prune_count[0]++;
				bms_free(right_tables);
				continue;
			}

			/* 早期全局剪枝 */
			if (left_group->cost > global_best_cost)
			{
				prune_count[0]++;
				bms_free(right_tables);
				continue;
			}

			/* === 递归获取右子树 === */
			right_group = pg_get_best_expr(root, mjn, right_tables);

			/* === 剪枝2：右子树太贵 === */
			if (right_group->cost > best_cost)
			{
				prune_count[1]++;
				bms_free(right_tables);
				continue;
			}

			/* === 剪枝3：子树成本和太贵 === */
			child_lb = left_group->cost + right_group->cost;
			if (child_lb > best_cost)
			{
				prune_count[2]++;
				bms_free(right_tables);
				continue;
			}

			/* === 剪枝4：估算join成本太贵 === */
			join_lb = pg_estimate_join_cost(root, mjn, left_group, right_group);
			if (join_lb > best_cost)
			{
				prune_count[3]++;
				bms_free(right_tables);
				continue;
			}

			/* === 通过所有剪枝：构建完整join表达式 === */
			join_expr = pg_build_join_expr(root, mjn, left_group, right_group);

			/* 更新最优 */
			if (join_expr->cost < best_cost)
			{
				best_cost = join_expr->cost;
				best_expr = join_expr;

				/* 更新全局最优（帮助后续剪枝） */
				if (best_cost < global_best_cost)
				{
					global_best_cost = best_cost;
				}
			}

			bms_free(right_tables);
		}

		/* 创建结果 */
		if (best_expr == NULL)
		{
			elog(WARNING, "No valid join found for table set");
			return NULL;
		}

		result = pg_group_info_create(tables,
									   best_cost,
									   best_expr->rows,
									   best_expr);
	}

	/* ========== 缓存结果 ========== */
	{
		MemoHashKey key;
		MemoHashEntry *entry;
		bool		found;

		key.tables = bms_copy(tables);
		entry = (MemoHashEntry *) hash_search(best_plan_memo,
											   &key,
											   HASH_ENTER,
											   &found);
		entry->group_info = result;
	}

	return result;
}

/*
 * generate_partitions - 生成所有bipartition
 */
List *
generate_partitions(Bitmapset *tables)
{
	List	   *result = NIL;
	int			num_tables = bms_num_members(tables);
	int		   *table_ids;
	int			idx = 0;
	long		max_mask;
	long		mask;
	Bitmapset  *tmpset;

	/* 提取所有表ID */
	table_ids = (int *) palloc(num_tables * sizeof(int));
	tmpset = bms_copy(tables);
	while (!bms_is_empty(tmpset))
	{
		int x = bms_first_member(tmpset);
		table_ids[idx++] = x;
	}

	/* 枚举所有非空真子集 */
	max_mask = (1L << num_tables) - 1;

	for (mask = 1; mask < max_mask; mask++)
	{
		Bitmapset  *partition = NULL;
		int			i;

		for (i = 0; i < num_tables; i++)
		{
			if (mask & (1L << i))
			{
				partition = bms_add_member(partition, table_ids[i]);
			}
		}

		/* 只保留较小的partition（避免重复） */
		if (bms_num_members(partition) <= num_tables / 2)
		{
			result = lappend(result, partition);
		}
		else
		{
			bms_free(partition);
		}
	}

	pfree(table_ids);
	return result;
}

/*
 * generate_partitions_optimized - 优化的partition生成
 * 
 * 优先尝试有join条件的partition
 */
List *
generate_partitions_optimized(Bitmapset *tables,
							   PgMultiJoinNode *mjn,
							   PlannerInfo *root)
{
	List	   *with_join = NIL;
	List	   *without_join = NIL;
	List	   *all_partitions;
	ListCell   *lc;

	/* 生成所有partition */
	all_partitions = generate_partitions(tables);

	/* 分类：有join条件 vs 无join条件 */
	foreach(lc, all_partitions)
	{
		Bitmapset  *partition = (Bitmapset *) lfirst(lc);
		Bitmapset  *complement = bms_difference(tables, partition);
		List	   *preds;

		preds = pg_multijoin_get_join_predicates(mjn, partition, complement);

		if (list_length(preds) > 0)
		{
			with_join = lappend(with_join, partition);
		}
		else
		{
			without_join = lappend(without_join, partition);
		}

		bms_free(complement);
	}

	/* 有join条件的优先 */
	return list_concat(with_join, without_join);
}

/*
 * init_dp_context - 初始化DP上下文
 */
static void
init_dp_context(PgMultiJoinNode *mjn)
{
	HASHCTL		ctl;

	/* 创建hash表 */
	MemSet(&ctl, 0, sizeof(ctl));
	ctl.keysize = sizeof(MemoHashKey);
	ctl.entrysize = sizeof(MemoHashEntry);
	ctl.hash = bitmapset_hash_func;
	ctl.match = bitmapset_compare_func;

	best_plan_memo = hash_create("BestPlanMemo",
								 1024,
								 &ctl,
								 HASH_ELEM | HASH_FUNCTION | HASH_COMPARE);

	/* 初始化统计 */
	global_best_cost = DBL_MAX;
	MemSet(prune_count, 0, sizeof(prune_count));
	total_enumerations = 0;
	memo_hits = 0;
}

/*
 * cleanup_dp_context - 清理DP上下文
 */
static void
cleanup_dp_context(void)
{
	if (best_plan_memo != NULL)
	{
		hash_destroy(best_plan_memo);
		best_plan_memo = NULL;
	}
}

/*
 * bitmapset_hash_func - Bitmapset的hash函数
 */
static uint32
bitmapset_hash_func(const void *key, Size keysize)
{
	const MemoHashKey *mkey = (const MemoHashKey *) key;
	Bitmapset  *bms = bms_copy(mkey->tables);
	uint32		hash = 0;

	while (!bms_is_empty(bms))
	{
		int x = bms_first_member(bms);
		hash = hash * 31 + x;
	}
	bms_free(bms);

	return hash;
}

/*
 * bitmapset_compare_func - Bitmapset的比较函数
 */
static int
bitmapset_compare_func(const void *key1, const void *key2, Size keysize)
{
	const MemoHashKey *mkey1 = (const MemoHashKey *) key1;
	const MemoHashKey *mkey2 = (const MemoHashKey *) key2;

	return bms_equal(mkey1->tables, mkey2->tables) ? 0 : 1;
}
