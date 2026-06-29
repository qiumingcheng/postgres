-- Self-join + Subquery coverage (direct queries)
\set ON_ERROR_STOP off
SET enable_cascades_planner = on;
SET cascades_planner_fallback_on_error = on;
SET cascades_planner_debug = off;
SET client_min_messages = warning;

\echo '=== Self-joins ==='
SELECT 'SJ1', count(*) FROM cascades_test_t a JOIN cascades_test_t b ON a.a = b.a WHERE a.b > 10;
SELECT 'SJ2', count(*) FROM (SELECT a.a, count(*) FROM cascades_test_t a JOIN cascades_test_t b ON a.a = b.b GROUP BY a.a) s;
SELECT 'SJ3', count(*) FROM cascades_test_j1 a JOIN cascades_test_j1 b ON a.id = b.id;
SELECT 'SJ4', count(*) FROM cascades_test_j1 a JOIN cascades_test_j1 b ON a.id >= b.id;
SELECT 'SJ5', count(*) FROM cascades_test_t a JOIN cascades_test_t b ON a.a = b.b JOIN cascades_test_t c ON b.a = c.a;
SELECT 'SJ6', count(*) FROM (SELECT DISTINCT a.a, b.b FROM cascades_test_t a JOIN cascades_test_t b ON a.id = b.id) s;
SELECT 'SJ7', count(*) FROM cascades_test_t a JOIN cascades_test_t b ON a.a = b.b WHERE a.a > 10 AND b.b > 10;
SELECT 'SJ8', count(*) FROM (SELECT a.* FROM cascades_test_t a JOIN cascades_test_t b ON a.id = b.id ORDER BY a.a LIMIT 10) s;

\echo '=== Subqueries ==='
SELECT 'SQ1', count(*) FROM cascades_test_t WHERE a = (SELECT max(a) FROM cascades_test_t);
SELECT 'SQ2', count(*) FROM (SELECT a, b FROM cascades_test_t WHERE a > 10) s WHERE s.b > 10;
SELECT 'SQ3', count(*) FROM (SELECT a, b FROM (SELECT * FROM cascades_test_t WHERE b > 10) s1 WHERE a > 10) s2;
SELECT 'SQ5', count(*) FROM cascades_test_j1 t1 JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id WHERE t1.val > (SELECT avg(val) FROM cascades_test_j1);

\echo '=== Vtable dispatch ==='
SELECT 'VT1', grp, count(*) FROM cascades_test_t GROUP BY grp;
SELECT 'VT2', * FROM cascades_test_t ORDER BY a, b;
SELECT 'VT3', * FROM cascades_test_t WHERE a > 10 ORDER BY a LIMIT 10;
SELECT 'VT4', count(*) FROM (SELECT DISTINCT a FROM cascades_test_t ORDER BY a) s;
SELECT 'VT5', grp, count(*) FROM cascades_test_t GROUP BY grp ORDER BY grp LIMIT 5;
SELECT 'VT6', count(*) FROM cascades_test_j1 t1 JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id JOIN cascades_test_j3 t3 ON t2.id = t3.j2_id;

\echo '=== Edge cases ==='
SELECT 'EC1', count(*) FROM cascades_test_t WHERE a > 99999;
SELECT 'EC2', count(*) FROM cascades_test_t WHERE id = 1;
SELECT 'EC3', CASE WHEN a > 50 THEN 'hi' WHEN a > 25 THEN 'mid' ELSE 'lo' END, count(*) FROM cascades_test_t GROUP BY 1;

\echo '=== ALL DONE ==='

\echo '=== Distinct edge cases ==='
SELECT 'DT1', count(*) FROM (SELECT DISTINCT a FROM cascades_test_t) s;
SELECT 'DT2', count(*) FROM (SELECT DISTINCT a, b FROM cascades_test_t) s;
SELECT 'DT3', count(*) FROM (SELECT DISTINCT a FROM cascades_test_t WHERE a > 10 ORDER BY a) s;
SELECT 'DT4', a, count(*) FROM (SELECT DISTINCT a, b FROM cascades_test_t) s GROUP BY a;
SELECT 'DT5', count(*) FROM (SELECT DISTINCT ON (a) a, b FROM cascades_test_t) s;
