/*-------------------------------------------------------------------------
 * adapters/pg_plan.c
 *    PG Plan 构建工具 — planbuild.c 的补充工具函数。
 *    当前 plan 构建（create_plan → Plan*）在 planbuild.c 中，
 *    本文件预留给未来拆分出的 Plan 辅助函数。
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "optimizer/cascades.h"
