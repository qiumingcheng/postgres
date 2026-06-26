-- ============================================================================
-- Cascades 80% 覆盖率专项测试套件
-- 目标: 配合现有测试将行覆盖提升至 80%
-- 涵盖: planbuild PHYSICAL_* upper 分支、rule 变换规则、pattern 匹配、
--        memo group merge、rewrite 规则链、debug 函数、task 边缘路径
-- ============================================================================

\set ON_ERROR_STOP off
SET enable_cascades_planner = on;
SET cascades_planner_fallback_on_error = on;
SET cascades_planner_debug = on;         -- 覆盖 debug.c
SET cascades_planner_max_tasks = 500000;
SET client_min_messages = warning;

-- 确保扩展表存在
DROP TABLE IF EXISTS coverage_t1;
DROP TABLE IF EXISTS coverage_t2;
DROP TABLE IF EXISTS coverage_t3;
CREATE TABLE coverage_t1 (id INTEGER, grp INTEGER, val INTEGER, name TEXT);
CREATE TABLE coverage_t2 (id INTEGER PRIMARY KEY, t1_id INTEGER, score REAL);
CREATE TABLE coverage_t3 (id INTEGER, ref_id INTEGER, label TEXT);
INSERT INTO coverage_t1 SELECT i, i%10, i*10, 'item_'||i FROM generate_series(1,100) i;
INSERT INTO coverage_t2 SELECT i, i%50+1, random()*100 FROM generate_series(1,200) i;
INSERT INTO coverage_t3 SELECT i, i%30+1, 'label_'||(i%5) FROM generate_series(1,150) i;
CREATE INDEX IF NOT EXISTS idx_cov_t1_grp ON coverage_t1(grp);
CREATE INDEX IF NOT EXISTS idx_cov_t2_t1id ON coverage_t2(t1_id);
CREATE INDEX IF NOT EXISTS idx_cov_t3_ref ON coverage_t3(ref_id);
ANALYZE coverage_t1;
ANALYZE coverage_t2;
ANALYZE coverage_t3;

DROP TABLE IF EXISTS _cov80_results;
CREATE TEMP TABLE _cov80_results (test_id INTEGER, test_name TEXT, cas_path TEXT);

CREATE OR REPLACE FUNCTION cov80_test(test_id INTEGER, test_name TEXT, sql_text TEXT) RETURNS TEXT AS $$
DECLARE
    cas_path_val TEXT := 'ERROR';
    dummy       INTEGER;
BEGIN
    BEGIN
        EXECUTE 'SELECT count(*) FROM (' || sql_text || ') AS _sub' INTO dummy;
        cas_path_val := 'CASCADES';
    EXCEPTION WHEN OTHERS THEN
        BEGIN
            SET enable_cascades_planner = off;
            EXECUTE 'SELECT count(*) FROM (' || sql_text || ') AS _sub' INTO dummy;
            cas_path_val := 'FALLBACK';
        EXCEPTION WHEN OTHERS THEN
            cas_path_val := 'BOTH_FAIL';
        END;
        SET enable_cascades_planner = on;
    END;
    INSERT INTO _cov80_results VALUES (test_id, test_name, cas_path_val);
    RETURN test_name || ': ' || cas_path_val;
END;
$$ LANGUAGE plpgsql;

-- ====================================================================
-- Section A: 单表 upper-op 查询 → 覆盖 planbuild PHYSICAL_* 分支
-- planbuild.c: PHYSICAL_SORT, HASHAGG, GROUPAGG, LIMIT, UNIQUE, PROJECT
-- ====================================================================

\echo '=== A1: Sort enforcer (planbuild PHYSICAL_SORT) ==='
SELECT cov80_test(1001, 'A1_sort_simple', $$SELECT * FROM coverage_t1 ORDER BY id$$);
SELECT cov80_test(1002, 'A1_sort_desc', $$SELECT * FROM coverage_t1 ORDER BY val DESC$$);
SELECT cov80_test(1003, 'A1_sort_multikey', $$SELECT id, val FROM coverage_t1 ORDER BY grp, val$$);
SELECT cov80_test(1004, 'A1_sort_expr', $$SELECT id, val*2 AS dbl FROM coverage_t1 ORDER BY val*2$$);

\echo '=== A2: HashAgg (planbuild PHYSICAL_HASHAGG) ==='
SELECT cov80_test(1005, 'A2_agg_count', $$SELECT grp, count(*) FROM coverage_t1 GROUP BY grp$$);
SELECT cov80_test(1006, 'A2_agg_sum', $$SELECT grp, sum(val) FROM coverage_t1 GROUP BY grp$$);
SELECT cov80_test(1007, 'A2_agg_multi', $$SELECT grp, count(*), sum(val), avg(val) FROM coverage_t1 GROUP BY grp$$);
SELECT cov80_test(1008, 'A2_agg_nogroup', $$SELECT count(*), sum(val) FROM coverage_t1$$);
SELECT cov80_test(1009, 'A2_agg_expr', $$SELECT grp, sum(val*2+1) FROM coverage_t1 GROUP BY grp$$);

\echo '=== A3: GroupAgg (planbuild PHYSICAL_GROUPAGG) ==='
SELECT cov80_test(1010, 'A3_groupagg', $$SELECT grp, count(*) FROM coverage_t1 GROUP BY grp ORDER BY grp$$);
SELECT cov80_test(1011, 'A3_groupagg_multi', $$SELECT grp, count(*), sum(val) FROM coverage_t1 GROUP BY grp ORDER BY grp$$);

\echo '=== A4: Limit (planbuild PHYSICAL_LIMIT) ==='
SELECT cov80_test(1012, 'A4_limit_simple', $$SELECT * FROM coverage_t1 LIMIT 10$$);
SELECT cov80_test(1013, 'A4_limit_order', $$SELECT * FROM coverage_t1 ORDER BY val LIMIT 5$$);
SELECT cov80_test(1014, 'A4_limit_offset', $$SELECT * FROM coverage_t1 ORDER BY id LIMIT 10 OFFSET 20$$);

\echo '=== A5: Distinct/Unique (planbuild PHYSICAL_UNIQUE) ==='
SELECT cov80_test(1015, 'A5_distinct_simple', $$SELECT DISTINCT grp FROM coverage_t1$$);
SELECT cov80_test(1016, 'A5_distinct_multi', $$SELECT DISTINCT grp, val FROM coverage_t1$$);
SELECT cov80_test(1017, 'A5_distinct_order', $$SELECT DISTINCT grp FROM coverage_t1 ORDER BY grp$$);

\echo '=== A6: Project (planbuild PHYSICAL_PROJECT) ==='
SELECT cov80_test(1018, 'A6_project_expr', $$SELECT id, val+1, upper(name) FROM coverage_t1$$);
SELECT cov80_test(1019, 'A6_project_alias', $$SELECT id AS a, val AS b FROM coverage_t1$$);
SELECT cov80_test(1020, 'A6_project_const', $$SELECT id, 42 AS answer FROM coverage_t1$$);

