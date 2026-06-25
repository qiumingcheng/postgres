-- ============================================================================
-- 基于 gcov 分析的精确测试套件
-- 目标：从 70.3% 提升到 80%
-- 预期新增覆盖：+200 行
-- ============================================================================

\timing on
\set ON_ERROR_STOP off

-- ============================================================================
-- 套件 A: rule.c 高级规则覆盖 (预期 +80 行)
-- ============================================================================

SELECT '=== 套件 A: rule.c 高级规则 ===' as section;

-- A1: 4表 Bushy JOIN 树 (触发 Join Reorder 规则)
SELECT count(*) FROM (
    SELECT t1.id
    FROM cascades_test_j1 t1
    JOIN cascades_test_j2 t2a ON t1.id = t2a.j1_id
    JOIN cascades_test_j2 t2b ON t1.id = t2b.j1_id
    JOIN cascades_test_j3 t3 ON t2a.id = t3.j2_id
    WHERE t2a.val > 10 AND t2b.val < 50
) AS a1;

-- A2: Join Commutativity - LEFT JOIN 转 INNER JOIN
SELECT count(*) FROM (
    SELECT t1.id, t2.val
    FROM cascades_test_j1 t1
    LEFT JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id
    WHERE t2.val IS NOT NULL AND t2.val > 20
) AS a2;

-- A3: Aggregate over multiple joins (触发 Agg pushdown)
SELECT count(*) FROM (
    SELECT t1.id, count(*) as cnt, sum(t2.val) as s2, avg(t3.val) as a3
    FROM cascades_test_j1 t1
    JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id
    JOIN cascades_test_j3 t3 ON t2.id = t3.j2_id
    WHERE t1.val > 15
    GROUP BY t1.id
    HAVING count(*) > 0
) AS a3;

-- A4: Predicate pushdown through UNION ALL
SELECT count(*) FROM (
    SELECT * FROM (
        SELECT id, val, 'j1' as src FROM cascades_test_j1
        UNION ALL
        SELECT j1_id as id, val, 'j2' as src FROM cascades_test_j2
    ) u
    WHERE val > 50 AND val < 80
) AS a4;

-- A5: Predicate pushdown through LEFT JOIN (保留 NULL)
SELECT count(*) FROM (
    SELECT t1.id, t1.val, t2.val as t2_val
    FROM cascades_test_j1 t1
    LEFT JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id AND t2.val > 30
    WHERE t1.val > 20
) AS a5;

-- A6: Multi-level aggregate (Agg over Agg)
SELECT count(*) FROM (
    SELECT avg(cnt) as avg_cnt FROM (
        SELECT j1_id, count(*) as cnt
        FROM cascades_test_j2
        GROUP BY j1_id
        HAVING count(*) > 1
    ) sub
) AS a6;

-- A7: Join with multiple predicates
SELECT count(*) FROM (
    SELECT t1.id
    FROM cascades_test_j1 t1
    JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id AND t1.val + t2.val > 50
    WHERE t1.val > 10 AND t2.val < 80
) AS a7;

-- A8: Complex GROUP BY with HAVING
SELECT count(*) FROM (
    SELECT t1.id, count(DISTINCT t2.val) as distinct_vals
    FROM cascades_test_j1 t1
    JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id
    GROUP BY t1.id
    HAVING count(DISTINCT t2.val) > 0 AND sum(t2.val) > 50
) AS a8;

-- ============================================================================
-- 套件 B: decorrelate.c 复杂关联子查询 (预期 +30 行)
-- ============================================================================

SELECT '=== 套件 B: decorrelate.c 复杂关联 ===' as section;

-- B1: 3层嵌套 EXISTS (深度关联)
SELECT count(*) FROM (
    SELECT t1.id
    FROM cascades_test_j1 t1
    WHERE EXISTS (
        SELECT 1 FROM cascades_test_j2 t2
        WHERE t2.j1_id = t1.id AND t2.val > t1.val / 2
          AND EXISTS (
              SELECT 1 FROM cascades_test_j3 t3
              WHERE t3.j2_id = t2.id AND t3.val < t1.val
          )
    )
) AS b1;

