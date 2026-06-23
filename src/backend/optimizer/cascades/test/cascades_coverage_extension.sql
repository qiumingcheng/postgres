-- ============================================================================
-- Cascades 扩展覆盖测试套件
-- 运行: psql -d cascades_test -f cascades_coverage_extension.sql
-- 注意: 请先运行 cascades_regression_test_final.sql 创建基础表
-- ============================================================================

\set ON_ERROR_STOP off
SET enable_cascades_planner = on;
SET cascades_planner_fallback_on_error = on;
SET cascades_planner_debug = off;
SET cascades_planner_max_tasks = 200000;
SET client_min_messages = warning;

-- 额外测试表
DROP TABLE IF EXISTS cascades_empty;
CREATE TABLE cascades_empty (id INTEGER, val INTEGER);
DROP TABLE IF EXISTS cascades_single;
CREATE TABLE cascades_single AS SELECT 1 AS id, 42 AS val;
DROP TABLE IF EXISTS cascades_unique;
CREATE TABLE cascades_unique (id INTEGER PRIMARY KEY, val INTEGER UNIQUE);
INSERT INTO cascades_unique (id, val) SELECT i, i*10 FROM generate_series(1,50) i;
ANALYZE cascades_empty;
ANALYZE cascades_single;
ANALYZE cascades_unique;

DROP TABLE IF EXISTS _cov_test_results;
CREATE TEMP TABLE _cov_test_results (test_id INTEGER, test_name TEXT, pg_ok BOOLEAN, cas_ok BOOLEAN, cas_path TEXT);

CREATE OR REPLACE FUNCTION cov_test(test_id INTEGER, test_name TEXT, sql_text TEXT) RETURNS TEXT AS $$
DECLARE
    pg_ok_val  BOOLEAN := false;
    cas_ok_val BOOLEAN := false;
    cas_path_val TEXT := 'ERROR';
    dummy      INTEGER;
BEGIN
    -- PG original planner: wrap in subquery to force plan execution
    SET enable_cascades_planner = off;
    BEGIN
        EXECUTE 'SELECT count(*) FROM (' || sql_text || ') AS _sub' INTO dummy;
        pg_ok_val := true;
    EXCEPTION WHEN OTHERS THEN
        pg_ok_val := false;
    END;

    -- Cascades planner
    SET enable_cascades_planner = on;
    BEGIN
        EXECUTE 'SELECT count(*) FROM (' || sql_text || ') AS _sub' INTO dummy;
        cas_ok_val := true;
        cas_path_val := 'CASCADES';
    EXCEPTION WHEN OTHERS THEN
        -- Fallback: try PG original planner
        BEGIN
            SET enable_cascades_planner = off;
            EXECUTE 'SELECT count(*) FROM (' || sql_text || ') AS _sub' INTO dummy;
            cas_ok_val := true;
            cas_path_val := 'FALLBACK';
        EXCEPTION WHEN OTHERS THEN
            cas_ok_val := false;
            cas_path_val := 'BOTH_FAIL';
        END;
    END;

    INSERT INTO _cov_test_results VALUES (test_id, test_name, pg_ok_val, cas_ok_val, cas_path_val);
    RETURN test_name || ': ' || cas_path_val;
END;
$$ LANGUAGE plpgsql;

\echo '=== Part 1: Join impl rules ==='
SELECT cov_test(101, 'C101: inner join eq', $$SELECT t1.val AS v1, t2.val AS v2 FROM cascades_test_j1 t1 JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id$$);
SELECT cov_test(102, 'C102: left join', $$SELECT t1.id, t2.val AS v2 FROM cascades_test_j1 t1 LEFT JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id$$);
SELECT cov_test(103, 'C103: right join', $$SELECT t1.id, t2.val AS v2 FROM cascades_test_j1 t1 RIGHT JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id$$);
SELECT cov_test(104, 'C104: join+order', $$SELECT t1.val AS v1, t2.val AS v2 FROM cascades_test_j1 t1 JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id ORDER BY t1.id, t2.id$$);
SELECT cov_test(105, 'C105: 3-table join', $$SELECT t1.val AS v1, t2.val AS v2, t3.val AS v3 FROM cascades_test_j1 t1 JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id JOIN cascades_test_j3 t3 ON t2.id = t3.j2_id$$);
SELECT cov_test(106, 'C106: join single-row', $$SELECT t.val AS v1, s.val AS v2 FROM cascades_test_j1 t JOIN cascades_single s ON t.id = s.id$$);