\echo '=== A7: Combined upper ops ==='
SELECT cov80_test(1021, 'A7_agg_sort_limit', $$SELECT grp, count(*) FROM coverage_t1 GROUP BY grp ORDER BY grp LIMIT 5$$);
SELECT cov80_test(1022, 'A7_distinct_sort_limit', $$SELECT DISTINCT grp FROM coverage_t1 ORDER BY grp LIMIT 3$$);
SELECT cov80_test(1023, 'A7_expr_agg_sort', $$SELECT val%5 AS mod5, sum(val) FROM coverage_t1 GROUP BY val%5 ORDER BY mod5$$);

-- ====================================================================
-- Section B: 谓词下推 → 覆盖 rule.c PushDownPredicate 规则族
-- rule.c: A1 PushDownPredicateScan, A2 PushDownPredicateJoin,
--         A3 PushDownPredicateProject, A4 PushDownPredicateAgg
-- ====================================================================

\echo '=== B1: PushDownPredicateScan ==='
SELECT cov80_test(1101, 'B1_scan_filter', $$SELECT * FROM coverage_t1 WHERE val > 50$$);
SELECT cov80_test(1102, 'B1_scan_composite', $$SELECT * FROM coverage_t1 WHERE val > 30 AND grp = 5$$);
SELECT cov80_test(1103, 'B1_scan_or', $$SELECT * FROM coverage_t1 WHERE val < 10 OR val > 90$$);
SELECT cov80_test(1104, 'B1_scan_in', $$SELECT * FROM coverage_t1 WHERE grp IN (1,3,5,7)$$);
SELECT cov80_test(1105, 'B1_scan_is_null', $$SELECT * FROM coverage_t1 WHERE name IS NOT NULL$$);

\echo '=== B2: PushDownPredicateJoin ==='
SELECT cov80_test(1106, 'B2_join_filter', $$SELECT t1.id, t2.score FROM coverage_t1 t1 JOIN coverage_t2 t2 ON t1.id = t2.t1_id WHERE t1.val > 50$$);
SELECT cov80_test(1107, 'B2_join_both_filter', $$SELECT t1.id, t2.score FROM coverage_t1 t1 JOIN coverage_t2 t2 ON t1.id = t2.t1_id WHERE t1.val > 30 AND t2.score > 50$$);
SELECT cov80_test(1108, 'B2_join_three', $$SELECT t1.id, t2.score, t3.label FROM coverage_t1 t1 JOIN coverage_t2 t2 ON t1.id = t2.t1_id JOIN coverage_t3 t3 ON t1.id = t3.ref_id WHERE t1.grp = 3$$);

\echo '=== B3: PushDownPredicateProject ==='
SELECT cov80_test(1109, 'B3_project_filter', $$SELECT id*10, val FROM (SELECT id, val FROM coverage_t1 WHERE val > 50) sub$$);

\echo '=== B4: PushDownPredicateAgg ==='
SELECT cov80_test(1110, 'B4_agg_having', $$SELECT grp, sum(val) FROM coverage_t1 GROUP BY grp HAVING sum(val) > 200$$);

-- ====================================================================
-- Section C: Join 变换规则 → 覆盖 rewrite.c JoinReorder + rule.c
-- pattern.c: 多节点模式匹配 (JoinCommutativity/Associativity)
-- ====================================================================

\echo '=== C1: JoinCommutativity (Binder pattern matching) ==='
SELECT cov80_test(1201, 'C1_join_2table', $$SELECT * FROM coverage_t1 t1 JOIN coverage_t2 t2 ON t1.id = t2.t1_id$$);
SELECT cov80_test(1202, 'C1_join_2table_filter', $$SELECT * FROM coverage_t1 t1 JOIN coverage_t2 t2 ON t1.id = t2.t1_id WHERE t1.grp = 1$$);

\echo '=== C2: Join with limit pushdown ==='
SELECT cov80_test(1203, 'C2_limit_join', $$SELECT * FROM (SELECT * FROM coverage_t1 ORDER BY id LIMIT 10) t1 JOIN coverage_t2 t2 ON t1.id = t2.t1_id$$);

-- ====================================================================
-- Section D: 列裁剪规则链 → 覆盖 rule.c B1-B5
-- ====================================================================

\echo '=== D1: PruneScanColumns ==='
SELECT cov80_test(1301, 'D1_single_col', $$SELECT id FROM coverage_t1$$);
SELECT cov80_test(1302, 'D1_two_cols', $$SELECT id, val FROM coverage_t1$$);

\echo '=== D2: PruneJoinColumns ==='
SELECT cov80_test(1303, 'D2_join_cols', $$SELECT t1.id, t2.score FROM coverage_t1 t1 JOIN coverage_t2 t2 ON t1.id = t2.t1_id$$);

\echo '=== D3: PruneAggColumns ==='
SELECT cov80_test(1304, 'D3_agg_col', $$SELECT grp, sum(val) FROM coverage_t1 GROUP BY grp$$);

\echo '=== D4: PruneProjectColumns ==='
SELECT cov80_test(1305, 'D4_proj_col', $$SELECT id, val+1 AS inc FROM coverage_t1$$);

-- ====================================================================
-- Section E: Merge/Eliminate 规则 → 覆盖 memo.c Group Merge
-- memo.c: pg_memo_merge_group, EliminateLimit/EliminateAgg/EliminateProject
-- ====================================================================

\echo '=== E1: EliminateLimit (no limit clause) ==='
SELECT cov80_test(1401, 'E1_no_limit', $$SELECT * FROM coverage_t1$$);

\echo '=== E2: EliminateAgg (no aggregation) ==='
SELECT cov80_test(1402, 'E2_no_agg', $$SELECT id, val FROM coverage_t1$$);

\echo '=== E3: EliminateProject (simple column ref) ==='
SELECT cov80_test(1403, 'E3_simple_proj', $$SELECT id FROM coverage_t1$$);

\echo '=== E4: MergeProjectWithChild ==='
SELECT cov80_test(1404, 'E4_merge_proj', $$SELECT id*2 FROM (SELECT id FROM coverage_t1) sub$$);

\echo '=== E5: PruneEmptyJoin/Scan ==='
SELECT cov80_test(1405, 'E5_empty_filter', $$SELECT * FROM coverage_t1 WHERE id < 0$$);

-- ====================================================================
-- Section F: Semi-Join Dedup 规则 → 覆盖 rule.c G1-G3, H4, E1
-- ====================================================================

\echo '=== F1: MergeFilterWithJoin ==='
SELECT cov80_test(1501, 'F1_filter_join', $$SELECT * FROM coverage_t1 t1 JOIN coverage_t2 t2 ON t1.id = t2.t1_id AND t1.val > 50$$);

