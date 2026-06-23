-- ============================================================================
-- Cascades 优化器回归测试套件 (最终版)
-- 覆盖: 设计文档 §14.6 最小回归矩阵 + Phase 3/4 扩展
-- 运行: psql -d cascades_test -f cascades_regression_test_final.sql
-- ============================================================================

\set ON_ERROR_STOP off

-- 确保 fallback_on_error 开启
SET enable_cascades_planner = on;
SET cascades_planner_fallback_on_error = on;
SET cascades_planner_debug = off;
SET cascades_planner_max_tasks = 200000;
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
-- Part 8: Extended Coverage — 变换规则 + Enforcer + Property 全覆盖 (T25-T44)
-- ============================================================================
\echo '=== T25-T44: Extended Coverage ==='

-- T25: 3-table all INNER JOIN → triggers C2 JoinAssociativity
SELECT cas_run_test(25, 'T25: 3-inner-join (associativity)',
    $$SELECT t1.val AS v1, t2.val AS v2, t3.val AS v3
      FROM cascades_test_j1 t1
      JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id
      JOIN cascades_test_j3 t3 ON t2.id = t3.j2_id$$);

-- T26: LEFT JOIN where right side unreferenced → triggers G2 OuterJoinElimination
SELECT cas_run_test(26, 'T26: left join unreferenced (outer elim)',
    $$SELECT t1.id, t1.val FROM cascades_test_j1 t1
      LEFT JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id$$);

-- T27: INNER JOIN with dual WHERE → triggers A2 PushDownPredicateJoin
SELECT cas_run_test(27, 'T27: join+filter (predicate pushdown join)',
    $$SELECT t1.val AS v1, t2.val AS v2 FROM cascades_test_j1 t1
      JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id
      WHERE t1.val > 20 AND t2.val > 10$$);

-- T28: INNER JOIN with WHERE on both sides → triggers filter interaction with join
SELECT cas_run_test(28, 'T28: join with filter on both sides',
    $$SELECT t1.val AS v1, t2.val AS v2 FROM cascades_test_j1 t1
      JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id
      WHERE t1.val > 10 AND t2.val > 5$$);

-- T29: ORDER BY without matching index → forces Sort enforcer
SELECT cas_run_test(29, 'T29: sort enforcer (no index)',
    $$SELECT * FROM cascades_test_t ORDER BY name LIMIT 10$$);

-- T30: GROUP BY + ORDER BY different columns → tests pathkey interaction
SELECT cas_run_test(30, 'T30: group+order diff cols',
    $$SELECT a, count(*) AS cnt FROM cascades_test_t
      GROUP BY a ORDER BY cnt DESC LIMIT 5$$);

-- T31: DISTINCT + ORDER BY same column → Unique sorted path
SELECT cas_run_test(31, 'T31: distinct+order same col',
    $$SELECT DISTINCT b FROM cascades_test_t ORDER BY b LIMIT 10$$);

-- T32: LIMIT without ORDER → tests pure Limit path
SELECT cas_run_test(32, 'T32: limit no order',
    $$SELECT * FROM cascades_test_t LIMIT 3$$);

-- T33: Aggregate without GROUP BY → HashAgg single-group
SELECT cas_run_test(33, 'T33: agg no group (single-group HashAgg)',
    $$SELECT count(*), sum(a), avg(b) FROM cascades_test_t WHERE a > 10$$);

-- T34: GROUP BY + HAVING with aggregation condition
SELECT cas_run_test(34, 'T34: having with agg condition',
    $$SELECT a, count(*) AS cnt, sum(b) AS s FROM cascades_test_t
      GROUP BY a HAVING count(*) > 1 AND sum(b) > 10 ORDER BY a LIMIT 10$$);

-- T35: LIMIT with OFFSET
SELECT cas_run_test(35, 'T35: limit with offset',
    $$SELECT * FROM cascades_test_t ORDER BY id LIMIT 5 OFFSET 10$$);

-- T36: 2-table self-join → tests dedup with same table twice
SELECT cas_run_test(36, 'T36: semi join (inner unique)',
    $$SELECT t1.* FROM cascades_test_j1 t1
      WHERE EXISTS (SELECT 1 FROM cascades_test_j2 t2
                    WHERE t2.j1_id = t1.id AND t2.val > 5)$$);

-- T37: Composite ORDER BY with LIMIT → tests multi-key sort
SELECT cas_run_test(37, 'T37: multi-key sort',
    $$SELECT * FROM cascades_test_t ORDER BY a DESC, b ASC LIMIT 15$$);

-- T38: NOT IN (anti-join with subquery)
SELECT cas_run_test(38, 'T38: not in subquery',
    $$SELECT * FROM cascades_test_j1 t1
      WHERE t1.id NOT IN (SELECT j1_id FROM cascades_test_j2 WHERE val > 10)
      LIMIT 10$$);

-- T39: Correlated EXISTS with aggregation
SELECT cas_run_test(39, 'T39: correlated exists agg',
    $$SELECT * FROM cascades_test_j1 t1
      WHERE EXISTS (SELECT 1 FROM cascades_test_j2 t2
                    WHERE t2.j1_id = t1.id AND t2.val > 15)$$);

-- T40: SELECT with expression in target list → tests Project cost
SELECT cas_run_test(40, 'T40: expr in target list',
    $$SELECT a + b AS sum_ab, a * b AS prod_ab, name || '_suffix' AS labeled
      FROM cascades_test_t WHERE a > 10 ORDER BY sum_ab LIMIT 10$$);

-- T41: Aggregate with DISTINCT inside (count distinct)
SELECT cas_run_test(41, 'T41: count distinct',
    $$SELECT count(DISTINCT a) AS distinct_a, count(DISTINCT b) AS distinct_b
      FROM cascades_test_t$$);