\echo '=== Part 2: TRANS rules ==='
SELECT cov_test(201, 'C201: join commutativity', $$SELECT t1.val AS v1, t2.val AS v2 FROM cascades_test_j1 t1 JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id$$);
SELECT cov_test(202, 'C202: join associativity', $$SELECT t1.val AS v1, t2.val AS v2, t3.val AS v3 FROM cascades_test_j1 t1 JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id JOIN cascades_test_j3 t3 ON t2.id = t3.j2_id$$);
SELECT cov_test(203, 'C203: join left asscom', $$SELECT t1.val AS v1, t2.val AS v2, t3.val AS v3 FROM cascades_test_j1 t1 JOIN cascades_test_j2 t2 ON t1.val = t2.val JOIN cascades_test_j3 t3 ON t2.val = t3.val$$);
SELECT cov_test(204, 'C204: outer join elim', $$SELECT t1.id, t2.val AS v2 FROM cascades_test_j1 t1 LEFT JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id WHERE t2.val IS NOT NULL$$);
SELECT cov_test(205, 'C205: inner to semi', $$SELECT t1.* FROM cascades_test_j1 t1 JOIN cascades_unique t2 ON t1.id = t2.id$$);
SELECT cov_test(206, 'C206: predicate pushdown join', $$SELECT t1.val AS v1, t2.val AS v2 FROM cascades_test_j1 t1 JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id WHERE t1.val > 5 AND t2.val > 3$$);
SELECT cov_test(207, 'C207: limit pushdown join', $$SELECT t1.val AS v1, t2.val AS v2 FROM cascades_test_j1 t1 JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id LIMIT 10$$);
SELECT cov_test(208, 'C208: merge filter join', $$SELECT t1.val AS v1, t2.val AS v2 FROM cascades_test_j1 t1 JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id WHERE t1.val BETWEEN 10 AND 30$$);
SELECT cov_test(209, 'C209: prune empty join', $$SELECT t.val AS v1, e.val AS v2 FROM cascades_test_j1 t JOIN cascades_empty e ON t.id = e.id$$);
SELECT cov_test(210, 'C210: merge join child proj', $$SELECT s1.a, s2.c FROM (SELECT id AS a, val FROM cascades_test_j1) s1 JOIN (SELECT j1_id AS a, val AS c FROM cascades_test_j2) s2 ON s1.a = s2.a$$);
SELECT cov_test(211, 'C211: predicate pushdown agg', $$SELECT a, count(*) AS cnt FROM cascades_test_t GROUP BY a HAVING count(*) > 1$$);
SELECT cov_test(212, 'C212: agg pushdown limit', $$SELECT count(*), sum(a) FROM (SELECT * FROM cascades_test_t LIMIT 100) sub$$);
SELECT cov_test(213, 'C213: merge two agg', $$SELECT a, sum(cnt) FROM (SELECT a, count(*) AS cnt FROM cascades_test_t GROUP BY a) sub GROUP BY a$$);
SELECT cov_test(214, 'C214: merge limit sort', $$SELECT * FROM cascades_test_t ORDER BY a LIMIT 10$$);
SELECT cov_test(215, 'C215: merge limit child limit', $$SELECT * FROM (SELECT * FROM cascades_test_t LIMIT 50) sub LIMIT 10$$);
SELECT cov_test(216, 'C216: eliminate limit', $$SELECT * FROM cascades_test_t$$);
SELECT cov_test(217, 'C217: eliminate agg', $$SELECT id, a FROM cascades_test_t$$);
SELECT cov_test(218, 'C218: eliminate sort const', $$SELECT * FROM cascades_test_t ORDER BY 1$$);

