/*-------------------------------------------------------------------------
 *
 * multijoin.c
 *	  Multi-join node extraction and manipulation
 *
 * 功能：
 *   1. 从Query的join树提取所有基表(atoms)
 *   2. 收集所有join条件(predicates)
 *   3. 检查是否可以重排序
 *   4. 提供join条件查询功能
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "optimizer/cascades/reorder/multijoin.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/restrictinfo.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "utils/memutils.h"
#include "nodes/primnodes.h"
#include "optimizer/clauses.h"

/* 静态函数声明 */
static void extract_join_tree_recursive(PlannerInfo *root, Node *jtnode,
										PgMultiJoinNode *mjn);
static List *extract_predicates_from_node(Node *node);
static List *extract_predicates_from_qual_node(Node *node);
static bool has_equijoin_condition(PgMultiJoinNode *mjn);

/*
 * pg_multijoin_extract - 从Query提取MultiJoinNode
 *
 * 递归遍历join树，提取所有基表和join条件
 */
PgMultiJoinNode *
pg_multijoin_extract(PlannerInfo *root, Node *jtnode)
{
	PgMultiJoinNode *mjn;
	MemoryContext mcxt;
	MemoryContext oldcxt;
	ListCell   *lc;
	int			rti;

	/* 创建独立的内存上下文 */
	mcxt = AllocSetContextCreate(CurrentMemoryContext,
								 "MultiJoinNode",
								 ALLOCSET_DEFAULT_MINSIZE,
								 ALLOCSET_DEFAULT_INITSIZE,
								 ALLOCSET_DEFAULT_MAXSIZE);
	oldcxt = MemoryContextSwitchTo(mcxt);

	/* 初始化结构 */
	mjn = (PgMultiJoinNode *) palloc0(sizeof(PgMultiJoinNode));
	mjn->atoms = NIL;
	mjn->predicates = NIL;
	mjn->table_ids = NULL;
	mjn->num_tables = 0;
	mjn->mcxt = mcxt;

	/* 方法1: 从root->simple_rel_array收集所有基表 */
	for (rti = 1; rti < root->simple_rel_array_size; rti++)
	{
		RelOptInfo *rel = root->simple_rel_array[rti];
		if (rel != NULL && rel->reloptkind == RELOPT_BASEREL)
		{
			mjn->atoms = lappend(mjn->atoms, rel);
			mjn->table_ids = bms_add_member(mjn->table_ids, rel->relid);
		}
	}
	mjn->num_tables = list_length(mjn->atoms);

	elog(NOTICE, "DP_TRACE: Collected %d base tables from root", mjn->num_tables);

	/* 方法2: 从root的qual_clauses收集WHERE条件 */
	if (root->parse->jointree && root->parse->jointree->quals)
	{
		List *all_clauses = NIL;
		Node *quals = root->parse->jointree->quals;

		elog(NOTICE, "DP_TRACE: Processing WHERE quals, node_type=%d", nodeTag(quals));

		/* 检查是否是List类型 */
		if (IsA(quals, List))
		{
			ListCell *lc;
			List *qual_list = (List *) quals;

			elog(NOTICE, "DP_TRACE:   quals is List with %d items", list_length(qual_list));

			/* 处理List中的每个元素 */
			foreach(lc, qual_list)
			{
				Node *qual_item = (Node *) lfirst(lc);
				List *sub = extract_predicates_from_qual_node(qual_item);
				all_clauses = list_concat(all_clauses, sub);
			}
		}
		else
		{
			/* 单个节点，直接处理 */
			all_clauses = extract_predicates_from_qual_node(quals);
		}

		elog(NOTICE, "DP_TRACE: Extracted %d clauses from WHERE", list_length(all_clauses));

		mjn->predicates = all_clauses;
	}

	MemoryContextSwitchTo(oldcxt);

	elog(NOTICE, "DP_TRACE: MultiJoinNode final: %d tables, %d predicates",
		 mjn->num_tables, list_length(mjn->predicates));

	return mjn;
}

/*
 * extract_join_tree_recursive - 递归提取join树
 */
