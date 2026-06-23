-- ============================================================================
-- Cascades 优化器最小回归测试矩阵 (基于 postgres_cascades_migration_plan.md §14.6)
-- ============================================================================
-- 运行方式:
--   psql -d cascades_test -f cascades_regression_test.sql
--
-- 对比方式:
--   1. 同一 SQL 分别设置 enable_cascades_planner off/on
--   2. 对 SELECT 结果做 EXCEPT 双向比较
--   3. 对 EXPLAIN 检查 Cascades 路径是否生效
--   4. 对 fallback SQL 检查正确回退到 PG 原 planner
-- ============================================================================

-- ============================================================================
-- Part 0: 环境准备 - 确保测试表和数据存在
-- ============================================================================

-- 清理并重建测试表 t（单表测试用：有索引的通用表）
DROP TABLE IF EXISTS cascades_test_t CASCADE;
CREATE TABLE cascades_test_t (
    id      INTEGER PRIMARY KEY,
    a       INTEGER NOT NULL,
    b       INTEGER NOT NULL,
    name    TEXT
);
CREATE INDEX idx_ct_a ON cascades_test_t (a);
CREATE INDEX idx_ct_b ON cascades_test_t (b);

-- 插入测试数据（1000 行）
INSERT INTO cascades_test_t (id, a, b, name)
SELECT i, i % 100, i % 50, 'item_' || i
FROM generate_series(1, 1000) AS i;

-- 多表 join 测试表
DROP TABLE IF EXISTS cascades_test_j1 CASCADE;
DROP TABLE IF EXISTS cascades_test_j2 CASCADE;
DROP TABLE IF EXISTS cascades_test_j3 CASCADE;

CREATE TABLE cascades_test_j1 (
    id      INTEGER PRIMARY KEY,
    val     INTEGER NOT NULL
);
CREATE TABLE cascades_test_j2 (
    id      INTEGER PRIMARY KEY,
    val     INTEGER NOT NULL,
    j1_id   INTEGER REFERENCES cascades_test_j1(id)
);
CREATE TABLE cascades_test_j3 (
    id      INTEGER PRIMARY KEY,
    val     INTEGER NOT NULL,
    j2_id   INTEGER REFERENCES cascades_test_j2(id)
);

INSERT INTO cascades_test_j1 (id, val) SELECT i, i % 50 FROM generate_series(1, 100) AS i;
INSERT INTO cascades_test_j2 (id, val, j1_id) SELECT i, i % 20, (i % 100) + 1 FROM generate_series(1, 500) AS i;
INSERT INTO cascades_test_j3 (id, val, j2_id) SELECT i, i % 10, (i % 500) + 1 FROM generate_series(1, 1000) AS i;

-- 分析统计信息
ANALYZE cascades_test_t;
ANALYZE cascades_test_j1;
ANALYZE cascades_test_j2;
ANALYZE cascades_test_j3;

-- ============================================================================
-- Part 1: 辅助函数 - 用于结果对比
-- ============================================================================

-- 创建临时表来存储两个 planner 的结果
CREATE TEMP TABLE IF NOT EXISTS _pg_result AS SELECT 1::int AS dummy LIMIT 0;
CREATE TEMP TABLE IF NOT EXISTS _cas_result AS SELECT 1::int AS dummy LIMIT 0;

-- ============================================================================
-- Part 2: 测试用例 - 设计文档 §14.6 最小回归矩阵
-- ============================================================================

\echo '========================================'
\echo 'Part 2.1: Trivial Result / Empty Jointree'
\echo '========================================'

-- T1: 纯表达式，无 FROM
\echo 'T1: select 1 + 1'
SET enable_cascades_planner = off;
SELECT 1 + 1 AS result;
SET enable_cascades_planner = on;
SELECT 1 + 1 AS result;

-- T2: 空 FROM (PostgreSQL 9.2 不支持 SELECT without FROM，用 VALUES 模拟)
\echo 'T2: values(1) order by 1'  
SET enable_cascades_planner = off;
VALUES(1) ORDER BY 1;
SET enable_cascades_planner = on;
VALUES(1) ORDER BY 1;

\echo '========================================'
\echo 'Part 2.2: SeqScan / Filter'
\echo '========================================'

-- T3: 带 WHERE 条件的单表查询
\echo 'T3: select * from cascades_test_t where a > 10'
SET enable_cascades_planner = off;
SELECT count(*) FROM cascades_test_t WHERE a > 10;
SET enable_cascades_planner = on;
SELECT count(*) FROM cascades_test_t WHERE a > 10;

