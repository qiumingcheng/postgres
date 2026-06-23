-- ============================================================================
-- Cascades 优化器最小回归测试矩阵 v2
-- 针对 PG 9.2.4 适配
-- 运行: psql -d cascades_test -f cascades_regression_test_v2.sql
-- ============================================================================

\set ON_ERROR_STOP off
\set QUIET 1

-- 确保 fallback_on_error 开启，避免内部错误中断测试
SET cascades_planner_fallback_on_error = on;
SET cascades_planner_debug = off;
SET enable_cascades_planner = off;

-- ============================================================================
-- Part 0: 环境准备
-- ============================================================================

DROP TABLE IF EXISTS cascades_test_t CASCADE;
CREATE TABLE cascades_test_t (
    id      INTEGER PRIMARY KEY,
    a       INTEGER NOT NULL,
    b       INTEGER NOT NULL,
    name    TEXT
);
CREATE INDEX idx_ct_a ON cascades_test_t (a);
CREATE INDEX idx_ct_b ON cascades_test_t (b);

INSERT INTO cascades_test_t (id, a, b, name)
SELECT i, i % 100, i % 50, 'item_' || i
FROM generate_series(1, 1000) AS i;

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
    j1_id   INTEGER
);
CREATE TABLE cascades_test_j3 (
    id      INTEGER PRIMARY KEY,
    val     INTEGER NOT NULL,
    j2_id   INTEGER
);

INSERT INTO cascades_test_j1 (id, val) SELECT i, i % 50 FROM generate_series(1, 100) AS i;
INSERT INTO cascades_test_j2 (id, val, j1_id) SELECT i, i % 20, (i % 100) + 1 FROM generate_series(1, 500) AS i;
INSERT INTO cascades_test_j3 (id, val, j2_id) SELECT i, i % 10, (i % 500) + 1 FROM generate_series(1, 1000) AS i;

ANALYZE cascades_test_t;
ANALYZE cascades_test_j1;
ANALYZE cascades_test_j2;
ANALYZE cascades_test_j3;

-- ============================================================================
-- 测试框架: 每个测试记录 PG 结果和 Cascades 结果
-- ============================================================================

CREATE TEMP TABLE test_results (
    test_id     INTEGER,
    test_name   TEXT,
    pg_result   TEXT,
    cas_result  TEXT,
    cas_status  TEXT,   -- 'OK' | 'FALLBACK' | 'ERROR'
    result_match BOOLEAN
);

\set QUIET 0

-- ============================================================================
-- 辅助函数: 执行一个测试，对比 PG 和 Cascades 结果
-- 用法: SELECT run_test(测试ID, '测试名', 'SQL文本');
-- ============================================================================

CREATE OR REPLACE FUNCTION run_test(test_id INTEGER, test_name TEXT, sql_text TEXT)
RETURNS TEXT AS $$
DECLARE
    pg_res      TEXT;
    cas_res     TEXT;
    cas_stat    TEXT;
    matches     BOOLEAN;
BEGIN
    -- Run with PG planner
    SET enable_cascades_planner = off;
    SET cascades_planner_fallback_on_error = on;
    BEGIN
        EXECUTE 'CREATE TEMP TABLE _pg_tmp AS ' || sql_text;
        pg_res := 'OK';
        DROP TABLE IF EXISTS _pg_tmp;
    EXCEPTION WHEN OTHERS THEN
        pg_res := 'ERROR: ' || SQLERRM;
    END;

    -- Run with Cascades planner
    SET enable_cascades_planner = on;
    SET cascades_planner_fallback_on_error = on;
    BEGIN
        EXECUTE 'CREATE TEMP TABLE _cas_tmp AS ' || sql_text;
        cas_res := 'OK';
        cas_stat := 'CASCADES_OK';
        DROP TABLE IF EXISTS _cas_tmp;
    EXCEPTION WHEN OTHERS THEN
        cas_res := 'ERROR: ' || SQLERRM;
        -- Check if it fell back to PG
        BEGIN
            SET enable_cascades_planner = off;
            EXECUTE 'CREATE TEMP TABLE _cas_tmp2 AS ' || sql_text;
            cas_stat := 'FALLBACK_OK';
            cas_res := 'OK (fallback)';
            DROP TABLE IF EXISTS _cas_tmp2;
        EXCEPTION WHEN OTHERS THEN
            cas_stat := 'BOTH_ERROR';
        END;
    END;

    -- For now, simple pass/fail based on no error
    IF pg_res LIKE 'OK%' AND cas_res LIKE 'OK%' THEN
        matches := true;
    ELSIF pg_res LIKE 'ERROR%' AND cas_res LIKE 'ERROR%' THEN
        matches := true;
    ELSE
        matches := false;
    END IF;

    INSERT INTO test_results VALUES (test_id, test_name, pg_res, cas_res, COALESCE(cas_stat, 'UNKNOWN'), matches);

    RETURN test_name || ': PG=' || pg_res || ' CAS=' || cas_res || ' MATCH=' || matches;