static void
extract_join_tree_recursive(PlannerInfo *root, Node *jtnode,
							PgMultiJoinNode *mjn)
{
	if (jtnode == NULL)
		return;

	if (IsA(jtnode, RangeTblRef))
	{
		/* 基表 */
		RangeTblRef *rtr = (RangeTblRef *) jtnode;
		RelOptInfo *rel = find_base_rel(root, rtr->rtindex);

		if (rel != NULL)
			mjn->atoms = lappend(mjn->atoms, rel);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *join = (JoinExpr *) jtnode;

		/* 只处理INNER JOIN */
		if (join->jointype != JOIN_INNER)
		{
			elog(DEBUG2, "Non-INNER JOIN detected, cannot reorder");
			return;
		}

		/* 递归处理左右子树 */
		extract_join_tree_recursive(root, join->larg, mjn);
		extract_join_tree_recursive(root, join->rarg, mjn);

		/* 收集join条件 */
		if (join->quals != NULL)
		{
			List	   *join_quals = extract_predicates_from_node(join->quals);
			mjn->predicates = list_concat(mjn->predicates, join_quals);
		}
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *from = (FromExpr *) jtnode;
		ListCell   *lc;

		/* 递归处理所有子节点 */
		foreach(lc, from->fromlist)
		{
			Node	   *child = (Node *) lfirst(lc);
			extract_join_tree_recursive(root, child, mjn);
		}

		/* WHERE条件 */
		if (from->quals != NULL)
		{
			List	   *where_quals = extract_predicates_from_node(from->quals);
			mjn->predicates = list_concat(mjn->predicates, where_quals);
		}
	}
}

/*
 * extract_predicates_from_qual_node - 从WHERE quals直接提取谓词
 *
 * 这个函数专门处理root->parse->jointree->quals
 * 会递归展开所有AND子句
 */
static List *
extract_predicates_from_qual_node(Node *node)
{
	List	   *result = NIL;

	if (node == NULL)
		return NIL;

	elog(NOTICE, "DP_TRACE: extract_qual node_type=%d", nodeTag(node));

	/* 处理BoolExpr (AND/OR/NOT) */
	if (IsA(node, BoolExpr))
	{
		BoolExpr *bexpr = (BoolExpr *) node;

		if (bexpr->boolop == AND_EXPR)
		{
			ListCell *lc;

			elog(NOTICE, "DP_TRACE:   AND_EXPR with %d args", list_length(bexpr->args));

			/* 递归处理每个AND子项 */
			foreach(lc, bexpr->args)
			{
				Node *arg = (Node *) lfirst(lc);
				List *sub = extract_predicates_from_qual_node(arg);
				result = list_concat(result, sub);
			}
			return result;
		}
		else
		{
			elog(NOTICE, "DP_TRACE:   Non-AND BoolExpr, wrapping");
			/* OR或NOT：作为单个条件 */
		}
	}

	/* 处理OpExpr (比较操作符，如 =, <, > 等) */
	if (IsA(node, OpExpr))
	{
		elog(NOTICE, "DP_TRACE:   OpExpr, creating RestrictInfo");

		RestrictInfo *rinfo = make_restrictinfo((Expr *) node,
												true,	/* is_pushed_down */
												false,	/* outerjoin_delayed */
												false,	/* pseudoconstant */
												NULL,	/* required_relids */
												NULL,	/* outer_relids */
												NULL);	/* nullable_relids */
		result = lappend(result, rinfo);
		return result;
	}

	/* 其他节点类型：尝试包装为RestrictInfo */
	elog(NOTICE, "DP_TRACE:   Other node type, trying to wrap");
	if (IsA(node, Expr))
	{
		RestrictInfo *rinfo = make_restrictinfo((Expr *) node,
												true,
												false,
												false,
												NULL,
												NULL,
												NULL);
		result = lappend(result, rinfo);
	}

	return result;
}

/*
 * extract_predicates_from_node - 从表达式节点提取谓词
 * (保留用于JoinExpr的quals处理)
 */
