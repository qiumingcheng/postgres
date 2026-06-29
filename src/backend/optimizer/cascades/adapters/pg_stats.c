/*-------------------------------------------------------------------------
 * adapters/pg_stats.c
 *    PG 统计工具 — 从 pg_statistic 系统表读取列级统计信息
 *    (NDV, null fraction, histogram)，供 Cascades 代价估算使用。
 *    当前 memo.c 中已有基础统计推导；本文件预留给增强版统计。
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "optimizer/cascades.h"