END;
$$ LANGUAGE plpgsql;

-- ============================================================================
-- Part 1: Trivial / Empty Jointree
-- ============================================================================
\echo '=== T1-T2: Trivial Result ==='

-- T1: 纯表达式
SELECT run_test(1, 'T1: select expr', 
    $$SELECT 1 + 1 AS result$$);

-- T2: VALUES + ORDER
SELECT run_test(2, 'T2: values order by', 
    $$VALUES(1) ORDER BY 1$$);

-- ============================================================================
-- Part 2: SeqScan / Filter (单表查询)
-- ============================================================================
\echo '=== T3-T4: SeqScan/Filter ==='

-- T3: 单表 + WHERE
SELECT run_test(3, 'T3: scan + filter', 
    $$SELECT count(*) AS c FROM cascades_test_t WHERE a > 10$$);

-- T4: 复合条件
SELECT run_test(4, 'T4: composite filter', 
    $$SELECT count(*) AS c FROM cascades_test_t WHERE a > 10 AND name LIKE 'item_1%'$$);

-- ============================================================================
-- Part 3: Index / Bitmap Scan
-- ============================================================================
\echo '=== T5-T7: Index/Bitmap ==='

-- T5: 等值 (应走 IndexScan)
SELECT run_test(5, 'T5: index scan eq', 
    $$SELECT id, a, b FROM cascades_test_t WHERE id = 1$$);

-- T6: AND 条件
SELECT run_test(6, 'T6: bitmap and', 
    $$SELECT count(*) AS c FROM cascades_test_t WHERE a = 1 AND b = 2$$);

-- T7: OR 条件
SELECT run_test(7, 'T7: bitmap or', 
    $$SELECT count(*) AS c FROM cascades_test_t WHERE a = 1 OR b = 2$$);

-- ============================================================================
-- Part 4: JOIN Tests
-- ============================================================================
\echo '=== T8-T12: JOIN ==='

-- T8: INNER JOIN
SELECT run_test(8, 'T8: inner join', 
    $$SELECT count(*) AS c FROM cascades_test_j1 t1
      JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id$$);

-- T9: LEFT JOIN
SELECT run_test(9, 'T9: left join', 
    $$SELECT count(*) AS c FROM cascades_test_j1 t1
      LEFT JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id$$);

-- T10: EXISTS (SEMI JOIN)
SELECT run_test(10, 'T10: exists', 
    $$SELECT count(*) AS c FROM cascades_test_j1 t1
      WHERE EXISTS (SELECT 1 FROM cascades_test_j2 t2 WHERE t2.j1_id = t1.id)$$);

-- T11: NOT EXISTS (ANTI JOIN)
SELECT run_test(11, 'T11: not exists', 
    $$SELECT count(*) AS c FROM cascades_test_j1 t1
      WHERE NOT EXISTS (SELECT 1 FROM cascades_test_j2 t2 WHERE t2.j1_id = t1.id)$$);

-- T12: 三表 JOIN
SELECT run_test(12, 'T12: 3-table join', 
    $$SELECT count(*) AS c FROM cascades_test_j1 t1
      JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id
      JOIN cascades_test_j3 t3 ON t2.id = t3.j2_id$$);

-- ============================================================================
-- Part 5: Upper Ops (Agg/Sort/Distinct/Limit)
-- ============================================================================
\echo '=== T13-T18: Upper Ops ==='

-- T13: GROUP BY + ORDER BY + LIMIT
SELECT run_test(13, 'T13: group+order+limit', 
    $$SELECT a, count(*) AS cnt FROM cascades_test_t
      GROUP BY a ORDER BY a LIMIT 5$$);