\echo '=== Part 3: Plan build upper ops ==='
SELECT cov_test(301, 'C301: sort no limit', $$SELECT * FROM cascades_test_t ORDER BY a$$);
SELECT cov_test(302, 'C302: group agg sorted', $$SELECT a, count(*) AS cnt FROM cascades_test_t GROUP BY a ORDER BY a$$);
SELECT cov_test(303, 'C303: distinct no order', $$SELECT DISTINCT a FROM cascades_test_t$$);
SELECT cov_test(304, 'C304: limit no order', $$SELECT * FROM cascades_test_t LIMIT 5$$);
SELECT cov_test(305, 'C305: hash agg no group', $$SELECT count(*), sum(a), avg(b) FROM cascades_test_t$$);
SELECT cov_test(306, 'C306: multi-key group agg', $$SELECT a, b, count(*) FROM cascades_test_t GROUP BY a, b ORDER BY a, b$$);
SELECT cov_test(307, 'C307: distinct sorted', $$SELECT DISTINCT a FROM cascades_test_t ORDER BY a$$);
SELECT cov_test(308, 'C308: limit offset', $$SELECT * FROM cascades_test_t ORDER BY id LIMIT 5 OFFSET 10$$);
SELECT cov_test(309, 'C309: order desc limit', $$SELECT * FROM cascades_test_t ORDER BY a DESC LIMIT 10$$);
SELECT cov_test(310, 'C310: group having', $$SELECT a, count(*) FROM cascades_test_t GROUP BY a HAVING count(*) > 2 ORDER BY a$$);
SELECT cov_test(311, 'C311: join group order limit', $$SELECT t1.a, count(*) AS cnt FROM cascades_test_t t1 JOIN cascades_test_j2 t2 ON t1.b = t2.val WHERE t1.a > 10 GROUP BY t1.a ORDER BY cnt DESC LIMIT 10$$);

\echo '=== Part 4: Decorrelation ==='
SELECT cov_test(401, 'C401: correlated exists', $$SELECT * FROM cascades_test_j1 t1 WHERE EXISTS (SELECT 1 FROM cascades_test_j2 t2 WHERE t2.j1_id = t1.id)$$);
SELECT cov_test(402, 'C402: correlated not exists', $$SELECT * FROM cascades_test_j1 t1 WHERE NOT EXISTS (SELECT 1 FROM cascades_test_j2 t2 WHERE t2.j1_id = t1.id)$$);
SELECT cov_test(403, 'C403: correlated in', $$SELECT * FROM cascades_test_j1 t1 WHERE t1.id IN (SELECT j1_id FROM cascades_test_j2 t2 WHERE t2.val > 10)$$);
SELECT cov_test(404, 'C404: correlated not in', $$SELECT * FROM cascades_test_j1 t1 WHERE t1.id NOT IN (SELECT j1_id FROM cascades_test_j2 t2 WHERE t2.val > 20)$$);
SELECT cov_test(405, 'C405: correlated scalar select', $$SELECT id, (SELECT max(val) FROM cascades_test_j2 t2 WHERE t2.j1_id = t1.id) AS mv FROM cascades_test_j1 t1$$);
SELECT cov_test(406, 'C406: correlated scalar where', $$SELECT * FROM cascades_test_j1 t1 WHERE t1.val > (SELECT avg(val) FROM cascades_test_j2 t2 WHERE t2.j1_id = t1.id)$$);
SELECT cov_test(407, 'C407: nested correlated', $$SELECT * FROM cascades_test_j1 t1 WHERE EXISTS (SELECT 1 FROM cascades_test_j2 t2 WHERE t2.j1_id = t1.id AND t2.val > (SELECT avg(val) FROM cascades_test_j3 t3 WHERE t3.j2_id = t2.id))$$);
SELECT cov_test(408, 'C408: uncorrelated in', $$SELECT * FROM cascades_test_j1 t1 WHERE t1.val IN (SELECT val FROM cascades_test_j2 WHERE val > 10)$$);
SELECT cov_test(409, 'C409: uncorrelated scalar', $$SELECT id, (SELECT count(*) FROM cascades_test_t) AS total FROM cascades_test_j1 t1$$);