-- T4: 复合条件
\echo 'T4: select * from cascades_test_t where a > 10 and name like ''item_1%'''
SET enable_cascades_planner = off;
SELECT count(*) FROM cascades_test_t WHERE a > 10 AND name LIKE 'item_1%';
SET enable_cascades_planner = on;
SELECT count(*) FROM cascades_test_t WHERE a > 10 AND name LIKE 'item_1%';

\echo '========================================'
\echo 'Part 2.3: Index / Bitmap Scan'
\echo '========================================'

-- T5: 等值条件 (应使用 IndexScan)
\echo 'T5: select * from cascades_test_t where id = 1'
SET enable_cascades_planner = off;
SELECT id, a, b FROM cascades_test_t WHERE id = 1;
SET enable_cascades_planner = on;
SELECT id, a, b FROM cascades_test_t WHERE id = 1;

-- T6: 两个列上的 AND 条件 (可能触发 BitmapAnd)
\echo 'T6: select * from cascades_test_t where a = 1 and b = 2'
SET enable_cascades_planner = off;
SELECT count(*) FROM cascades_test_t WHERE a = 1 AND b = 2;
SET enable_cascades_planner = on;
SELECT count(*) FROM cascades_test_t WHERE a = 1 AND b = 2;

-- T7: OR 条件 (可能触发 BitmapOr)
\echo 'T7: select * from cascades_test_t where a = 1 or b = 2'
SET enable_cascades_planner = off;
SELECT count(*) FROM cascades_test_t WHERE a = 1 OR b = 2;
SET enable_cascades_planner = on;
SELECT count(*) FROM cascades_test_t WHERE a = 1 OR b = 2;

\echo '========================================'
\echo 'Part 2.4: JOIN Tests'
\echo '========================================'

-- T8: INNER JOIN (两表)
\echo 'T8: inner join'
SET enable_cascades_planner = off;
SELECT count(*) FROM cascades_test_j1 t1
JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id;
SET enable_cascades_planner = on;
SELECT count(*) FROM cascades_test_j1 t1
JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id;

-- T9: LEFT JOIN
\echo 'T9: left join'
SET enable_cascades_planner = off;
SELECT count(*) FROM cascades_test_j1 t1
LEFT JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id;
SET enable_cascades_planner = on;
SELECT count(*) FROM cascades_test_j1 t1
LEFT JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id;

-- T10: EXISTS (SEMI JOIN after pull_up_sublinks)
\echo 'T10: exists'
SET enable_cascades_planner = off;
SELECT count(*) FROM cascades_test_j1 t1
WHERE EXISTS (SELECT 1 FROM cascades_test_j2 t2 WHERE t2.j1_id = t1.id);
SET enable_cascades_planner = on;
SELECT count(*) FROM cascades_test_j1 t1
WHERE EXISTS (SELECT 1 FROM cascades_test_j2 t2 WHERE t2.j1_id = t1.id);

-- T11: NOT EXISTS (ANTI JOIN)
\echo 'T11: not exists'
SET enable_cascades_planner = off;
SELECT count(*) FROM cascades_test_j1 t1
WHERE NOT EXISTS (SELECT 1 FROM cascades_test_j2 t2 WHERE t2.j1_id = t1.id);
SET enable_cascades_planner = on;
SELECT count(*) FROM cascades_test_j1 t1
WHERE NOT EXISTS (SELECT 1 FROM cascades_test_j2 t2 WHERE t2.j1_id = t1.id);

-- T12: 三表 JOIN
\echo 'T12: three-table join'
SET enable_cascades_planner = off;
SELECT count(*) FROM cascades_test_j1 t1
JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id
JOIN cascades_test_j3 t3 ON t2.id = t3.j2_id;
SET enable_cascades_planner = on;
SELECT count(*) FROM cascades_test_j1 t1
JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id
JOIN cascades_test_j3 t3 ON t2.id = t3.j2_id;

\echo '========================================'
\echo 'Part 2.5: Upper Ops (Agg/Sort/Distinct/Limit)'
\echo '========================================'

-- T13: GROUP BY with aggregation
\echo 'T13: group by + count'
SET enable_cascades_planner = off;
SELECT a, count(*) AS cnt FROM cascades_test_t GROUP BY a ORDER BY a LIMIT 5;
SET enable_cascades_planner = on;
SELECT a, count(*) AS cnt FROM cascades_test_t GROUP BY a ORDER BY a LIMIT 5;

