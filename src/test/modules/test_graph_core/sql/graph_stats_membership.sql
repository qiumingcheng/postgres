\set ON_ERROR_STOP 1
\set ECHO none
SET client_min_messages=warning;
SET jit=off;
SET stats_fetch_consistency=none;
SET plan_cache_mode=force_generic_plan;
CREATE ROLE ggc_stats_full;
CREATE ROLE ggc_stats_member;
GRANT ggc_stats_full TO ggc_stats_member;
CREATE SCHEMA ggc_stats_membership;
SET search_path=ggc_stats_membership,public;
GRANT USAGE ON SCHEMA ggc_stats_membership TO ggc_stats_member;
CREATE TABLE v(id bigint PRIMARY KEY,secret text);
CREATE TABLE e(id bigint PRIMARY KEY,src bigint NOT NULL,dst bigint NOT NULL,secret text);
INSERT INTO v SELECT i,'hidden' FROM generate_series(1,5)i;
INSERT INTO e SELECT (s-1)*4+j,s,s+1,'hidden'
 FROM generate_series(1,4)s,generate_series(1,4)j;
CREATE INDEX e_out ON e(src);
CREATE PROPERTY GRAPH g VERTEX TABLES(v KEY(id) PROPERTIES(id))
 EDGE TABLES(e KEY(id) SOURCE KEY(src) REFERENCES v(id)
 DESTINATION KEY(dst) REFERENCES v(id) PROPERTIES(id,src,dst));
ANALYZE v;
ANALYZE e;
GRANT SELECT,MAINTAIN ON v,e TO ggc_stats_full;
GRANT SELECT ON g TO ggc_stats_member;
-- These grants remain after membership is revoked. Query execution is still
-- authorized, but the role no longer has the full visibility required to use
-- global statistics. The unreferenced secret columns are never granted here.
GRANT SELECT(id) ON v TO ggc_stats_member;
GRANT SELECT(id,src,dst) ON e TO ggc_stats_member;
CREATE TABLE config(q text,l jsonb,p jsonb,options jsonb,lo oid,po oid,
 ordinary_mark bigint,native_mark bigint);
INSERT INTO config(q,options) VALUES(
 $query$SELECT DISTINCT endpoint FROM GRAPH_TABLE(g MATCH (a IS v WHERE a.id=1::bigint)
 -[x1 IS e]->(b IS v)-[x2 IS e]->(c IS v)-[x3 IS e]->(d IS v)-[x4 IS e]->(z IS v)
 COLUMNS(z.id AS endpoint)) gt$query$,
 '{"seed":1,"seed_target":1,"hops":4,"predicate_attnum":0,"predicate_min":0,
 "predicate_max":0,"predicate_clause_id":0,"max_scan_pages":1000,"max_examined_entries":10000,
 "max_bindings":16,"max_bindings_per_seed":4,"max_entries_per_seed":64,
 "max_memory_bytes":1048576,"max_payload_bytes":1048576,"max_profiles":64,
 "max_parent_batch":8,"deadline_ms":10000,"lock_timeout_ms":1000}');
UPDATE config SET l=test_graph_pgq_statistics_scope(q)::jsonb||jsonb_build_object(
 'effective_role',(SELECT oid::bigint FROM pg_roles WHERE rolname='ggc_stats_member'));
UPDATE config SET p=l||jsonb_build_object('scope_kind',2,
 'index_relid','e_out'::regclass::oid::bigint,'build_generation',1);
CREATE TABLE observations(phase text,statistics text,ordinary_mark bigint,native_mark bigint);
CREATE SEQUENCE ordinary_plan_count;
CREATE SEQUENCE native_plan_count;
GRANT SELECT,UPDATE ON config TO ggc_stats_member;
GRANT SELECT,INSERT ON observations TO ggc_stats_member;
GRANT USAGE,SELECT ON ordinary_plan_count,native_plan_count TO ggc_stats_member;

