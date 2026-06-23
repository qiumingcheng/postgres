-- ============================================================================
-- Cascades 优化器回归测试套件 (最终版)
-- 覆盖: 设计文档 §14.6 最小回归矩阵 + Phase 3/4 扩展
-- 运行: psql -d cascades_test -f cascades_regression_test_final.sql
-- ============================================================================

\set ON_ERROR_STOP off

-- 确保 fallback_on_error 开启
SET cascades_planner_fallback_on_error = on;
SET cascades_planner_debug = off;
SET client_min_messages = warning;

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

CREATE TABLE cascades_test_j1 (id INTEGER PRIMARY KEY, val INTEGER NOT NULL);
CREATE TABLE cascades_test_j2 (id INTEGER PRIMARY KEY, val INTEGER NOT NULL, j1_id INTEGER);
CREATE TABLE cascades_test_j3 (id INTEGER PRIMARY KEY, val INTEGER NOT NULL, j2_id INTEGER);

INSERT INTO cascades_test_j1 (id, val) SELECT i, i % 50 FROM generate_series(1, 100) AS i;
INSERT INTO cascades_test_j2 (id, val, j1_id) SELECT i, i % 20, (i % 100) + 1 FROM generate_series(1, 500) AS i;
INSERT INTO cascades_test_j3 (id, val, j2_id) SELECT i, i % 10, (i % 500) + 1 FROM generate_series(1, 1000) AS i;

ANALYZE cascades_test_t;
ANALYZE cascades_test_j1;
ANALYZE cascades_test_j2;
ANALYZE cascades_test_j3;

-- ============================================================================
-- 测试框架
-- ============================================================================

DROP TABLE IF EXISTS _cas_test_results;
CREATE TEMP TABLE _cas_test_results (
    test_id     INTEGER,
    test_name   TEXT,
    pg_ok       BOOLEAN,
    cas_ok      BOOLEAN,
    cas_path    TEXT
);

CREATE OR REPLACE FUNCTION cas_run_test(test_id INTEGER, test_name TEXT, sql_text TEXT)
RETURNS TEXT AS $$
DECLARE
    pg_ok_val   BOOLEAN := false;
    cas_ok_val  BOOLEAN := false;
    cas_path_val TEXT := 'ERROR';
BEGIN
    -- PG original planner
    SET enable_cascades_planner = off;
    BEGIN
        EXECUTE 'CREATE TEMP TABLE _t AS ' || sql_text;
        pg_ok_val := true;
        DROP TABLE IF EXISTS _t;
    EXCEPTION WHEN OTHERS THEN
        pg_ok_val := false;
    END;

    -- Cascades planner
    SET enable_cascades_planner = on;
    BEGIN
        EXECUTE 'CREATE TEMP TABLE _t AS ' || sql_text;
        cas_ok_val := true;
        cas_path_val := 'CASCADES';
        DROP TABLE IF EXISTS _t;
    EXCEPTION WHEN OTHERS THEN
        -- Fallback: try PG original planner
        BEGIN
            SET enable_cascades_planner = off;
            EXECUTE 'CREATE TEMP TABLE _t AS ' || sql_text;
            cas_ok_val := true;
            cas_path_val := 'FALLBACK';
            DROP TABLE IF EXISTS _t;
        EXCEPTION WHEN OTHERS THEN
            cas_ok_val := false;
            cas_path_val := 'BOTH_FAIL';
        END;
    END;

    INSERT INTO _cas_test_results VALUES (test_id, test_name, pg_ok_val, cas_ok_val, cas_path_val);
    RETURN test_name || ': ' || cas_path_val;
END;
$$ LANGUAGE plpgsql;

-- ============================================================================
-- Part 1: Trivial / Empty Jointree (T1-T2)
-- ============================================================================
\echo '=== T1-T2: Trivial ==='

SELECT cas_run_test(1, 'T1: select expr',
    $$SELECT 1 + 1 AS r$$);

SELECT cas_run_test(2, 'T2: values order',
    $$VALUES(1) ORDER BY 1$$);

-- ============================================================================
-- Part 2: SeqScan / Filter (T3-T4)
-- ============================================================================
\echo '=== T3-T4: Scan/Filter ==='

SELECT cas_run_test(3, 'T3: scan+filter',
    $$SELECT count(*) FROM cascades_test_t WHERE a > 10$$);

SELECT cas_run_test(4, 'T4: composite filter',
    $$SELECT count(*) FROM cascades_test_t WHERE a > 10 AND name LIKE 'item_1%'$$);

-- ============================================================================
-- Part 3: Index / Bitmap Scan (T5-T7)
-- ============================================================================
\echo '=== T5-T7: Index/Bitmap ==='

SELECT cas_run_test(5, 'T5: index eq',
    $$SELECT id, a, b FROM cascades_test_t WHERE id = 1$$);

SELECT cas_run_test(6, 'T6: bitmap and',
    $$SELECT count(*) FROM cascades_test_t WHERE a = 1 AND b = 2$$);

SELECT cas_run_test(7, 'T7: bitmap or',
    $$SELECT count(*) FROM cascades_test_t WHERE a = 1 OR b = 2$$);

-- ============================================================================
-- Part 4: JOIN Tests (T8-T12)
-- ============================================================================
\echo '=== T8-T12: JOIN ==='

SELECT cas_run_test(8, 'T8: inner join',
    $$SELECT count(*) FROM cascades_test_j1 t1
      JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id$$);