-- B2: 非等值关联 (>)
SELECT count(*) FROM (
    SELECT t1.id
    FROM cascades_test_j1 t1
    WHERE EXISTS (
        SELECT 1 FROM cascades_test_j2 t2
        WHERE t2.val > t1.val AND t2.j1_id != t1.id
    )
) AS b2;

-- B3: IN 子查询with关联条件
SELECT count(*) FROM (
    SELECT t1.id
    FROM cascades_test_j1 t1
    WHERE t1.id IN (
        SELECT t2.j1_id FROM cascades_test_j2 t2
        WHERE t2.val > t1.val * 0.5
    )
) AS b3;

-- B4: NOT EXISTS with关联
SELECT count(*) FROM (
    SELECT t1.id
    FROM cascades_test_j1 t1
    WHERE NOT EXISTS (
        SELECT 1 FROM cascades_test_j2 t2
        WHERE t2.j1_id = t1.id AND t2.val > 80
    )
) AS b4;

-- B5: 关联Scalar子查询在SELECT
SELECT count(*) FROM (
    SELECT t1.id,
           (SELECT max(val) FROM cascades_test_j2 WHERE j1_id = t1.id) as max_val,
           (SELECT count(*) FROM cascades_test_j2 WHERE j1_id = t1.id AND val > t1.val) as cnt
    FROM cascades_test_j1 t1
) AS b5;

-- B6: ANY/ALL 关联
SELECT count(*) FROM (
    SELECT t1.id
    FROM cascades_test_j1 t1
    WHERE t1.val > ANY(SELECT val FROM cascades_test_j2 WHERE j1_id = t1.id)
) AS b6;

-- ============================================================================
-- 套件 C: rewrite.c CTE和复杂查询 (预期 +25 行)
-- ============================================================================

SELECT '=== 套件 C: rewrite.c CTE和复杂查询 ===' as section;

-- C1: 深层嵌套 CTE
WITH cte1 AS (
    SELECT id, val FROM cascades_test_j1 WHERE val > 20
),
cte2 AS (
    SELECT cte1.id, t2.val as val2
    FROM cte1
    JOIN cascades_test_j2 t2 ON cte1.id = t2.j1_id
    WHERE cte1.val + t2.val > 30
),
cte3 AS (
    SELECT cte2.id, t3.val as val3
    FROM cte2
    JOIN cascades_test_j3 t3 ON cte2.id = t3.j2_id
)
SELECT count(*) FROM cte3;

-- C2: CTE 自引用（非递归）
WITH base AS (
    SELECT j1_id, count(*) as cnt, sum(val) as total
    FROM cascades_test_j2
    GROUP BY j1_id
)
SELECT count(*) FROM (
    SELECT b1.j1_id, b1.cnt, b2.total
    FROM base b1, base b2
    WHERE b1.j1_id = b2.j1_id AND b1.cnt > 1
) AS c2;

-- C3: 复杂子查询with多个层级
SELECT count(*) FROM (
    SELECT *
    FROM (
        SELECT *
        FROM (
            SELECT id, val, val * 2 as doubled
            FROM cascades_test_j1
        ) l1
        WHERE doubled > 40
    ) l2
    WHERE val < 80
) AS c3;

-- C4: UNION with复杂子查询
SELECT count(*) FROM (
    SELECT id, val FROM (
        SELECT id, val FROM cascades_test_j1 WHERE val < 30
        UNION
        SELECT id, val FROM cascades_test_j1 WHERE val > 70
    ) u1
    UNION
    SELECT j1_id as id, val FROM cascades_test_j2 WHERE val BETWEEN 40 AND 60
) AS c4;

-- C5: 多层子查询with聚合
SELECT count(*) FROM (
    SELECT subq.category, sum(subq.cnt) as total
    FROM (
        SELECT CASE
                   WHEN val < 33 THEN 'low'
                   WHEN val < 67 THEN 'mid'
                   ELSE 'high'
               END as category,
               count(*) as cnt
        FROM cascades_test_j1
        GROUP BY category
    ) subq
    GROUP BY subq.category
) AS c5;