-- T14: GROUP BY + ORDER BY + LIMIT
\echo 'T14: group by + order by + limit'
SET enable_cascades_planner = off;
SELECT a, count(*) AS cnt FROM cascades_test_t GROUP BY a ORDER BY a LIMIT 10;
SET enable_cascades_planner = on;
SELECT a, count(*) AS cnt FROM cascades_test_t GROUP BY a ORDER BY a LIMIT 10;

-- T15: DISTINCT + ORDER BY
\echo 'T15: distinct + order by'
SET enable_cascades_planner = off;
SELECT DISTINCT a FROM cascades_test_t ORDER BY a LIMIT 5;
SET enable_cascades_planner = on;
SELECT DISTINCT a FROM cascades_test_t ORDER BY a LIMIT 5;

-- T16: ORDER BY + LIMIT (无 GROUP)
\echo 'T16: order by + limit'
SET enable_cascades_planner = off;
SELECT id, a, name FROM cascades_test_t ORDER BY a LIMIT 5;
SET enable_cascades_planner = on;
SELECT id, a, name FROM cascades_test_t ORDER BY a LIMIT 5;

-- T17: 复杂 upper: JOIN + GROUP + ORDER + LIMIT
\echo 'T17: join + group + order + limit'
SET enable_cascades_planner = off;
SELECT t1.val, count(*) AS cnt
FROM cascades_test_j1 t1
JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id
WHERE t1.val > 10
GROUP BY t1.val
ORDER BY t1.val
LIMIT 10;
SET enable_cascades_planner = on;
SELECT t1.val, count(*) AS cnt
FROM cascades_test_j1 t1
JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id
WHERE t1.val > 10
GROUP BY t1.val
ORDER BY t1.val
LIMIT 10;

-- T18: HAVING
\echo 'T18: group by + having'
SET enable_cascades_planner = off;
SELECT a, count(*) AS cnt FROM cascades_test_t GROUP BY a HAVING count(*) > 1 ORDER BY a LIMIT 5;
SET enable_cascades_planner = on;
SELECT a, count(*) AS cnt FROM cascades_test_t GROUP BY a HAVING count(*) > 1 ORDER BY a LIMIT 5;

\echo '========================================'
\echo 'Part 2.6: Fallback Cases'
\echo '========================================'

-- T19: DISTINCT ON (Cascades should fallback)
\echo 'T19: distinct on (fallback expected)'
SET enable_cascades_planner = off;
SELECT DISTINCT ON (a) a, b FROM cascades_test_t ORDER BY a, b LIMIT 5;
SET enable_cascades_planner = on;
SELECT DISTINCT ON (a) a, b FROM cascades_test_t ORDER BY a, b LIMIT 5;

-- T20: Window Function (Cascades should fallback)
\echo 'T20: window function (fallback expected)'
SET enable_cascades_planner = off;
SELECT a, b, row_number() OVER (PARTITION BY a ORDER BY b) AS rn
FROM cascades_test_t ORDER BY a, b LIMIT 5;
SET enable_cascades_planner = on;
SELECT a, b, row_number() OVER (PARTITION BY a ORDER BY b) AS rn
FROM cascades_test_t ORDER BY a, b LIMIT 5;

-- T21: FOR UPDATE (Cascades should fallback)
\echo 'T21: for update (fallback expected)'
SET enable_cascades_planner = off;
SELECT * FROM cascades_test_t WHERE id = 1 FOR UPDATE;
SET enable_cascades_planner = on;
SELECT * FROM cascades_test_t WHERE id = 1 FOR UPDATE;

\echo '========================================'
\echo 'Part 2.7: EXPLAIN checks (verify Cascades path)'
\echo '========================================'

-- 验证 Cascades 计划中是否包含预期的节点类型
\echo 'EXPLAIN T3: seqscan/filter'
SET enable_cascades_planner = on;
EXPLAIN (COSTS OFF) SELECT count(*) FROM cascades_test_t WHERE a > 10;

\echo 'EXPLAIN T8: inner join'
SET enable_cascades_planner = on;
EXPLAIN (COSTS OFF) SELECT count(*) FROM cascades_test_j1 t1
JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id;

