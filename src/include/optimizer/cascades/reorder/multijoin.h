/*-------------------------------------------------------------------------
 *
 * multijoin.h
 *	  Definitions for multi-join node extraction and manipulation
 *
 * Copyright (c) 2026, PostgreSQL Global Development Group
 *
 *-------------------------------------------------------------------------
 */
#ifndef CASCADES_MULTIJOIN_H
#define CASCADES_MULTIJOIN_H

#include "nodes/pg_list.h"
#include "nodes/bitmapset.h"
#include "nodes/relation.h"

/*
 * PgMultiJoinNode - 展平的inner join树表示
 *
 * 包含所有基表(atoms)和join条件(predicates)的扁平化表示
 * 用于后续的join枚举算法
 */
typedef struct PgMultiJoinNode
{
	List	   *atoms;			/* List of RelOptInfo* - 基表列表 */
	List	   *predicates;		/* List of RestrictInfo* - join条件 */
	Bitmapset  *table_ids;		/* 表ID位集合，用于快速操作 */
	int			num_tables;		/* 表数量 */
	MemoryContext mcxt;			/* 内存上下文 */
} PgMultiJoinNode;

/* 主要函数 */
extern PgMultiJoinNode *pg_multijoin_extract(PlannerInfo *root, Node *jtnode);
extern bool pg_multijoin_can_reorder(PgMultiJoinNode *mjn);
extern List *pg_multijoin_get_join_predicates(PgMultiJoinNode *mjn,
											   Bitmapset *left_tables,
											   Bitmapset *right_tables);
extern void pg_multijoin_free(PgMultiJoinNode *mjn);

#endif							/* CASCADES_MULTIJOIN_H */