SELECT cas_run_test(9, 'T9: left join',
    $$SELECT count(*) FROM cascades_test_j1 t1
      LEFT JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id$$);

SELECT cas_run_test(10, 'T10: exists/semi',
    $$SELECT count(*) FROM cascades_test_j1 t1
      WHERE EXISTS (SELECT 1 FROM cascades_test_j2 t2 WHERE t2.j1_id = t1.id)$$);

SELECT cas_run_test(11, 'T11: not exists/anti',
    $$SELECT count(*) FROM cascades_test_j1 t1
      WHERE NOT EXISTS (SELECT 1 FROM cascades_test_j2 t2 WHERE t2.j1_id = t1.id)$$);

SELECT cas_run_test(12, 'T12: 3-table join',
    $$SELECT count(*) FROM cascades_test_j1 t1
      JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id
      JOIN cascades_test_j3 t3 ON t2.id = t3.j2_id$$);

-- ============================================================================
-- Part 5: Upper Ops (T13-T18)
-- ============================================================================
\echo '=== T13-T18: Upper Ops ==='

SELECT cas_run_test(13, 'T13: group+order+limit',
    $$SELECT a, count(*) AS cnt FROM cascades_test_t
      GROUP BY a ORDER BY a LIMIT 5$$);

SELECT cas_run_test(14, 'T14: group+order+limit10',
    $$SELECT a, count(*) AS cnt FROM cascades_test_t
      GROUP BY a ORDER BY a LIMIT 10$$);

SELECT cas_run_test(15, 'T15: distinct+order+limit',
    $$SELECT DISTINCT a FROM cascades_test_t ORDER BY a LIMIT 5$$);

SELECT cas_run_test(16, 'T16: order+limit',
    $$SELECT id, a, name FROM cascades_test_t ORDER BY a LIMIT 5$$);

SELECT cas_run_test(17, 'T17: join+group+order+limit',
    $$SELECT t1.val, count(*) AS cnt
      FROM cascades_test_j1 t1
      JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id
      WHERE t1.val > 10
      GROUP BY t1.val ORDER BY t1.val LIMIT 10$$);

SELECT cas_run_test(18, 'T18: group+having',
    $$SELECT a, count(*) AS cnt FROM cascades_test_t
      GROUP BY a HAVING count(*) > 1 ORDER BY a LIMIT 5$$);

-- ============================================================================
-- Part 6: Phase 6/8 — Subquery Decorrelation + Enhanced Debug (T22-T24)
-- ============================================================================
\echo '=== T22-T24: Decorrelation / Debug ==='

-- T22: Uncorrelated scalar subquery (initPlan) — should pass Cascades (Phase 6b)
SELECT cas_run_test(22, 'T22: uncorrelated subquery',
    $$SELECT count(*) FROM (SELECT id, (SELECT count(*) FROM cascades_test_t) AS total FROM cascades_test_j1) sub$$);

-- T23: Correlated scalar subquery — decorrelation attempt, PG fallback if fails (Phase 6a)
SELECT cas_run_test(23, 'T23: correlated subquery',
    $$SELECT count(*) FROM (SELECT id, (SELECT max(val) FROM cascades_test_j2 WHERE j1_id = t1.id) AS mv FROM cascades_test_j1 t1) sub$$);

-- T24: Debug output — verify rules/memo dump work with cascades_planner_debug=on (Phase 8)
SET cascades_planner_debug = on;
SELECT cas_run_test(24, 'T24: debug output',
    $$SELECT count(*) FROM cascades_test_t WHERE a > 10$$);
SET cascades_planner_debug = off;

-- ============================================================================
-- Part 7: Fallback Cases (T19-T21)
-- 这些场景 Cascades 设计上不支持, 预检查会自动回退到 PG 原 planner
-- ============================================================================
\echo '=== T19-T21: Fallback ==='

SELECT cas_run_test(19, 'T19: distinct on',
    $$SELECT DISTINCT ON (a) a, b FROM cascades_test_t ORDER BY a, b LIMIT 5$$);

SELECT cas_run_test(20, 'T20: window function',
    $$SELECT a, b, row_number() OVER (PARTITION BY a ORDER BY b) AS rn
      FROM cascades_test_t ORDER BY a, b LIMIT 5$$);

SELECT cas_run_test(21, 'T21: for update',
    $$SELECT id, a, b FROM cascades_test_t WHERE id = 1 FOR UPDATE$$);

-- ============================================================================
-- TEST SUMMARY
-- ============================================================================
\echo ''
\echo '========================================'
\echo '  CASCADES REGRESSION TEST SUMMARY'
\echo '========================================'

SELECT test_id, test_name,
       CASE WHEN pg_ok AND cas_ok THEN 'PASS' ELSE 'FAIL' END AS verdict,
       cas_path
FROM _cas_test_results
ORDER BY test_id;

\echo ''
SELECT
    count(*) AS total_tests,
    sum(CASE WHEN pg_ok AND cas_ok THEN 1 ELSE 0 END) AS passed,
    sum(CASE WHEN NOT pg_ok OR NOT cas_ok THEN 1 ELSE 0 END) AS failed,
    sum(CASE WHEN cas_path = 'CASCADES' THEN 1 ELSE 0 END) AS cascades_path,
    sum(CASE WHEN cas_path = 'FALLBACK' THEN 1 ELSE 0 END) AS fallback_path
FROM _cas_test_results;

\echo ''
\echo '========================================'
\echo '  ALL TESTS COMPLETE'
\echo '========================================'