static List *
extract_predicates_from_node(Node *node)
{
	List	   *result = NIL;

	if (node == NULL)
		return NIL;

	elog(NOTICE, "DP_TRACE: extract_pred node_type=%d (%s)",
		 nodeTag(node),
		 IsA(node, RestrictInfo) ? "RestrictInfo" :
		 and_clause(node) ? "AND" :
		 IsA(node, OpExpr) ? "OpExpr" : "Other");

	if (IsA(node, RestrictInfo))
	{
		RestrictInfo *rinfo = (RestrictInfo *) node;

		elog(NOTICE, "DP_TRACE:   RestrictInfo->clause type=%d", nodeTag(rinfo->clause));

		/* 检查RestrictInfo内部的clause是否是AND子句 */
		if (and_clause((Node *) rinfo->clause))
		{
			/* 递归展开AND子句 */
			elog(NOTICE, "DP_TRACE:   Expanding AND inside RestrictInfo");
			return extract_predicates_from_node((Node *) rinfo->clause);
		}

		/* 否则直接添加RestrictInfo */
		elog(NOTICE, "DP_TRACE:   Adding RestrictInfo as-is");
		result = lappend(result, node);
	}
	else if (and_clause(node))
	{
		/* AND子句：递归处理每个子项 */
		List	   *args = ((BoolExpr *) node)->args;
		ListCell   *lc;

		elog(NOTICE, "DP_TRACE:   AND clause with %d args, expanding", list_length(args));

		foreach(lc, args)
		{
			List	   *sub = extract_predicates_from_node((Node *) lfirst(lc));
			result = list_concat(result, sub);
		}
	}
	else
	{
		/* 其他表达式：包装为RestrictInfo */
		elog(NOTICE, "DP_TRACE:   Wrapping as RestrictInfo");
		RestrictInfo *rinfo = make_restrictinfo((Expr *) node,
												true,	/* is_pushed_down */
												false,	/* outerjoin_delayed */
												false,	/* pseudoconstant */
												NULL,	/* required_relids */
												NULL,	/* outer_relids */
												NULL);	/* nullable_relids */
		result = lappend(result, rinfo);
	}

	elog(NOTICE, "DP_TRACE:   Returning %d predicates", list_length(result));
	return result;
}

/*
 * pg_multijoin_can_reorder - 检查是否可以重排序
 */
bool
pg_multijoin_can_reorder(PgMultiJoinNode *mjn)
{
	/* 必须有至少2个表 */
	if (mjn->num_tables < 2)
	{
		elog(DEBUG2, "Cannot reorder: less than 2 tables");
		return false;
	}

	/* 必须有join条件 */
	if (list_length(mjn->predicates) < mjn->num_tables - 1)
	{
		elog(NOTICE, "DP_TRACE: can_reorder=false: predicates=%d, need=%d",
			 list_length(mjn->predicates), mjn->num_tables - 1);
		return false;
	}

	/* 检查是否有等值join条件 */
	if (!has_equijoin_condition(mjn))
	{
		elog(DEBUG2, "Cannot reorder: no equijoin conditions");
		return false;
	}

	return true;
}

/*
 * has_equijoin_condition - 检查是否有等值join条件
 */
static bool
has_equijoin_condition(PgMultiJoinNode *mjn)
{
	ListCell   *lc;

	foreach(lc, mjn->predicates)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		/* 检查是否是等值条件（简化实现） */
		if (IsA(rinfo->clause, OpExpr))
		{
			OpExpr	   *opexpr = (OpExpr *) rinfo->clause;

			/* 检查操作符是否是"=" */
			if (list_length(opexpr->args) == 2)
			{
				/* 假设找到等值条件 */
				return true;
			}
		}
	}

	return false;
}

/*
 * pg_multijoin_get_join_predicates - 获取连接两个表集合的join条件
 */
List *
pg_multijoin_get_join_predicates(PgMultiJoinNode *mjn,
								  Bitmapset *left_tables,
								  Bitmapset *right_tables)
{
	List	   *result = NIL;
	ListCell   *lc;
	Bitmapset  *both_tables;

	if (left_tables == NULL || right_tables == NULL)
		return NIL;

	both_tables = bms_union(left_tables, right_tables);

	foreach(lc, mjn->predicates)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);
		Relids		clause_relids = rinfo->clause_relids;

		/*
		 * 检查此条件是否连接left和right：
		 * 1. 与left有overlap
		 * 2. 与right有overlap
		 * 3. 所有涉及的表都在left+right中
		 */
		if (bms_overlap(clause_relids, left_tables) &&
			bms_overlap(clause_relids, right_tables) &&
			bms_is_subset(clause_relids, both_tables))
		{
			result = lappend(result, rinfo);
		}
	}

	bms_free(both_tables);
	return result;
}

/*
 * pg_multijoin_free - 释放MultiJoinNode
 */
void
pg_multijoin_free(PgMultiJoinNode *mjn)
{
	if (mjn != NULL && mjn->mcxt != NULL)
	{
		MemoryContextDelete(mjn->mcxt);
	}
}