\echo '=== F2: EliminateJoinWithConst (single-row) ==='
SELECT cov80_test(1502, 'F2_const_join', $$SELECT * FROM cascades_single t1 JOIN coverage_t2 t2 ON t1.id = t2.t1_id$$);

\echo '=== F3: InnerToSemi (EXISTS pattern) ==='
SELECT cov80_test(1503, 'F3_exists', $$SELECT * FROM coverage_t1 t1 WHERE EXISTS (SELECT 1 FROM coverage_t2 t2 WHERE t2.t1_id = t1.id)$$);

\echo '=== F4: EliminateSortWithConstKey ==='
SELECT cov80_test(1504, 'F4_sort_const', $$SELECT * FROM coverage_t1 ORDER BY 1$$);

-- ====================================================================
-- Section G: 多属性优化 → 覆盖 task.c push_enforce_and_cost_tasks
-- property.c: pathkeys_contained_in, bms_equal 分支
-- ====================================================================

\echo '=== G1: Multi-property (sort+group+distinct) ==='
SELECT cov80_test(1601, 'G1_multi_prop', $$SELECT DISTINCT grp FROM coverage_t1 GROUP BY grp ORDER BY grp$$);
SELECT cov80_test(1602, 'G2_sort_group_diff', $$SELECT grp, sum(val) FROM coverage_t1 GROUP BY grp ORDER BY sum(val)$$);

-- ====================================================================
-- Section H: NestLoop 外连接 → 覆盖 postopt.c Material 插入
-- ====================================================================

\echo '=== H1: NestLoop outer join ==='
SELECT cov80_test(1701, 'H1_left_join', $$SELECT t1.id, t2.score FROM coverage_t1 t1 LEFT JOIN coverage_t2 t2 ON t1.id = t2.t1_id WHERE t2.t1_id IS NULL$$);

\echo '=== H2: RIGHT JOIN (pg_adapter swap branch) ==='
SELECT cov80_test(1702, 'H2_right_join', $$SELECT t1.id, t2.score FROM coverage_t1 t1 RIGHT JOIN coverage_t2 t2 ON t1.id = t2.t1_id$$);

-- ====================================================================
-- Section I: cascades.c 边缘路径
-- ====================================================================

\echo '=== I1: ORDER+LIMIT 2-table (bypass 3-table fallback) ==='
SELECT cov80_test(1801, 'I1_order_limit_2table', $$SELECT t1.id, t2.score FROM coverage_t1 t1 JOIN coverage_t2 t2 ON t1.id = t2.t1_id ORDER BY t1.id LIMIT 5$$);

-- ====================================================================
-- Section J: 无索引表 → 强制 COMPOSABLE_OP Sort/Scan 路径
-- 覆盖 planbuild.c PHYSICAL_SORT/SCAN 完整分支
-- ====================================================================

DROP TABLE IF EXISTS cov_noindex;
CREATE TABLE cov_noindex (x INTEGER, y TEXT, z REAL);
INSERT INTO cov_noindex SELECT i, 'row_'||i, random()*100 FROM generate_series(1,500) i;
ANALYZE cov_noindex;

\echo '=== J1: No-index queries (force Sort enforcer) ==='
SELECT cov80_test(1901, 'J1_sort_noindex', $$SELECT * FROM cov_noindex ORDER BY x$$);
SELECT cov80_test(1902, 'J1_sort_text', $$SELECT * FROM cov_noindex ORDER BY y$$);
SELECT cov80_test(1903, 'J1_sort_real', $$SELECT * FROM cov_noindex ORDER BY z$$);
SELECT cov80_test(1904, 'J1_sort_multi', $$SELECT x, z FROM cov_noindex ORDER BY x, z$$);
SELECT cov80_test(1905, 'J1_sort_desc_noidx', $$SELECT * FROM cov_noindex ORDER BY x DESC$$);

\echo '=== J2: No-index agg+sort (force GroupAgg path) ==='
SELECT cov80_test(1906, 'J2_agg_sort_noidx', $$SELECT x%10 AS g, count(*) FROM cov_noindex GROUP BY g ORDER BY g$$);
SELECT cov80_test(1907, 'J2_agg_sort_multi', $$SELECT x%5 AS g, sum(z) FROM cov_noindex GROUP BY g ORDER BY sum(z)$$);

\echo '=== J3: No-index distinct+sort (force Unique sorted) ==='
SELECT cov80_test(1908, 'J3_distinct_sort', $$SELECT DISTINCT x FROM cov_noindex ORDER BY x$$);

\echo '=== J4: No-index limit patterns ==='
SELECT cov80_test(1909, 'J4_limit_sort', $$SELECT * FROM cov_noindex ORDER BY z LIMIT 10$$);
SELECT cov80_test(1910, 'J4_offset', $$SELECT * FROM cov_noindex ORDER BY x LIMIT 5 OFFSET 20$$);

-- ====================================================================
-- Section K: 扩展 JOIN 模式 → 覆盖 rule.c 变换规则
-- ====================================================================

DROP TABLE IF EXISTS cov_j1;
DROP TABLE IF EXISTS cov_j2;
DROP TABLE IF EXISTS cov_j3;
CREATE TABLE cov_j1 (a INTEGER, b INTEGER, c TEXT);
CREATE TABLE cov_j2 (a INTEGER, d REAL, e TEXT);
CREATE TABLE cov_j3 (a INTEGER, f INTEGER);
INSERT INTO cov_j1 SELECT i, i%20, 'x'||(i%5) FROM generate_series(1,200) i;
INSERT INTO cov_j2 SELECT i%50+1, random()*100, 'y'||(i%5) FROM generate_series(1,300) i;
INSERT INTO cov_j3 SELECT i%30+1, i FROM generate_series(1,100) i;
CREATE INDEX idx_j1_a ON cov_j1(a);
CREATE INDEX idx_j2_a ON cov_j2(a);
ANALYZE cov_j1; ANALYZE cov_j2; ANALYZE cov_j3;

\echo '=== K1: 3-table join (predicate pushdown chain) ==='
SELECT cov80_test(2001, 'K1_3table', $$SELECT j1.a, j2.d, j3.f FROM cov_j1 j1 JOIN cov_j2 j2 ON j1.a=j2.a JOIN cov_j3 j3 ON j1.a=j3.a$$);
SELECT cov80_test(2002, 'K1_3table_filter', $$SELECT j1.a, j2.d FROM cov_j1 j1 JOIN cov_j2 j2 ON j1.a=j2.a JOIN cov_j3 j3 ON j1.a=j3.a WHERE j1.b > 10 AND j2.d > 30$$);

\echo '=== K2: Subquery in FROM (merge project chain) ==='
SELECT cov80_test(2003, 'K2_sub_from', $$SELECT a, d FROM (SELECT j1.a, j2.d FROM cov_j1 j1 JOIN cov_j2 j2 ON j1.a=j2.a WHERE j1.b > 5) sub$$);