CREATE FUNCTION pg_temp.membership_assert(ok boolean,message text) RETURNS void
LANGUAGE plpgsql AS $$BEGIN
 IF ok IS DISTINCT FROM true THEN RAISE EXCEPTION 'stats membership: %',message; END IF;
END$$;
CREATE FUNCTION pg_temp.membership_plan(statement text) RETURNS jsonb
LANGUAGE plpgsql AS $$DECLARE p jsonb; BEGIN
 EXECUTE 'EXPLAIN(FORMAT JSON) '||statement INTO p; RETURN p;
END$$;
CREATE FUNCTION pg_temp.membership_graph(statement text DEFAULT 'EXECUTE membership_graph')
RETURNS jsonb LANGUAGE sql AS $$
 SELECT jsonb_path_query_first(pg_temp.membership_plan(statement),
 '$.** ? (@."Custom Plan Provider" == "GraphExpandFixed")')
$$;
-- Test-only instruments. The deliberately false IMMUTABLE declaration makes
-- each nextval run at planning time, so a changed value detects replanning.
CREATE FUNCTION pg_temp.membership_ordinary_tick() RETURNS bigint
LANGUAGE plpgsql IMMUTABLE AS $$BEGIN
 RETURN nextval('ggc_stats_membership.ordinary_plan_count'); END$$;
CREATE FUNCTION pg_temp.membership_native_tick() RETURNS bigint
LANGUAGE plpgsql IMMUTABLE AS $$BEGIN
 RETURN nextval('ggc_stats_membership.native_plan_count'); END$$;
CREATE FUNCTION pg_temp.membership_check(phase text,ready boolean) RETURNS void
LANGUAGE plpgsql AS $$
DECLARE n jsonb; fresh jsonb; ordinary bigint; native_row record;
 old_ordinary bigint; old_native bigint; endpoint bigint; statement text; expected text;
BEGIN
 expected:=CASE WHEN ready THEN 'READY' ELSE 'STALE' END;
 n:=pg_temp.membership_graph();
 PERFORM pg_temp.membership_assert(n->>'Graph Statistics' LIKE
  'logical='||expected||'%physical='||expected||'%',phase||': cached graph state');
 SELECT q,ordinary_mark,native_mark INTO statement,old_ordinary,old_native FROM config;
 fresh:=pg_temp.membership_graph('/*+ GraphRoute(btree) */ '||statement);
 PERFORM pg_temp.membership_assert(fresh->>'Graph Statistics' LIKE
  'logical='||expected||'%physical='||expected||'%',phase||': fresh graph state');
 EXECUTE 'EXECUTE membership_graph' INTO STRICT endpoint;
 PERFORM pg_temp.membership_assert(endpoint=5,phase||': graph results');
 EXECUTE 'EXECUTE membership_ordinary' INTO STRICT ordinary;
 EXECUTE 'EXECUTE membership_native' INTO STRICT native_row;
 PERFORM pg_temp.membership_assert(native_row.endpoint=5,phase||': native results');
 PERFORM pg_temp.membership_assert(old_ordinary IS NULL OR ordinary=old_ordinary,
  phase||': ordinary PREPARE must not be invalidated');
 PERFORM pg_temp.membership_assert(old_native IS NULL OR native_row.marker>old_native,
  phase||': logical-only native statistics plan must be invalidated');
 PERFORM pg_temp.membership_assert(NOT jsonb_path_exists(
  pg_temp.membership_plan('EXECUTE membership_native'),
  '$.** ? (@."Custom Plan Provider" == "GraphExpandFixed")'),phase||': native route');
 PERFORM pg_temp.membership_assert(has_table_privilege('e','SELECT')=ready AND
  has_column_privilege('e','id','SELECT') AND
  has_column_privilege('e','secret','SELECT')=ready,phase||': effective ACLs');
 IF NOT ready THEN
  BEGIN
   PERFORM pg_graph_statistic_collect_path(lo,po,options) FROM config;
   RAISE EXCEPTION 'collection after privilege revocation unexpectedly succeeded';
  EXCEPTION WHEN insufficient_privilege THEN NULL;
  END;
 END IF;
 UPDATE config SET ordinary_mark=ordinary,native_mark=native_row.marker;
 INSERT INTO observations VALUES(phase,n->>'Graph Statistics',ordinary,native_row.marker);
