-- Self-join + Subquery coverage tests
\set ON_ERROR_STOP off
SET enable_cascades_planner = on;
SET cascades_planner_fallback_on_error = on;
SET cascades_planner_debug = off;
SET client_min_messages = warning;

-- ====================================================================
-- Self-joins
-- ====================================================================
SELECT cov_test(9001, 'SJ: eq self-join', $$SELECT count(*) FROM cascades_test_t a JOIN cascades_test_t b ON a.a = b.a WHERE a.b > 10$$);
SELECT cov_test(9002, 'SJ: self-join agg', $$SELECT count(*) FROM (SELECT a.a, count(*) FROM cascades_test_t a JOIN cascades_test_t b ON a.a = b.b GROUP BY a.a) sub$$);
SELECT cov_test(9003, 'SJ: self-join sort limit', $$SELECT count(*) FROM (SELECT a.* FROM cascades_test_t a JOIN cascades_test_t b ON a.id = b.id ORDER BY a.a LIMIT 10) sub$$);
SELECT cov_test(9004, 'SJ: pk self-join', $$SELECT count(*) FROM cascades_test_j1 a JOIN cascades_test_j1 b ON a.id = b.id$$);
SELECT cov_test(9005, 'SJ: inequality self-join', $$SELECT count(*) FROM cascades_test_j1 a JOIN cascades_test_j1 b ON a.id >= b.id$$);
SELECT cov_test(9006, 'SJ: triple self-join', $$SELECT count(*) FROM cascades_test_t a JOIN cascades_test_t b ON a.a = b.b JOIN cascades_test_t c ON b.a = c.a$$);
SELECT cov_test(9007, 'SJ: distinct self-join', $$SELECT count(*) FROM (SELECT DISTINCT a.a, b.b FROM cascades_test_t a JOIN cascades_test_t b ON a.id = b.id) sub$$);
SELECT cov_test(9008, 'SJ: filter self-join', $$SELECT count(*) FROM cascades_test_t a JOIN cascades_test_t b ON a.a = b.b WHERE a.a > 10 AND b.b > 10$$);

-- ====================================================================
-- Uncorrelated subqueries
-- ====================================================================
SELECT cov_test(9101, 'SQ: scalar subq WHERE', $$SELECT count(*) FROM cascades_test_t WHERE a = (SELECT max(a) FROM cascades_test_t)$$);
SELECT cov_test(9102, 'SQ: from subq', $$SELECT count(*) FROM (SELECT a, b FROM cascades_test_t WHERE a > 10) sub WHERE sub.b > 10$$);
SELECT cov_test(9103, 'SQ: nested from subq', $$SELECT count(*) FROM (SELECT a, b FROM (SELECT * FROM cascades_test_t WHERE b > 10) s1 WHERE a > 10) s2$$);
SELECT cov_test(9104, 'SQ: scalar comp WHERE', $$SELECT count(*) FROM cascades_test_t WHERE b > (SELECT avg(b) FROM cascades_test_t)$$);
SELECT cov_test(9105, 'SQ: having subq', $$SELECT count(*) FROM (SELECT a, count(*) FROM cascades_test_t GROUP BY a HAVING count(*) > (SELECT avg(b) FROM cascades_test_t)) sub$$);
SELECT cov_test(9106, 'SQ: join+subq', $$SELECT count(*) FROM cascades_test_j1 t1 JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id WHERE t1.val > (SELECT avg(val) FROM cascades_test_j1)$$);

-- ====================================================================
-- Vtable dispatch
-- ====================================================================
SELECT cov_test(9201, 'VT: hashagg', $$SELECT grp, count(*) FROM cascades_test_t GROUP BY grp$$);
SELECT cov_test(9202, 'VT: sort multi', $$SELECT * FROM cascades_test_t ORDER BY a, b$$);
SELECT cov_test(9203, 'VT: sort limit', $$SELECT * FROM cascades_test_t WHERE a > 10 ORDER BY a LIMIT 10$$);
SELECT cov_test(9204, 'VT: distinct sort', $$SELECT count(*) FROM (SELECT DISTINCT a FROM cascades_test_t ORDER BY a) sub$$);
SELECT cov_test(9205, 'VT: agg sort limit', $$SELECT grp, count(*) FROM cascades_test_t GROUP BY grp ORDER BY grp LIMIT 5$$);
SELECT cov_test(9206, 'VT: 3table join', $$SELECT count(*) FROM cascades_test_j1 t1 JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id JOIN cascades_test_j3 t3 ON t2.id = t3.j2_id$$);
SELECT cov_test(9207, 'VT: filter pushdown', $$SELECT count(*) FROM cascades_test_t WHERE a > 10 AND b > 10$$);

-- ====================================================================
-- Edge cases
-- ====================================================================
SELECT cov_test(9301, 'EC: empty result', $$SELECT count(*) FROM cascades_test_t WHERE a > 99999$$);
SELECT cov_test(9302, 'EC: single row', $$SELECT count(*) FROM cascades_test_t WHERE id = 1$$);
SELECT cov_test(9303, 'EC: case group', $$SELECT CASE WHEN a > 50 THEN 'high' WHEN a > 25 THEN 'mid' ELSE 'low' END, count(*) FROM cascades_test_t GROUP BY 1$$);

SELECT '=== coverage_selfjoin_subquery COMPLETE ===' AS result;