\echo '=== K3: Self-join (commutativity with same table) ==='
SELECT cov80_test(2004, 'K3_self', $$SELECT t1.a, t2.b FROM cov_j1 t1 JOIN cov_j1 t2 ON t1.a=t2.a WHERE t1.b < t2.b$$);

\echo '=== K4: Multi-condition join ==='
SELECT cov80_test(2005, 'K4_multicond', $$SELECT j1.a, j2.d FROM cov_j1 j1 JOIN cov_j2 j2 ON j1.a=j2.a AND j1.b > 5 AND j2.d < 50$$);

\echo '=== K5: Left join with IS NULL (anti-join pattern) ==='
SELECT cov80_test(2006, 'K5_anti', $$SELECT j1.* FROM cov_j1 j1 LEFT JOIN cov_j2 j2 ON j1.a=j2.a WHERE j2.a IS NULL$$);

\echo '=== K6: Cross join with WHERE (→ inner join) ==='
SELECT cov80_test(2007, 'K6_cross', $$SELECT j1.a, j2.d FROM cov_j1 j1, cov_j2 j2 WHERE j1.a=j2.a$$);

-- ====================================================================
-- Section L: 聚合 HAVING + LIMIT 组合 → 覆盖 rule.c agg 规则
-- ====================================================================

\echo '=== L1: Agg+Having complex ==='
SELECT cov80_test(2101, 'L1_having_agg', $$SELECT b, count(*), avg(a) FROM cov_j1 GROUP BY b HAVING count(*) > 5 AND avg(a) > 50$$);
SELECT cov80_test(2102, 'L1_having_noagg', $$SELECT b, count(*) FROM cov_j1 GROUP BY b HAVING b > 5$$);

\echo '=== L2: Group+Having+Order+Limit ==='
SELECT cov80_test(2103, 'L2_full', $$SELECT b, sum(a) FROM cov_j1 GROUP BY b HAVING sum(a) > 100 ORDER BY b LIMIT 5$$);

-- ====================================================================
-- Section M: 表达式/函数 → 覆盖 planbuild PHYSICAL_PROJECT
-- ====================================================================

\echo '=== M1: Complex expressions ==='
SELECT cov80_test(2201, 'M1_expr', $$SELECT a*2+b AS calc, upper(c) AS up, length(c) AS ln FROM cov_j1$$);
SELECT cov80_test(2202, 'M1_case', $$SELECT CASE WHEN a>100 THEN 'big' ELSE 'small' END, count(*) FROM cov_j1 GROUP BY 1$$);
SELECT cov80_test(2203, 'M1_coalesce', $$SELECT coalesce(c,'default'), a FROM cov_j1$$);

-- ====================================================================
-- Section N: task.c 边缘路径
-- ====================================================================

\echo '=== N1: Low max_tasks to trigger LIMIT path ==='
SET cascades_planner_max_tasks = 5;
SELECT cov80_test(2301, 'N1_limit_tasks', $$SELECT * FROM cov_noindex ORDER BY x$$);
SET cascades_planner_max_tasks = 500000;

\echo '=== N2: Timeout-close query (not actually timeout, but exercise check) ==='
SELECT cov80_test(2302, 'N2_timeout_check', $$SELECT * FROM cov_noindex WHERE x BETWEEN 1 AND 100$$);

-- ====================================================================
-- Section O: 扩展聚合模式 → rule.c PushDownAgg/EliminateAgg/MergeTwoAgg
-- ====================================================================

\echo '=== O1: Agg with filter (pushdown) ==='
SELECT cov80_test(2401, 'O1_agg_filter', $$SELECT b, sum(a) FROM cov_j1 WHERE a > 50 GROUP BY b$$);
SELECT cov80_test(2402, 'O1_agg_filter_having', $$SELECT b, count(*) FROM cov_j1 WHERE c='x1' GROUP BY b HAVING count(*) >= 3$$);

\echo '=== O2: Multi-level aggregation ==='
SELECT cov80_test(2403, 'O2_multi_agg', $$SELECT g, max(s) FROM (SELECT b as g, sum(a) as s FROM cov_j1 GROUP BY b) sub GROUP BY g$$);

\echo '=== O3: Agg with DISTINCT ==='
SELECT cov80_test(2404, 'O3_agg_distinct', $$SELECT b, count(DISTINCT a) FROM cov_j1 GROUP BY b$$);

\echo '=== O4: No-group agg (scalar) ==='
SELECT cov80_test(2405, 'O4_scalar_agg', $$SELECT max(a), min(b), avg(a) FROM cov_j1$$);
SELECT cov80_test(2406, 'O4_scalar_agg_filter', $$SELECT count(*) FROM cov_j1 WHERE c LIKE 'x%'$$);

-- ====================================================================
-- Section P: 扩展 JOIN 模式 → rule.c 变换规则全家桶
-- ====================================================================

\echo '=== P1: Three-way join variations ==='
SELECT cov80_test(2501, 'P1_3way_abc', $$SELECT j1.a, j2.d, j3.f FROM cov_j1 j1, cov_j2 j2, cov_j3 j3 WHERE j1.a=j2.a AND j2.a=j3.a$$);
SELECT cov80_test(2502, 'P1_3way_filter', $$SELECT j1.a, j2.d FROM cov_j1 j1 JOIN cov_j2 j2 ON j1.a=j2.a JOIN cov_j3 j3 ON j1.a=j3.a WHERE j1.b BETWEEN 5 AND 15$$);

\echo '=== P2: NOT IN / NOT EXISTS patterns ==='
SELECT cov80_test(2503, 'P2_not_in', $$SELECT * FROM cov_j1 WHERE a NOT IN (SELECT a FROM cov_j2 WHERE d > 50)$$);
SELECT cov80_test(2504, 'P2_not_exists', $$SELECT * FROM cov_j1 j1 WHERE NOT EXISTS (SELECT 1 FROM cov_j2 j2 WHERE j2.a=j1.a AND j2.d < 10)$$);

\echo '=== P3: IN with subquery ==='
SELECT cov80_test(2505, 'P3_in_subq', $$SELECT * FROM cov_j1 WHERE a IN (SELECT a FROM cov_j2 WHERE d > 30)$$);

\echo '=== P4: LEFT JOIN with complex conditions ==='
SELECT cov80_test(2506, 'P4_left_complex', $$SELECT j1.a, j2.d FROM cov_j1 j1 LEFT JOIN cov_j2 j2 ON j1.a=j2.a AND j2.d > 20 WHERE j1.b > 5$$);
SELECT cov80_test(2507, 'P4_left_coalesce', $$SELECT j1.a, COALESCE(j2.d,0) FROM cov_j1 j1 LEFT JOIN cov_j2 j2 ON j1.a=j2.a$$);