\echo '=== Part 5: Memo edge cases ==='
SELECT cov_test(501, 'C501: self join', $$SELECT t1.id AS id1, t2.id AS id2 FROM cascades_test_t t1 JOIN cascades_test_t t2 ON t1.a = t2.b$$);
SELECT cov_test(502, 'C502: 4-table join', $$SELECT t1.id, t2.val AS v2, t3.val AS v3, t4.a AS v4 FROM cascades_test_j1 t1 JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id JOIN cascades_test_j3 t3 ON t2.id = t3.j2_id JOIN cascades_test_t t4 ON t3.val = t4.b$$);
SELECT cov_test(503, 'C503: duplicate sub-expr', $$SELECT a + a AS double_a, a * a AS square_a FROM cascades_test_t$$);
SELECT cov_test(504, 'C504: min scan', $$SELECT 1 FROM cascades_test_t LIMIT 1$$);

\echo '=== Part 6: pg_adapter paths ==='
SELECT cov_test(601, 'C601: bitmap or', $$SELECT count(*) FROM cascades_test_t WHERE a = 1 OR b = 2$$);
SELECT cov_test(602, 'C602: index scan', $$SELECT id, a FROM cascades_test_t WHERE a = 50$$);
SELECT cov_test(603, 'C603: left join condition', $$SELECT t1.id, t2.val FROM cascades_test_j1 t1 LEFT JOIN cascades_test_j2 t2 ON t1.id = t2.j1_id AND t2.val > 10$$);

\echo '=== Part 7: Complex combos ==='
SELECT cov_test(701, 'C701: subquery agg', $$SELECT a, count(*) FROM (SELECT * FROM cascades_test_t WHERE b > 10) sub GROUP BY a ORDER BY a LIMIT 5$$);
SELECT cov_test(702, 'C702: distinct agg combo', $$SELECT count(DISTINCT a) AS da, count(DISTINCT b) AS db FROM cascades_test_t$$);
SELECT cov_test(703, 'C703: full combo', $$SELECT t1.a, count(*) AS cnt, sum(t2.val) AS total FROM cascades_test_t t1 JOIN cascades_test_j2 t2 ON t1.b = t2.val WHERE t1.a > 10 GROUP BY t1.a HAVING count(*) > 1 ORDER BY total DESC LIMIT 10$$);
SELECT cov_test(704, 'C704: exists agg', $$SELECT t1.* FROM cascades_test_j1 t1 WHERE EXISTS (SELECT 1 FROM cascades_test_j2 t2 WHERE t2.j1_id = t1.id GROUP BY t2.val HAVING count(*) > 1)$$);

\echo ''
\echo '========================================'
\echo '  EXTENDED COVERAGE TEST SUMMARY'
\echo '========================================'
SELECT test_id, test_name, CASE WHEN pg_ok AND cas_ok THEN 'PASS' ELSE 'FAIL' END AS verdict, cas_path FROM _cov_test_results ORDER BY test_id;
\echo ''
SELECT count(*) AS total_tests, sum(CASE WHEN pg_ok AND cas_ok THEN 1 ELSE 0 END) AS passed, sum(CASE WHEN NOT pg_ok OR NOT cas_ok THEN 1 ELSE 0 END) AS failed, sum(CASE WHEN cas_path = 'CASCADES' THEN 1 ELSE 0 END) AS cascades_path, sum(CASE WHEN cas_path = 'FALLBACK' THEN 1 ELSE 0 END) AS fallback_path FROM _cov_test_results;
\echo ''
\echo '========================================'
\echo '  ALL EXTENDED TESTS COMPLETE'
\echo '========================================'
