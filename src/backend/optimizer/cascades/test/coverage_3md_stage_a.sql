-- ============================================================================
-- 3.md 实施：阶段 A - SQL 回归测试
-- 目标：从 68.7% 提升到 74-76%
-- 预期净增：+300 行覆盖
-- ============================================================================

\timing on
\set ON_ERROR_STOP off

-- ============================================================================
-- Part A1: PlanBuild fallback logical path (planbuild.c)
-- 目标：+100 到 +140 行
-- ============================================================================

SELECT '=== A1: PlanBuild Logical Path Tests ===' as section;

-- Test A1.1: Project over join
SELECT count(*) FROM (
    SELECT t1.val + t2.val AS s
    FROM cascades_test_j1 t1
    JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id
    WHERE t1.val > 10
) AS a1_1;

-- Test A1.2: Agg plain
SELECT count(*) FROM (
    SELECT count(*) as cnt
    FROM cascades_test_j1
) AS a1_2;

-- Test A1.3: Agg grouped + having
SELECT count(*) FROM (
    SELECT t1.id, count(*) as cnt, sum(t2.val) as total
    FROM cascades_test_j1 t1
    JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id
    GROUP BY t1.id
    HAVING count(*) > 1
) AS a1_3;

-- Test A1.4: Sort only
SELECT count(*) FROM (
    SELECT t1.id, t1.val
    FROM cascades_test_j1 t1
    ORDER BY t1.val, t1.id
) AS a1_4;

-- Test A1.5: Sort + Limit
SELECT count(*) FROM (
    SELECT t1.id, t1.val
    FROM cascades_test_j1 t1
    ORDER BY t1.val DESC
    LIMIT 7
) AS a1_5;

-- Test A1.6: Limit only
SELECT count(*) FROM (
    SELECT t1.id, t1.val
    FROM cascades_test_j1 t1
    LIMIT 5 OFFSET 2
) AS a1_6;

-- Test A1.7: Distinct
SELECT count(*) FROM (
    SELECT DISTINCT val
    FROM cascades_test_j1
    ORDER BY val
) AS a1_7;

-- Test A1.8: Distinct multi-column
SELECT count(*) FROM (
    SELECT DISTINCT id, val
    FROM cascades_test_j1
    ORDER BY id, val
) AS a1_8;

-- ============================================================================
-- Part A2: Rule Coverage (rule.c, rewrite.c)
-- 目标：+150 到 +220 行
-- ============================================================================

SELECT '=== A2: Rule Coverage Tests ===' as section;

-- Test A2.1: Predicate pushdown scan
SELECT count(*) FROM (
    SELECT id, val
    FROM cascades_test_j1
    WHERE val > 10 AND id < 80
) AS a2_1;

-- Test A2.2: Predicate pushdown project
SELECT count(*) FROM (
    SELECT *
    FROM (
        SELECT id, val, val + 1 AS v2
        FROM cascades_test_j1
    ) s
    WHERE v2 > 20
) AS a2_2;

-- Test A2.3: Predicate pushdown join
SELECT count(*) FROM (
    SELECT t1.id, t2.val
    FROM cascades_test_j1 t1
    JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id
    WHERE t1.val > 10 AND t2.val < 300
) AS a2_3;

-- Test A2.4: Predicate pushdown agg
SELECT count(*) FROM (
    SELECT id, count(*) as cnt
    FROM cascades_test_j1
    GROUP BY id
    HAVING id < 20
) AS a2_4;

-- Test A2.5: Union predicate pushdown
SELECT count(*) FROM (
    SELECT *
    FROM (
        SELECT id, val FROM cascades_test_j1
        UNION ALL
        SELECT j1_id AS id, val FROM cascades_test_j2
    ) u
    WHERE val > 50
) AS a2_5;

-- Test A2.6: Join column prune shape (中间 join key 不在 SELECT)
-- 注意：必须验证没有 C3203 错误
SELECT count(*) FROM (
    SELECT t1.val as v1, t3.val as v3
    FROM cascades_test_j1 t1
    JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id
    JOIN cascades_test_j3 t3 ON t2.id = t3.j2_id
) AS a2_6;

-- ============================================================================
-- Part A3: Rewrite pipeline组合 (rewrite.c, rule.c, planbuild.c)
-- 目标：+40 到 +80 行
-- ============================================================================

SELECT '=== A3: Rewrite Pipeline Tests ===' as section;

-- Test A3.1: Filter + project + sort + limit
SELECT count(*) FROM (
    SELECT *
    FROM (
        SELECT id, val, val * 2 AS doubled
        FROM cascades_test_j1
        WHERE id > 5
    ) s
    WHERE doubled > 40
    ORDER BY doubled
    LIMIT 10
) AS a3_1;

-- Test A3.2: Nested project merge
SELECT count(*) FROM (
    SELECT id, v2
    FROM (
        SELECT id, val + 1 AS v2
        FROM (
            SELECT id, val
            FROM cascades_test_j1
        ) q1
    ) q2
    WHERE v2 < 50
) AS a3_2;

-- Test A3.3: Limit child limit merge
SELECT count(*) FROM (
    SELECT *
    FROM (
        SELECT id, val
        FROM cascades_test_j1
        ORDER BY id
        LIMIT 20
    ) s
    LIMIT 5
) AS a3_3;