\echo '=== P5: Multiple joins with subquery ==='
SELECT cov80_test(2508, 'P5_sub_join', $$SELECT * FROM (SELECT a, b FROM cov_j1 WHERE b > 3) t1 JOIN cov_j2 t2 ON t1.a=t2.a$$);

-- ====================================================================
-- Section Q: 排序/限制组合 → planbuild PHYSICAL_SORT/LIMIT 边缘
-- ====================================================================

\echo '=== Q1: Sort with expression ==='
SELECT cov80_test(2601, 'Q1_sort_expr', $$SELECT a, b, a+b AS s FROM cov_j1 ORDER BY a+b$$);
SELECT cov80_test(2602, 'Q1_sort_multi_expr', $$SELECT a, b FROM cov_j1 ORDER BY a+b, a-b$$);

\echo '=== Q2: Limit without order ==='
SELECT cov80_test(2603, 'Q2_limit_no_order', $$SELECT * FROM cov_j1 LIMIT 1$$);
SELECT cov80_test(2604, 'Q2_limit_offset_large', $$SELECT * FROM cov_j1 LIMIT 10 OFFSET 100$$);

\echo '=== Q3: ORDER BY + LIMIT + OFFSET ==='
SELECT cov80_test(2605, 'Q3_full_pagination', $$SELECT a, c FROM cov_j1 ORDER BY a LIMIT 20 OFFSET 10$$);

\echo '=== Q4: Sort on joined result ==='
SELECT cov80_test(2606, 'Q4_join_sort', $$SELECT j1.a, j2.d FROM cov_j1 j1 JOIN cov_j2 j2 ON j1.a=j2.a ORDER BY j2.d DESC$$);
SELECT cov80_test(2607, 'Q4_join_sort_limit', $$SELECT j1.a, j2.d FROM cov_j1 j1 JOIN cov_j2 j2 ON j1.a=j2.a ORDER BY j2.d LIMIT 15$$);

-- ====================================================================
-- Section R: 过滤模式 → rule.c PushDownPredicate chain
-- ====================================================================

\echo '=== R1: Complex WHERE (AND+OR+IN) ==='
SELECT cov80_test(2701, 'R1_complex_where', $$SELECT * FROM cov_j1 WHERE (b=1 OR b=2) AND c='x1'$$);
SELECT cov80_test(2702, 'R1_between', $$SELECT * FROM cov_j1 WHERE a BETWEEN 50 AND 100$$);
SELECT cov80_test(2703, 'R1_like', $$SELECT * FROM cov_j1 WHERE c LIKE 'x_'$$);
SELECT cov80_test(2704, 'R1_is_null', $$SELECT * FROM cov_j1 WHERE c IS NOT NULL AND a > 10$$);

\echo '=== R2: Join with OR condition ==='
SELECT cov80_test(2705, 'R2_join_or', $$SELECT j1.a, j2.d FROM cov_j1 j1 JOIN cov_j2 j2 ON j1.a=j2.a WHERE j1.b=1 OR j2.d > 80$$);

-- ====================================================================
-- Section S: memo.c edge cases → group merge, hash dedup
-- ====================================================================

\echo '=== S1: Self-join merge ==='
SELECT cov80_test(2801, 'S1_self_merge', $$SELECT t1.a, t2.a FROM cov_j1 t1 JOIN cov_j1 t2 ON t1.b=t2.b$$);
SELECT cov80_test(2802, 'S1_double_self', $$SELECT t1.a, t2.b FROM cov_j1 t1, cov_j1 t2, cov_j1 t3 WHERE t1.a=t2.a AND t2.a=t3.a$$);

\echo '=== S2: Join without ON (cross→inner via WHERE) ==='
SELECT cov80_test(2803, 'S2_cross_where', $$SELECT j1.a, j2.a FROM cov_j1 j1, cov_j2 j2 WHERE j1.a = j2.a AND j1.b < 10$$);

\echo '=== S3: Empty result pruning ==='
SELECT cov80_test(2804, 'S3_empty', $$SELECT * FROM cov_j1 WHERE a < 0 AND b > 1000$$);

-- ====================================================================
-- Section T: task.c 多属性 → Clone+Resume, EnforceAndCost
-- ====================================================================

\echo '=== T1: Multiple required properties (sort+group+distinct all present) ==='
SELECT cov80_test(2901, 'T1_all_props', $$SELECT DISTINCT b FROM cov_j1 GROUP BY b ORDER BY b$$);
SELECT cov80_test(2902, 'T1_group_distinct_sort', $$SELECT b, count(*) FROM cov_j1 GROUP BY b ORDER BY count(*)$$);

\echo '=== T2: forced multi-alternative queries ==='
SELECT cov80_test(2903, 'T2_multi_alt', $$SELECT * FROM cov_j1 WHERE a IN (1,2,3) ORDER BY b LIMIT 5$$);

-- ====================================================================
-- Section U: 触发 rule.c 变换规则具体分支 — 精确定向
-- 目标: A1(PushDownPredicateScan) A2(Join) A3(Project) A4(Agg)
--       D1(MergeLimitWithSort) D2(PushDownLimitJoin) D3(EliminateLimit)
--       F1(EliminateAgg) F2(EliminateProject)
--       H4(MergeFilterWithJoin) G1(EliminateJoinConstant) G2(OuterJoinElim)
-- ====================================================================

\echo '=== U1: PushDownPredicate on multiple child types ==='
-- A1 Scan predicate pushdown with LIKE/NOT/BETWEEN/IS NULL
SELECT cov80_test(3001, 'U1_scan_multi_pred', $$SELECT * FROM cov_j1 WHERE a>10 AND b<15 AND c LIKE 'x%'$$);
-- A2 Join predicate pushdown with OR
SELECT cov80_test(3002, 'U1_join_pushdown_or', $$SELECT j1.a,j2.d FROM cov_j1 j1 JOIN cov_j2 j2 ON j1.a=j2.a WHERE j1.b=1 OR j1.b=7$$);
-- A3 Project predicate pushdown
SELECT cov80_test(3003, 'U1_proj_pushdown', $$SELECT a*2,b FROM (SELECT a,b FROM cov_j1 WHERE b>3) t WHERE a>50$$);
-- A4 Agg predicate pushdown
SELECT cov80_test(3004, 'U1_agg_pushdown', $$SELECT b,sum(a) FROM cov_j1 WHERE c='x2' GROUP BY b$$);

\echo '=== U2: Eliminate/Merge 规则 ==='
-- D3 EliminateLimit (no actual LIMIT clause → eliminate)
SELECT cov80_test(3005, 'U2_elim_limit', $$SELECT * FROM cov_j1$$);
-- F1 EliminateAgg (no aggregation, no group by)
SELECT cov80_test(3006, 'U2_elim_agg', $$SELECT a,c FROM cov_j1$$);
-- F2 EliminateProject (trivial projection on single column)
SELECT cov80_test(3007, 'U2_elim_proj', $$SELECT a FROM cov_j1$$);
-- EliminateSortWithConstKey
SELECT cov80_test(3008, 'U2_elim_sort_const', $$SELECT a FROM cov_j1 ORDER BY 1$$);
-- MergeProjectWithChild
SELECT cov80_test(3009, 'U2_merge_proj', $$SELECT a+0 FROM (SELECT a FROM cov_j1) t$$);