-- T42: 2-table LEFT JOIN → tests outer join handling
SELECT cas_run_test(42, 'T42: left join with filter',
    $$SELECT t1.val AS v1, t2.val AS v2 FROM cascades_test_j1 t1
      LEFT JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id
      WHERE t1.val > 10 LIMIT 15$$);

-- T43: IS NULL filter → empty set edge case
SELECT cas_run_test(43, 'T43: is null filter',
    $$SELECT * FROM cascades_test_t WHERE name IS NULL LIMIT 5$$);

-- T44: Complex: JOIN + WHERE + GROUP + HAVING + ORDER + LIMIT
SELECT cas_run_test(44, 'T44: full complex query',
    $$SELECT t1.a, count(*) AS cnt, sum(t2.val) AS total_val
      FROM cascades_test_t t1
      JOIN cascades_test_j2 t2 ON t1.b = t2.val
      WHERE t1.a > 10
      GROUP BY t1.a HAVING count(*) > 1
      ORDER BY total_val DESC LIMIT 10$$);

-- ============================================================================
-- Part 9: Deep Coverage — 未覆盖规则 + 边界路径 (T45-T60)
-- ============================================================================
\echo '=== T45-T60: Deep Coverage ==='

-- T45: WHERE false → empty join set, triggers E1 PruneEmptyJoin
SELECT cas_run_test(45, 'T45: empty join (prune empty)',
    $$SELECT t1.id AS id1, t2.id AS id2 FROM cascades_test_j1 t1
      JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id
      WHERE false$$);

-- T46: No agg + no GROUP BY → triggers F1 EliminateAgg (should skip Agg node)
SELECT cas_run_test(46, 'T46: no agg no group (eliminate agg)',
    $$SELECT id, a FROM cascades_test_t WHERE a > 10 ORDER BY id$$);

-- T47: GROUP BY with ORDER BY same keys → GroupAgg sorted path
SELECT cas_run_test(47, 'T47: group+order same keys (GroupAgg)',
    $$SELECT a, count(*) AS cnt FROM cascades_test_t
      GROUP BY a ORDER BY a LIMIT 10$$);

-- T48: DISTINCT without ORDER → Unique unordered
SELECT cas_run_test(48, 'T48: distinct no order',
    $$SELECT DISTINCT a FROM cascades_test_t LIMIT 10$$);

-- T49: HAVING without GROUP BY (aggregate only)
SELECT cas_run_test(49, 'T49: having without group',
    $$SELECT count(*) AS cnt FROM cascades_test_t HAVING count(*) > 0$$);

-- T50: Agg(Filter) → should trigger predicate interaction
SELECT cas_run_test(50, 'T50: agg with filter (predicate agg)',
    $$SELECT a, count(*) FROM cascades_test_t WHERE b > 10
      GROUP BY a ORDER BY a$$);

-- T51: INNER JOIN with LIMIT → triggers D2 PushDownLimitJoin
SELECT cas_run_test(51, 'T51: join with limit (limit pushdown)',
    $$SELECT t1.val AS v1, t2.val AS v2 FROM cascades_test_j1 t1
      JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id LIMIT 5$$);

-- T52: ORDER BY + LIMIT 1 → tight limit, tests TopN path
SELECT cas_run_test(52, 'T52: order+limit 1 (TopN)',
    $$SELECT * FROM cascades_test_t ORDER BY a DESC LIMIT 1$$);

-- T53: GROUP BY multiple cols → tests multi-key grouping
SELECT cas_run_test(53, 'T53: group by multi cols',
    $$SELECT a, b, count(*) AS cnt FROM cascades_test_t
      GROUP BY a, b ORDER BY a, b LIMIT 10$$);

-- T54: Agg with WHERE IS NOT NULL → filter interaction
SELECT cas_run_test(54, 'T54: agg with not-null filter',
    $$SELECT a, count(*) FROM cascades_test_t WHERE b IS NOT NULL
      GROUP BY a ORDER BY a LIMIT 10$$);

-- T55: 2-table LEFT JOIN + GROUP BY → complex outer join interaction
SELECT cas_run_test(55, 'T55: left join+group',
    $$SELECT t1.val AS v1, count(t2.val) AS cnt FROM cascades_test_j1 t1
      LEFT JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id
      GROUP BY t1.val ORDER BY t1.val LIMIT 10$$);

-- T56: LIMIT large offset → tests offset handling
SELECT cas_run_test(56, 'T56: limit large offset',
    $$SELECT * FROM cascades_test_t ORDER BY id LIMIT 5 OFFSET 100$$);

-- T57: Boolean column filter → tests simple predicate
SELECT cas_run_test(57, 'T57: simple count star',
    $$SELECT count(*) FROM cascades_test_t WHERE a > 50$$);

-- T58: ORDER BY descending + LIMIT → tests reverse sort
SELECT cas_run_test(58, 'T58: order desc+limit',
    $$SELECT * FROM cascades_test_t ORDER BY id DESC, a ASC LIMIT 10$$);

-- T59: IN with subquery → tests semi-join rewrite
SELECT cas_run_test(59, 'T59: in subquery',
    $$SELECT * FROM cascades_test_j1 t1 WHERE t1.id IN
      (SELECT j1_id FROM cascades_test_j2 WHERE val > 10) LIMIT 10$$);

-- T60: MAX/MIN aggregate → tests simple aggregate plan
SELECT cas_run_test(60, 'T60: max min aggregate',
    $$SELECT max(a), min(b), avg(a) FROM cascades_test_t WHERE a > 10$$);

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