-- ============================================================================
-- 套件 D: planbuild.c Enforcer场景 (预期 +35 行)
-- ============================================================================

SELECT '=== 套件 D: planbuild.c Enforcer ===' as section;

-- D1: 嵌套排序（强制 Sort enforcer）
SELECT count(*) FROM (
    SELECT * FROM (
        SELECT id, val FROM cascades_test_j1 ORDER BY val DESC
    ) sub
    ORDER BY id ASC
) AS d1;

-- D2: Hash Aggregate with多个聚合函数
SELECT count(*) FROM (
    SELECT val,
           count(*) as cnt,
           count(DISTINCT id) as dist_cnt,
           sum(id) as sum_id,
           avg(id) as avg_id,
           min(id) as min_id,
           max(id) as max_id
    FROM cascades_test_j1
    GROUP BY val
    HAVING count(*) > 0
) AS d2;

-- D3: 复杂 property 传播 (Sort + Limit + Join)
SELECT count(*) FROM (
    SELECT t1.id, t1.val, t2.val as val2
    FROM cascades_test_j1 t1
    JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id
    WHERE t1.val > 10
    ORDER BY t2.val DESC, t1.id ASC
    LIMIT 15
) AS d3;

-- D4: Distinct with复杂排序
SELECT count(*) FROM (
    SELECT DISTINCT id, val
    FROM cascades_test_j1
    WHERE val > 20
    ORDER BY val DESC, id ASC
) AS d4;

-- D5: Group Aggregate with Sort
SELECT count(*) FROM (
    SELECT j1_id, count(*) as cnt
    FROM cascades_test_j2
    GROUP BY j1_id
    ORDER BY cnt DESC, j1_id ASC
    LIMIT 10
) AS d5;

-- ============================================================================
-- 套件 E: 边界和错误情况 (预期 +30 行)
-- ============================================================================

SELECT '=== 套件 E: 边界和错误情况 ===' as section;

-- E1: 空结果集传播through JOIN
SELECT count(*) FROM (
    SELECT t1.id, t2.val
    FROM (SELECT * FROM cascades_test_j1 WHERE 1=0) t1
    JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id
) AS e1;

-- E2: NULL 处理 - 只保留 NULL
SELECT count(*) FROM (
    SELECT t1.id, t2.val
    FROM cascades_test_j1 t1
    LEFT JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id
    WHERE t2.val IS NULL
) AS e2;

-- E3: COALESCE 处理 NULL
SELECT count(*) FROM (
    SELECT t1.id, COALESCE(t2.val, -1) as safe_val, COALESCE(t3.val, -1) as safe_val3
    FROM cascades_test_j1 t1
    LEFT JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id
    LEFT JOIN cascades_test_j3 t3 ON t2.id = t3.j2_id
) AS e3;

-- E4: CASE with NULL
SELECT count(*) FROM (
    SELECT t1.id,
           CASE WHEN t2.val IS NULL THEN 'null'
                WHEN t2.val < 50 THEN 'low'
                ELSE 'high'
           END as category
    FROM cascades_test_j1 t1
    LEFT JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id
) AS e4;

-- E5: UNION with NULL
SELECT count(*) FROM (
    SELECT id, val FROM cascades_test_j1
    UNION ALL
    SELECT j1_id, NULL as val FROM cascades_test_j2 WHERE val IS NULL
) AS e5;

-- E6: Self JOIN with不等条件
SELECT count(*) FROM (
    SELECT a.id, b.id as id2
    FROM cascades_test_j1 a
    JOIN cascades_test_j1 b ON a.val = b.val AND a.id <> b.id
    WHERE a.id < b.id
) AS e6;

\timing off
\set ON_ERROR_STOP on

SELECT '=== gcov 分析测试套件完成 ===' as status,
       '预期提升: +200 行覆盖 (70.3% → 75-76%)' as expected,
       current_timestamp as completed_at;