\echo '=== U3: MergeFilterWithJoin (H4) 变体 ==='
SELECT cov80_test(3010, 'U3_merge_filter_join1', $$SELECT * FROM cov_j1 t1 JOIN cov_j2 t2 ON t1.a=t2.a AND t1.b=3$$);
SELECT cov80_test(3011, 'U3_merge_filter_join2', $$SELECT t1.a,t2.d FROM cov_j1 t1 JOIN cov_j2 t2 ON t1.a=t2.a WHERE t1.b=5 AND t2.d>20$$);

\echo '=== U4: EliminateJoinWithConstant (G1) / OuterJoinElim (G2) ==='
-- G1: Single-row constant-side join
SELECT cov80_test(3012, 'U4_elim_join_const', $$SELECT cj1.* FROM cov_j1 cj1 JOIN cascades_single cs ON cj1.a=cs.id$$);
-- G2: LEFT JOIN where right side unreferenced in output
SELECT cov80_test(3013, 'U4_outer_elim', $$SELECT t1.a FROM cov_j1 t1 LEFT JOIN cov_j2 t2 ON t1.a=t2.a$$);

\echo '=== U5: InnerToSemi (G3) — INNER JOIN where inner side is unique ==='
SELECT cov80_test(3014, 'U5_inner_to_semi', $$SELECT j1.* FROM cov_j1 j1 JOIN cov_j2 j2 ON j1.a=j2.a$$);

\echo '=== U6: MergeJoinWithChildProj (G4) ==='
SELECT cov80_test(3015, 'U6_merge_join_proj', $$SELECT t1.a,t2.d FROM (SELECT a,b FROM cov_j1 WHERE b>3) t1 JOIN (SELECT a,d FROM cov_j2) t2 ON t1.a=t2.a$$);

\echo '=== U7: PruneEmpty 规则 (E1/E2) ==='
-- E1 PruneEmptyJoin
SELECT cov80_test(3016, 'U7_prune_empty_join', $$SELECT * FROM cov_j1 j1 JOIN (SELECT * FROM cov_j1 WHERE a<0) j2 ON j1.a=j2.a$$);
-- E2 PruneEmptyScan
SELECT cov80_test(3017, 'U7_prune_empty_scan', $$SELECT * FROM cov_j1 WHERE a<-999$$);

-- ====================================================================
-- Section V: 覆盖 cascades.c 未覆盖分支
-- 目标: pg_cascades_supported_query 各类不支持检测
--       pg_cascades_handle_status_or_error 分支
-- ====================================================================

\echo '=== V1: ORDER+LIMIT 2-table (刚好不触发 3-table fallback) ==='
SELECT cov80_test(3101, 'V1_order_limit_2t', $$SELECT j1.a,j2.d FROM cov_j1 j1 JOIN cov_j2 j2 ON j1.a=j2.a ORDER BY j1.a LIMIT 3$$);

\echo '=== V2: IS NOT NULL / NOT LIKE 变体 (subplan walker 路径) ==='
SELECT cov80_test(3102, 'V2_notnull', $$SELECT * FROM cov_j1 WHERE c IS NOT NULL AND b IS NOT NULL$$);
SELECT cov80_test(3103, 'V2_not_like', $$SELECT * FROM cov_j1 WHERE c NOT LIKE 'z%'$$);

-- ====================================================================
-- Section W: 覆盖 postopt.c (validate + physical rewrite)
-- 目标: pg_cascades_validate_plan_recurse 所有节点类型
--       pg_cascades_physical_rewrite NestLoop Material 插入
-- ====================================================================

\echo '=== W1: NestLoop with parameterized inner side ==='
SELECT cov80_test(3201, 'W1_nestloop', $$SELECT j1.* FROM cov_j1 j1 JOIN cov_j2 j2 ON j1.a=j2.a WHERE j1.b<10$$);

\echo '=== W2: Composite plan tree (Sort+Agg+Join+Scan) ==='
SELECT cov80_test(3202, 'W2_composite', $$SELECT b,count(*) FROM cov_j1 j1 JOIN cov_j2 j2 ON j1.a=j2.a WHERE j2.d>10 GROUP BY b ORDER BY b$$);

-- ====================================================================
-- Section X: 覆盖 memo.c Group 合并相关
-- 目标: pg_memo_merge_group 内层逻辑
--       Hash table 重复检测分支
-- ====================================================================

\echo '=== X1: Trigger group merge through EliminateLimit ==='
SELECT cov80_test(3301, 'X1_merge_limit', $$SELECT * FROM (SELECT * FROM cov_j1) t$$);
SELECT cov80_test(3302, 'X1_merge_proj2', $$SELECT a FROM (SELECT a,b FROM cov_j1) t$$);

\echo '=== X2: Trigger group merge through EliminateAgg ==='
SELECT cov80_test(3303, 'X2_merge_agg', $$SELECT a,b FROM (SELECT a,b,count(*) FROM cov_j1 GROUP BY a,b) t$$);

-- ====================================================================
-- Section Y: 覆盖 rewrite.c 组合规则路径
-- 目标: pg_cascades_logical_rewrite 中的 CombinationRules 分支
--       pg_rewrite_apply_rules_topdown (PRUNE_COLUMNS)
--       pg_rewrite_tree_node (standalone tree rewrite)
-- ====================================================================

\echo '=== Y1: Column pruning chain across Join+Project+Agg ==='
SELECT cov80_test(3401, 'Y1_col_prune_chain', $$SELECT j1.a,sum(j2.d) FROM cov_j1 j1 JOIN cov_j2 j2 ON j1.a=j2.a GROUP BY j1.a$$);

\echo '=== Y2: Top-down column prune through Project→Join→Scan ==='
SELECT cov80_test(3402, 'Y2_topdown_prune', $$SELECT a,d FROM cov_j1 j1 JOIN cov_j2 j2 ON j1.a=j2.a$$);

-- ====================================================================
-- Section Z: 覆盖 task.c 多属性优化路径
-- 目标: pg_cascades_push_enforce_and_cost_tasks 4 种 pathkeys 分支
--       Clone+Resume 完整路径
-- ====================================================================

\echo '=== Z1: All four property types: sort+group+distinct ==='
SELECT cov80_test(3501, 'Z1_all_props', $$SELECT DISTINCT b FROM cov_j1 GROUP BY b ORDER BY b$$);

\echo '=== Z2: Different sort and group keys (forces separate Sort) ==='
SELECT cov80_test(3502, 'Z2_diff_keys', $$SELECT b,sum(a) FROM cov_j1 GROUP BY b ORDER BY sum(a)$$);