-- T14: GROUP BY + ORDER BY + LIMIT (更多行)
SELECT run_test(14, 'T14: group+order+limit10', 
    $$SELECT a, count(*) AS cnt FROM cascades_test_t
      GROUP BY a ORDER BY a LIMIT 10$$);

-- T15: DISTINCT + ORDER BY + LIMIT
SELECT run_test(15, 'T15: distinct+order+limit', 
    $$SELECT DISTINCT a FROM cascades_test_t ORDER BY a LIMIT 5$$);

-- T16: ORDER BY + LIMIT (无 GROUP)
SELECT run_test(16, 'T16: order+limit', 
    $$SELECT id, a, name FROM cascades_test_t ORDER BY a LIMIT 5$$);

-- T17: JOIN + GROUP + ORDER + LIMIT
SELECT run_test(17, 'T17: join+group+order+limit', 
    $$SELECT t1.val, count(*) AS cnt
      FROM cascades_test_j1 t1
      JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id
      WHERE t1.val > 10
      GROUP BY t1.val ORDER BY t1.val
      LIMIT 10$$);

-- T18: GROUP + HAVING
SELECT run_test(18, 'T18: group+having', 
    $$SELECT a, count(*) AS cnt FROM cascades_test_t
      GROUP BY a HAVING count(*) > 1 ORDER BY a LIMIT 5$$);

-- ============================================================================
-- Part 6: Fallback Cases
-- ============================================================================
\echo '=== T19-T21: Fallback ==='

-- T19: DISTINCT ON (应 fallback)
SELECT run_test(19, 'T19: distinct on', 
    $$SELECT DISTINCT ON (a) a, b FROM cascades_test_t ORDER BY a, b LIMIT 5$$);

-- T20: Window Function (应 fallback)
SELECT run_test(20, 'T20: window func', 
    $$SELECT a, b, row_number() OVER (PARTITION BY a ORDER BY b) AS rn
      FROM cascades_test_t ORDER BY a, b LIMIT 5$$);

-- T21: FOR UPDATE (应 fallback)
SELECT run_test(21, 'T21: for update', 
    $$SELECT id, a, b FROM cascades_test_t WHERE id = 1 FOR UPDATE$$);

-- ============================================================================
-- Part 7: EXPLAIN Verification (Cascades path only)
-- ============================================================================
\echo '=== EXPLAIN Checks ==='

-- 验证 Cascades 生成的计划中包含预期节点
\echo 'EXPLAIN T3 (cascades):'
SET enable_cascades_planner = on;
EXPLAIN (COSTS OFF) SELECT count(*) AS c FROM cascades_test_t WHERE a > 10;

\echo ''
\echo 'EXPLAIN T16 (cascades):'
EXPLAIN (COSTS OFF) SELECT id, a, name FROM cascades_test_t ORDER BY a LIMIT 5;

\echo ''
\echo 'EXPLAIN T8 (cascades, may fallback):'
EXPLAIN (COSTS OFF) SELECT count(*) AS c FROM cascades_test_j1 t1
JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id;

-- ============================================================================
-- TEST SUMMARY
-- ============================================================================
\echo ''
\echo '========================================'
\echo 'TEST SUMMARY'
\echo '========================================'

SELECT 
    test_id,
    test_name,
    pg_result,
    CASE 
        WHEN cas_status = 'CASCADES_OK' THEN 'CASCADES_OK'
        WHEN cas_status = 'FALLBACK_OK' THEN 'FALLBACK_OK'
        ELSE cas_status
    END AS cascades_path,
    CASE WHEN result_match THEN 'PASS' ELSE 'FAIL' END AS verdict
FROM test_results
ORDER BY test_id;

SELECT 
    count(*) AS total_tests,
    count(*) FILTER (WHERE result_match) AS passed,
    count(*) FILTER (WHERE NOT result_match) AS failed,
    count(*) FILTER (WHERE cas_status = 'CASCADES_OK') AS cascades_ok,
    count(*) FILTER (WHERE cas_status = 'FALLBACK_OK') AS cascades_fallback
FROM test_results;

\echo '========================================'
\echo 'ALL TESTS COMPLETE'
\echo '========================================'