-- Test A3.4: Filter + agg + sort
SELECT count(*) FROM (
    SELECT id, count(*) as cnt
    FROM cascades_test_j1
    WHERE val > 15
    GROUP BY id
    HAVING count(*) > 0
    ORDER BY cnt DESC
) AS a3_4;

-- Test A3.5: Union + filter + sort
SELECT count(*) FROM (
    SELECT *
    FROM (
        SELECT id, val FROM cascades_test_j1
        UNION ALL
        SELECT j1_id, val FROM cascades_test_j2
    ) u
    WHERE val BETWEEN 20 AND 80
    ORDER BY val
    LIMIT 50
) AS a3_5;

-- ============================================================================
-- Part A4: Task/Enforcer Tests (task.c, property.c)
-- 目标：+35 到 +60 行
-- ============================================================================

SELECT '=== A4: Task/Enforcer Tests ===' as section;

-- Test A4.1: Enforce sort
SELECT count(*) FROM (
    SELECT id, val
    FROM cascades_test_j1
    ORDER BY val, id
) AS a4_1;

-- Test A4.2: Group agg requiring sorted input
SELECT count(*) FROM (
    SELECT val, count(*) as cnt
    FROM cascades_test_j1
    GROUP BY val
    ORDER BY val
) AS a4_2;

-- Test A4.3: Hash agg
SELECT count(*) FROM (
    SELECT val, count(*) as cnt
    FROM cascades_test_j1
    GROUP BY val
) AS a4_3;

-- Test A4.4: Distinct + order
SELECT count(*) FROM (
    SELECT DISTINCT val
    FROM cascades_test_j1
    ORDER BY val
) AS a4_4;

-- Test A4.5: Nested joins to exercise child required props
SELECT count(*) FROM (
    SELECT t1.id, t2.val, t3.val
    FROM cascades_test_j1 t1
    JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id
    JOIN cascades_test_j3 t3 ON t2.id = t3.j2_id
    WHERE t1.val > 10 AND t3.val < 500
    ORDER BY t2.val
    LIMIT 20
) AS a4_5;

-- ============================================================================
-- 额外测试：补充多样化查询模式
-- ============================================================================

SELECT '=== A5: Additional Diverse Queries ===' as section;

-- Test A5.1: 多级聚合
SELECT count(*) FROM (
    SELECT avg(cnt) FROM (
        SELECT j1_id, count(*) as cnt
        FROM cascades_test_j2
        GROUP BY j1_id
    ) subq
) AS a5_1;

-- Test A5.2: CASE 表达式
SELECT count(*) FROM (
    SELECT id,
           CASE
               WHEN val < 25 THEN 'low'
               WHEN val < 75 THEN 'medium'
               ELSE 'high'
           END as category
    FROM cascades_test_j1
) AS a5_2;

-- Test A5.3: 多个聚合函数
SELECT count(*) FROM (
    SELECT j1_id,
           count(*) as cnt,
           sum(val) as total,
           avg(val) as avg_val,
           min(val) as min_val,
           max(val) as max_val
    FROM cascades_test_j2
    GROUP BY j1_id
) AS a5_3;

-- Test A5.4: LEFT JOIN
SELECT count(*) FROM (
    SELECT t1.id, t2.val
    FROM cascades_test_j1 t1
    LEFT JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id
    WHERE t1.val > 20
) AS a5_4;

-- Test A5.5: COALESCE
SELECT count(*) FROM (
    SELECT t1.id, COALESCE(t2.val, 0) as safe_val
    FROM cascades_test_j1 t1
    LEFT JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id
) AS a5_5;

-- Test A5.6: UNION (not UNION ALL)
SELECT count(*) FROM (
    SELECT id FROM cascades_test_j1 WHERE val < 50
    UNION
    SELECT j1_id as id FROM cascades_test_j2 WHERE val < 50
) AS a5_6;

-- Test A5.7: INTERSECT
SELECT count(*) FROM (
    SELECT id FROM cascades_test_j1 WHERE val > 20
    INTERSECT
    SELECT j1_id as id FROM cascades_test_j2 WHERE val > 10
) AS a5_7;

-- Test A5.8: EXCEPT
SELECT count(*) FROM (
    SELECT id FROM cascades_test_j1
    EXCEPT
    SELECT j1_id as id FROM cascades_test_j2 WHERE val > 70
) AS a5_8;

-- Test A5.9: Self JOIN
SELECT count(*) FROM (
    SELECT a.id, b.id
    FROM cascades_test_j1 a
    JOIN cascades_test_j1 b ON a.val = b.val
    WHERE a.id < b.id
) AS a5_9;

-- Test A5.10: 复杂 WHERE 条件
SELECT count(*) FROM (
    SELECT id, val
    FROM cascades_test_j1
    WHERE (val > 10 AND val < 90)
       OR (id > 50 AND val > 50)
) AS a5_10;

\timing off
\set ON_ERROR_STOP on

SELECT '=== 阶段 A: SQL 回归测试完成 ===' as status,
       '预期提升: +300 行覆盖 (68.7% → 74-76%)' as expected,
       current_timestamp as completed_at;