\echo '=== Z3: Sort+Limit with specific fraction ==='
SELECT cov80_test(3503, 'Z3_sort_limit_frac', $$SELECT * FROM cov_j1 ORDER BY a LIMIT 1$$);

-- ====================================================================
-- 汇总
-- ====================================================================

SELECT '=== Coverage 80% Test Suite Complete ===' AS status;
SELECT test_id, test_name, cas_path FROM _cov80_results ORDER BY test_id;
SELECT count(*) AS total,
       sum(CASE WHEN cas_path='CASCADES' THEN 1 ELSE 0 END) AS cascades_path,
       sum(CASE WHEN cas_path='FALLBACK' THEN 1 ELSE 0 END) AS fallback_path
FROM _cov80_results;

-- ====================================================================
-- Section AA: planbuild PHYSICAL_* 深度覆盖 — 无索引表全组合
-- ====================================================================

\echo '=== AA1: No-index single-table ORDER BY + LIMIT ==='
SELECT cov80_test(4001, 'AA1_sort_limit', $$SELECT * FROM cov_noindex ORDER BY x LIMIT 5$$);
SELECT cov80_test(4002, 'AA1_sort_offset', $$SELECT * FROM cov_noindex ORDER BY z LIMIT 3 OFFSET 7$$);

\echo '=== AA2: No-index GROUP BY + ORDER BY diff cols (force GroupAgg) ==='
SELECT cov80_test(4003, 'AA2_group_sort_diff', $$SELECT x%10 AS g, count(*) FROM cov_noindex GROUP BY g ORDER BY g$$);
SELECT cov80_test(4004, 'AA2_group_sort_text', $$SELECT y, count(*) FROM cov_noindex GROUP BY y ORDER BY y$$);

\echo '=== AA3: No-index DISTINCT + ORDER BY (force Unique sorted) ==='
SELECT cov80_test(4005, 'AA3_distinct_sort', $$SELECT DISTINCT x FROM cov_noindex ORDER BY x$$);
SELECT cov80_test(4006, 'AA3_distinct_multi', $$SELECT DISTINCT x, z FROM cov_noindex ORDER BY x, z$$);

\echo '=== AA4: Complex expr no-index (planbuild PHYSICAL_PROJECT deep) ==='
SELECT cov80_test(4007, 'AA4_expr', $$SELECT x*2+z AS calc, upper(y) FROM cov_noindex ORDER BY calc$$);

\echo '=== AA5: Having without group (scalar agg + having) ==='
SELECT cov80_test(4008, 'AA5_having', $$SELECT count(*) FROM cov_noindex HAVING count(*) > 0$$);

-- ====================================================================
-- Section BB: task.c limit/timeout 边缘路径
-- ====================================================================

\echo '=== BB1: Very low max_tasks trigger limit path ==='
SET cascades_planner_max_tasks = 3;
SELECT cov80_test(4101, 'BB1_max_tasks_low', $$SELECT * FROM cov_noindex ORDER BY x$$);
SET cascades_planner_max_tasks = 500000;

-- ====================================================================
-- Section CC: decorrelate.c 触发 — correlated scalar subquery
-- ====================================================================

\echo '=== CC1: Correlated scalar subquery ==='
SELECT cov80_test(4201, 'CC1_scalar_corr', $$SELECT t1.a, (SELECT max(t2.d) FROM cov_j2 t2 WHERE t2.a = t1.a) FROM cov_j1 t1$$);
SELECT cov80_test(4202, 'CC1_scalar_corr2', $$SELECT a, (SELECT count(*) FROM cov_j2 WHERE cov_j2.a = cov_j1.a) FROM cov_j1$$);

-- ====================================================================
-- Section DD: rule.c 变换规则 — 更多 JOIN/WHERE 组合
-- ====================================================================

\echo '=== DD1: Join with filter on both sides ==='
SELECT cov80_test(4301, 'DD1_both_filter', $$SELECT t1.a, t2.d FROM cov_j1 t1 JOIN cov_j2 t2 ON t1.a=t2.a WHERE t1.b BETWEEN 3 AND 7 AND t2.d BETWEEN 20 AND 80$$);
SELECT cov80_test(4302, 'DD1_three_filter', $$SELECT j1.a, j2.d, j3.f FROM cov_j1 j1 JOIN cov_j2 j2 ON j1.a=j2.a JOIN cov_j3 j3 ON j1.a=j3.a WHERE j2.d > 50$$);

\echo '=== DD2: NOT IN subquery (anti-join decorrelation) ==='
SELECT cov80_test(4303, 'DD2_not_in_subq', $$SELECT * FROM cov_j1 WHERE a NOT IN (SELECT a FROM cov_j2 WHERE d < 30)$$);

\echo '=== DD3: Scalar subquery in SELECT ==='
SELECT cov80_test(4304, 'DD3_scalar_select', $$SELECT a, (SELECT max(d) FROM cov_j2 WHERE cov_j2.a = cov_j1.a) AS max_d FROM cov_j1$$);

\echo '=== DD4: Self-join with filter pushdown test ==='
SELECT cov80_test(4305, 'DD4_self_pushdown', $$SELECT t1.a, t2.b FROM cov_j1 t1 JOIN cov_j1 t2 ON t1.a=t2.a AND t1.b > t2.b WHERE t1.c='x1'$$);

-- ====================================================================
-- Section EE: memo.c group merge 多场景触发
-- ====================================================================

\echo '=== EE1: Nested subquery (merge project+scan) ==='
SELECT cov80_test(4401, 'EE1_nested', $$SELECT * FROM (SELECT a, b FROM cov_j1 WHERE b > 3) t$$);
SELECT cov80_test(4402, 'EE1_double_nest', $$SELECT a FROM (SELECT a, b FROM (SELECT * FROM cov_j1 WHERE c='x1') t1) t2$$);


-- ====================================================================
-- Section FF: task.c limit 路径 — max_tasks 极低触发 limit 分支
-- ====================================================================

\echo '=== FF1: max_tasks=1 force limit path ==='
SET cascades_planner_max_tasks = 1;
SELECT cov80_test(4501, 'FF1_limit1', $$SELECT * FROM cov_noindex ORDER BY x$$);
SELECT cov80_test(4502, 'FF1_limit1_join', $$SELECT * FROM cov_j1 t1 JOIN cov_j2 t2 ON t1.a=t2.a$$);
SET cascades_planner_max_tasks = 500000;


-- ====================================================================
-- Section GG: planbuild PHYSICAL_GROUPAGG — GROUP BY+ORDER BY 同列强制 GroupAgg
-- ====================================================================