END$$;

SET ROLE ggc_stats_member;
DO $$
DECLARE logical_oid oid; physical_oid oid; statement text;
BEGIN
 SELECT pg_graph_statistic_create(l,'{"anchor_condition":{"version":1,"key":1}}'),
  pg_graph_statistic_create(p,'{"anchor_condition":{"version":1,"key":1}}')
 INTO logical_oid,physical_oid FROM config;
 UPDATE config SET lo=logical_oid,po=physical_oid;
 PERFORM pg_temp.membership_assert(
  (SELECT pg_graph_statistic_collect_path(lo,po,options)->>'status'='READY' FROM config),
  'initial own-role collection via inherited privileges');
 SELECT q INTO statement FROM config;
 EXECUTE 'PREPARE membership_graph AS /*+ GraphRoute(btree) */ '||statement;
 EXECUTE 'PREPARE membership_ordinary AS SELECT pg_temp.membership_ordinary_tick()';
 -- The subquery uses the native logical-only graph estimator. Its outer
 -- constant projection detects replanning without changing graph semantics.
 EXECUTE 'PREPARE membership_native AS /*+ GraphRoute(relational) */ SELECT r.endpoint,'||
  'pg_temp.membership_native_tick() AS marker FROM ('||statement||') r';
END$$;
SELECT pg_temp.membership_check('initial',true);
RESET ROLE;
REVOKE ggc_stats_full FROM ggc_stats_member;
SET ROLE ggc_stats_member;
SELECT pg_temp.membership_check('membership_revoked',false);
RESET ROLE;
GRANT ggc_stats_full TO ggc_stats_member WITH INHERIT TRUE;
SET ROLE ggc_stats_member;
SELECT pg_temp.membership_check('membership_restored',true);
RESET ROLE;
GRANT ggc_stats_full TO ggc_stats_member WITH INHERIT FALSE;
SET ROLE ggc_stats_member;
SELECT pg_temp.membership_check('membership_inherit_false',false);
RESET ROLE;
GRANT ggc_stats_full TO ggc_stats_member WITH INHERIT TRUE;
SET ROLE ggc_stats_member;
SELECT pg_temp.membership_check('membership_inherit_true',true);
RESET ROLE;
-- In PG19, role-level INHERIT supplies the default for future grants; existing
-- membership has an explicit inherit_option. AUTHOID changes still invalidate
-- participating plans, without pretending this DDL revoked existing access.
ALTER ROLE ggc_stats_member NOINHERIT;
SET ROLE ggc_stats_member;
SELECT pg_temp.membership_check('role_noinherit_existing_grant',true);
RESET ROLE;
ALTER ROLE ggc_stats_member INHERIT;
SET ROLE ggc_stats_member;
SELECT pg_temp.membership_check('role_inherit_existing_grant',true);
SELECT pg_temp.membership_assert((SELECT custom_plans=0 AND generic_plans>0
 FROM pg_prepared_statements WHERE name='membership_graph'),'forced generic graph plan');
SELECT pg_temp.membership_assert((SELECT custom_plans=0 AND generic_plans>0
 FROM pg_prepared_statements WHERE name='membership_native'),'forced generic native plan');
SELECT pg_temp.membership_assert((SELECT last_value FROM ordinary_plan_count)=
 (SELECT ordinary_mark FROM config),'ordinary plan folded only once');
SELECT * FROM observations ORDER BY ctid;
DEALLOCATE membership_graph;
DEALLOCATE membership_native;
DEALLOCATE membership_ordinary;
RESET ROLE;
DROP SCHEMA ggc_stats_membership CASCADE;
DROP ROLE ggc_stats_member;
DROP ROLE ggc_stats_full;
RESET search_path;
RESET plan_cache_mode;
RESET stats_fetch_consistency;
SELECT 'graph_stats_membership_ok';