\echo 'EXPLAIN T13: group + order + limit'
SET enable_cascades_planner = on;
EXPLAIN (COSTS OFF) SELECT a, count(*) AS cnt FROM cascades_test_t
GROUP BY a ORDER BY a LIMIT 10;

\echo 'EXPLAIN T17: join + group + order + limit'
SET enable_cascades_planner = on;
EXPLAIN (COSTS OFF) SELECT t1.val, count(*) AS cnt
FROM cascades_test_j1 t1
JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id
WHERE t1.val > 10
GROUP BY t1.val
ORDER BY t1.val LIMIT 10;

\echo '========================================'
\echo 'Part 2.8: Result Correctness Verification'
\echo '========================================'

-- 使用 EXCEPT 双向对比确保结果一致
-- T3 结果对比
\echo 'T3 correctness: cascades vs pg (should be empty)'
CREATE TEMP TABLE _pg_t3 AS SELECT count(*) AS c FROM cascades_test_t WHERE a > 10;
SET enable_cascades_planner = off;
TRUNCATE _pg_t3; INSERT INTO _pg_t3 SELECT count(*) AS c FROM cascades_test_t WHERE a > 10;
SET enable_cascades_planner = on;
CREATE TEMP TABLE _cas_t3 AS SELECT count(*) AS c FROM cascades_test_t WHERE a > 10;

SELECT 'T3 MISMATCH (cas - pg)' AS test, * FROM (
    SELECT * FROM _cas_t3 EXCEPT SELECT * FROM _pg_t3
) sub
UNION ALL
SELECT 'T3 MISMATCH (pg - cas)' AS test, * FROM (
    SELECT * FROM _pg_t3 EXCEPT SELECT * FROM _cas_t3
) sub;
DROP TABLE _pg_t3; DROP TABLE _cas_t3;

-- T13 结果对比
\echo 'T13 correctness: cascades vs pg'
SET enable_cascades_planner = off;
CREATE TEMP TABLE _pg_t13 AS SELECT a, count(*) AS cnt FROM cascades_test_t GROUP BY a ORDER BY a LIMIT 5;
SET enable_cascades_planner = on;
CREATE TEMP TABLE _cas_t13 AS SELECT a, count(*) AS cnt FROM cascades_test_t GROUP BY a ORDER BY a LIMIT 5;

SELECT 'T13 MISMATCH (cas - pg)' AS test, * FROM (
    SELECT * FROM _cas_t13 EXCEPT SELECT * FROM _pg_t13
) sub
UNION ALL
SELECT 'T13 MISMATCH (pg - cas)' AS test, * FROM (
    SELECT * FROM _pg_t13 EXCEPT SELECT * FROM _cas_t13
) sub;
DROP TABLE _pg_t13; DROP TABLE _cas_t13;

-- T17 结果对比
\echo 'T17 correctness: cascades vs pg'
SET enable_cascades_planner = off;
CREATE TEMP TABLE _pg_t17 AS
SELECT t1.val, count(*) AS cnt
FROM cascades_test_j1 t1
JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id
WHERE t1.val > 10
GROUP BY t1.val ORDER BY t1.val LIMIT 10;
SET enable_cascades_planner = on;
CREATE TEMP TABLE _cas_t17 AS
SELECT t1.val, count(*) AS cnt
FROM cascades_test_j1 t1
JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id
WHERE t1.val > 10
GROUP BY t1.val ORDER BY t1.val LIMIT 10;

SELECT 'T17 MISMATCH (cas - pg)' AS test, * FROM (
    SELECT * FROM _cas_t17 EXCEPT SELECT * FROM _pg_t17
) sub
UNION ALL
SELECT 'T17 MISMATCH (pg - cas)' AS test, * FROM (
    SELECT * FROM _pg_t17 EXCEPT SELECT * FROM _cas_t17
) sub;
DROP TABLE _pg_t17; DROP TABLE _cas_t17;

\echo '========================================'
\echo 'Part 2.9: Debug Memo Dump (if cascades_planner_debug=on)'
\echo '========================================'

SET cascades_planner_debug = on;
SET client_min_messages = notice;
\echo 'T17 with debug:'
SET enable_cascades_planner = on;
SELECT t1.val, count(*) AS cnt
FROM cascades_test_j1 t1
JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id
WHERE t1.val > 10
GROUP BY t1.val ORDER BY t1.val LIMIT 5;

SET cascades_planner_debug = off;
SET client_min_messages = warning;

\echo '========================================'
\echo 'ALL TESTS COMPLETE'
\echo '========================================'