\echo '=== GG1: Force GroupAgg via GROUP BY + ORDER BY same column ==='
SELECT cov80_test(4601, 'GG1_groupagg_same', $$SELECT b, count(*) FROM cov_j1 GROUP BY b ORDER BY b$$);
SELECT cov80_test(4602, 'GG1_groupagg_multi', $$SELECT b, count(*), sum(a) FROM cov_j1 GROUP BY b ORDER BY b$$);

\echo '=== GG2: No-index force GroupAgg (no HashAgg possible via index) ==='
SELECT cov80_test(4603, 'GG2_noindex_groupagg', $$SELECT x%10 AS g, count(*) FROM cov_noindex GROUP BY g ORDER BY g$$);
SELECT cov80_test(4604, 'GG2_noindex_multi', $$SELECT y, count(*), avg(z) FROM cov_noindex GROUP BY y ORDER BY y$$);

-- ====================================================================
-- Section HH: planbuild PHYSICAL_UNIQUE + PHYSICAL_LIMIT 深度
-- ====================================================================

\echo '=== HH1: DISTINCT + ORDER BY same column (force Unique sorted) ==='
SELECT cov80_test(4701, 'HH1_uniq_sorted', $$SELECT DISTINCT b FROM cov_j1 ORDER BY b$$);
SELECT cov80_test(4702, 'HH1_uniq_multi', $$SELECT DISTINCT b, c FROM cov_j1 ORDER BY b, c$$);

\echo '=== HH2: LIMIT + OFFSET combinations ==='
SELECT cov80_test(4703, 'HH2_limit_offset', $$SELECT * FROM cov_noindex ORDER BY x LIMIT 5 OFFSET 10$$);
SELECT cov80_test(4704, 'HH2_limit_only', $$SELECT * FROM cov_noindex LIMIT 3$$);

-- ====================================================================
-- Section II: rule.c 边缘变换规则触发
-- ====================================================================

\echo '=== II1: PushDownPredicateAgg (A3) with complex HAVING ==='
SELECT cov80_test(4801, 'II1_pushdown_agg', $$SELECT b, sum(a) FROM cov_j1 WHERE a > 10 GROUP BY b HAVING sum(a) > 100$$);

\echo '=== II2: PushDownLimitJoin (D2) with LIMIT subquery + JOIN ==='
SELECT cov80_test(4802, 'II2_limit_join_push', $$SELECT * FROM (SELECT * FROM cov_j1 ORDER BY a LIMIT 10) t1 JOIN cov_j2 t2 ON t1.a=t2.a$$);

\echo '=== II3: MergeTwoAgg (F2) nested aggregation ==='
SELECT cov80_test(4803, 'II3_merge_agg', $$SELECT b, max(s) FROM (SELECT b, sum(a) AS s FROM cov_j1 GROUP BY b) t GROUP BY b$$);

\echo '=== II4: EliminateJoinWithConst (G1) single-row side ==='
SELECT cov80_test(4804, 'II4_const_join', $$SELECT j1.* FROM cov_j1 j1 JOIN cascades_single cs ON j1.a = cs.id$$);

\echo '=== II5: OuterJoinElimination (G2) unreferenced LEFT side ==='
SELECT cov80_test(4805, 'II5_outer_elim', $$SELECT j1.a FROM cov_j1 j1 LEFT JOIN cov_j2 j2 ON j1.a=j2.a$$);

\echo '=== II6: PruneEmptyJoin (E1) with impossible condition ==='
SELECT cov80_test(4806, 'II6_prune_empty', $$SELECT * FROM cov_j1 j1 JOIN cov_j2 j2 ON j1.a=j2.a WHERE j1.a < 0$$);

-- ====================================================================
-- Section JJ: planbuild fix_empty_targetlists — trigger INDEX_SCAN path
-- ====================================================================

\echo '=== JJ1: Index scan to trigger fix_empty_targetlists IndexScan cases ==='
SELECT cov80_test(4901, 'JJ1_index_scan', $$SELECT a FROM cov_j1 WHERE a = 42$$);
SELECT cov80_test(4902, 'JJ1_index_only', $$SELECT a FROM cov_j2 WHERE a = 10$$);

\echo '=== JJ2: NestLoop with Material (postopt physical rewrite) ==='
SELECT cov80_test(4903, 'JJ2_nestloop_mat', $$SELECT j1.a, j2.d FROM cov_j1 j1 JOIN cov_j2 j2 ON j1.a=j2.a WHERE j1.b < 3$$);

-- ====================================================================
-- Section KK: cascades.c AlternativeSubPlan + error branches
-- ====================================================================

\echo '=== KK1: Complex subquery in WHERE (AlternativeSubPlan candidate) ==='
SELECT cov80_test(5001, 'KK1_complex_subq', $$SELECT * FROM cov_j1 WHERE a IN (SELECT a FROM cov_j2 WHERE d > 80) AND b > 5$$);


-- ====================================================================
-- Section LL: planbuild PHYSICAL_HASHAGG make_group (GROUP BY without aggregates)
-- ====================================================================

\echo '=== LL1: GROUP BY no aggregate → make_group in PHYSICAL_HASHAGG ==='
SELECT cov80_test(5101, 'LL1_group_noagg', $$SELECT b FROM cov_j1 GROUP BY b$$);
SELECT cov80_test(5102, 'LL1_group_noagg_order', $$SELECT b FROM cov_j1 GROUP BY b ORDER BY b$$);

\echo '=== LL2: GROUP BY no aggregate → make_group in PHYSICAL_GROUPAGG ==='
SELECT cov80_test(5103, 'LL2_groupagg_noagg', $$SELECT b FROM cov_j1 GROUP BY b ORDER BY b$$);

-- ====================================================================
-- Section MM: Index scan queries → fix_empty_targetlists IndexScan branch
-- ====================================================================

\echo '=== MM1: Index scan on primary key / indexed columns ==='
SELECT cov80_test(5201, 'MM1_idx_eq', $$SELECT * FROM coverage_t2 WHERE id = 50$$);
SELECT cov80_test(5202, 'MM1_idx_range', $$SELECT * FROM cov_j1 WHERE a BETWEEN 10 AND 20$$);
SELECT cov80_test(5203, 'MM1_idx_multi', $$SELECT a FROM cov_j2 WHERE a IN (1,2,3)$$);

-- ====================================================================
-- Section NN: Rule edge cases
-- ====================================================================

\echo '=== NN1: where clause on both join sides ==='
SELECT cov80_test(5301, 'NN1_both_side', $$SELECT j1.a, j2.d FROM cov_j1 j1 JOIN cov_j2 j2 ON j1.a=j2.a WHERE j1.b > 5 AND j2.d > 30$$);

\echo '=== NN2: Three-table with ORDER BY ==='
SELECT cov80_test(5302, 'NN2_3table_order', $$SELECT j1.a, j2.d FROM cov_j1 j1 JOIN cov_j2 j2 ON j1.a=j2.a JOIN cov_j3 j3 ON j1.a=j3.a ORDER BY j1.a$$);

