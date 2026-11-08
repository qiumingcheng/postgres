\set ECHO none
SET client_min_messages=warning;
SET jit=off;
CREATE SCHEMA ggc_auto_stats;
SET search_path=ggc_auto_stats,public;
CREATE TABLE v(id bigint PRIMARY KEY);
CREATE TABLE e(id bigint PRIMARY KEY,src bigint NOT NULL,dst bigint NOT NULL);
INSERT INTO v SELECT generate_series(1,5);
INSERT INTO e SELECT (s-1)*4+j,s,s+1 FROM generate_series(1,4) s,generate_series(1,4) j;
CREATE INDEX e_out ON e(src);
CREATE PROPERTY GRAPH g VERTEX TABLES(v KEY(id)) EDGE TABLES(e KEY(id)
 SOURCE KEY(src) REFERENCES v(id) DESTINATION KEY(dst) REFERENCES v(id));
ANALYZE v; ANALYZE e;
CREATE FUNCTION pg_temp.stat_graph(q text) RETURNS jsonb LANGUAGE plpgsql AS $$
DECLARE p jsonb; n jsonb;
BEGIN
 EXECUTE 'EXPLAIN(FORMAT JSON) '||q INTO p;
 WITH RECURSIVE plans(j) AS (SELECT p->0->'Plan' UNION ALL
 SELECT c FROM plans CROSS JOIN LATERAL jsonb_array_elements(j->'Plans') c)
 SELECT j INTO STRICT n FROM plans WHERE j->>'Custom Plan Provider'='GraphExpandFixed';
 RETURN n;
END $$;
DO $$
DECLARE
 q text=$query$SELECT DISTINCT endpoint FROM GRAPH_TABLE(g MATCH (a IS v WHERE a.id=1::bigint)
 -[x1 IS e]->(b IS v)-[x2 IS e]->(c IS v)-[x3 IS e]->(d IS v)-[x4 IS e]->(z IS v)
 COLUMNS(z.id AS endpoint)) gt$query$;
 l jsonb; p jsonb; lo oid; po oid; collected jsonb; n jsonb; k jsonb;
 options jsonb='{"seed":1,"seed_target":1,"hops":4,"predicate_attnum":0,"predicate_min":0,
 "predicate_max":0,"predicate_clause_id":0,"max_scan_pages":1000,"max_examined_entries":10000,
 "max_bindings":16,"max_bindings_per_seed":4,"max_entries_per_seed":64,
 "max_memory_bytes":1048576,"max_payload_bytes":1048576,"max_profiles":64,
 "max_parent_batch":8,"deadline_ms":10000,"lock_timeout_ms":1000}';
BEGIN
 l:=test_graph_pgq_statistics_scope(q)::jsonb;
 p:=l||jsonb_build_object('scope_kind',2,'index_relid','e_out'::regclass::oid::bigint,'build_generation',1);
 lo:=pg_graph_statistic_create(l,'{"anchor_condition":{"version":1,"key":1}}');
 po:=pg_graph_statistic_create(p,'{"anchor_condition":{"version":1,"key":1}}');
 collected:=pg_graph_statistic_collect_path(lo,po,options);
 IF collected->>'status'<>'READY' OR (collected->>'complete_depth')::int<>4 THEN
   RAISE EXCEPTION 'endpoint collection expanded bag or lost a stage: %',collected; END IF;
 IF EXISTS(SELECT FROM jsonb_array_elements(collected->'stages') s
           WHERE (s->>'F')::int<>1 OR (s->>'P')::int<>1 OR (s->>'E')::int<>4) THEN
   RAISE EXCEPTION 'incorrect endpoint population: %',collected; END IF;
 IF (SELECT count(*) FROM jsonb_array_elements(pg_graph_statistic_read(lo,l)->'profiles') s
     WHERE (s->>'metric_kind')::int=210 AND (s->'estimate'->>'expected')::int=1)<>4 THEN
   RAISE EXCEPTION 'missing distinct-next endpoint profiles'; END IF;
 n:=pg_temp.stat_graph('/*+ GraphRoute(btree) */ '||q);
 IF n->>'Graph Statistics' NOT LIKE 'logical=READY%physical=READY%' OR (n->>'Plan Rows')::int<>1 THEN
   RAISE EXCEPTION 'PGQ did not consume conditional set profile: %',n; END IF;
 FOREACH k IN ARRAY ARRAY[
   to_jsonb(replace(q,'a.id=1::bigint','a.id=2::bigint')),
   to_jsonb(replace(q,'a.id=1::bigint','a.id<=2::bigint')),
   to_jsonb(replace(q,'SELECT DISTINCT','SELECT'))] LOOP
   n:=pg_temp.stat_graph('/*+ GraphRoute(btree) */ '||(k#>>'{}'));
   IF n->>'Graph Statistics' NOT LIKE 'logical=MISSING%physical=MISSING%' THEN
     RAISE EXCEPTION 'singleton-set population leaked into another query: %',n; END IF;
 END LOOP;
 EXECUTE 'PREPARE stats_cached AS /*+ GraphRoute(btree) */ '||q;
 n:=pg_temp.stat_graph('EXECUTE stats_cached');
 IF n->>'Graph Statistics' NOT LIKE 'logical=READY%' THEN RAISE EXCEPTION 'cached statistics missing'; END IF;
 collected:=pg_graph_statistic_collect_path(lo,po,options||'{"max_entries_per_seed":1}');
 IF collected->>'status'<>'PARTIAL' THEN RAISE EXCEPTION 'truncation lost'; END IF;
 n:=pg_temp.stat_graph('EXECUTE stats_cached');
 IF n->>'Graph Statistics' NOT LIKE 'logical=PARTIAL%' THEN RAISE EXCEPTION 'cached generation survived partial publish: %',n; END IF;
 EXECUTE 'DEALLOCATE stats_cached';
 PERFORM pg_graph_statistic_collect_path(lo,po,options);
 EXECUTE 'SET plan_cache_mode=force_generic_plan';
 EXECUTE 'PREPARE stats_generic(bigint) AS /*+ GraphRoute(btree) */ '||replace(q,'1::bigint','$1');
 n:=pg_temp.stat_graph('EXECUTE stats_generic(1)');
 IF n->>'Graph Statistics' NOT LIKE 'logical=MISSING%' THEN RAISE EXCEPTION 'generic used singleton profile'; END IF;
 EXECUTE 'DEALLOCATE stats_generic'; EXECUTE 'RESET plan_cache_mode';
 FOREACH k IN ARRAY ARRAY['{"predicate_attnum":3}'::jsonb,'{"predicate_clause_id":1}','{"predicate_min":-1}'] LOOP
   BEGIN
     PERFORM pg_graph_statistic_collect_path(lo,po,options||k);
     RAISE EXCEPTION 'partial predicate accepted';
   EXCEPTION WHEN invalid_parameter_value THEN NULL;
   END;
 END LOOP;
 EXECUTE 'REINDEX INDEX e_out';
 n:=pg_temp.stat_graph('/*+ GraphRoute(btree) */ '||q);
 IF n->>'Graph Statistics' NOT LIKE '%physical=STALE%' THEN RAISE EXCEPTION 'reindex did not stale profile: %',n; END IF;
END $$;
-- Same conditional population must price native joins before DP pruning.
-- Native search is free to delay the singleton anchor/unique vertex checks.
INSERT INTO v SELECT generate_series(6,102000);
CREATE TABLE t(id bigint PRIMARY KEY,src bigint NOT NULL,dst bigint NOT NULL);
INSERT INTO t SELECT (s-1)*3+j,s,(s-1)*3+j+1
 FROM generate_series(1,40) s,generate_series(1,3) j;
INSERT INTO t SELECT 1000+i,1000+i,1001+i FROM generate_series(1,99880) i;
CREATE INDEX t_out ON t(src);
ALTER TABLE t ALTER COLUMN src SET STATISTICS 1000;
CREATE PROPERTY GRAPH gt VERTEX TABLES(v KEY(id)) EDGE TABLES(t KEY(id)
 SOURCE KEY(src) REFERENCES v(id) DESTINATION KEY(dst) REFERENCES v(id));
VACUUM(FREEZE,ANALYZE) v; VACUUM(FREEZE,ANALYZE) t;
CREATE FUNCTION pg_temp.stat_plan(q text) RETURNS jsonb LANGUAGE plpgsql AS $$
DECLARE p jsonb;
BEGIN EXECUTE 'EXPLAIN(FORMAT JSON) '||q INTO p; RETURN p->0->'Plan'; END $$;
DO $$
DECLARE
 q text=$query$SELECT DISTINCT endpoint FROM GRAPH_TABLE(gt MATCH (a IS v WHERE a.id=1::bigint)
 -[x1 IS t]->(b IS v)-[x2 IS t]->(c IS v)-[x3 IS t]->(d IS v)-[x4 IS t]->(z IS v)
 COLUMNS(z.id AS endpoint)) r$query$;
 l jsonb; p jsonb; lo oid; po oid; n jsonb; base_rows int; cnt int;
 opts jsonb='{"seed":1,"seed_target":1,"hops":4,"predicate_attnum":0,"predicate_min":0,
 "predicate_max":0,"predicate_clause_id":0,"max_scan_pages":10000,"max_examined_entries":500000,
 "max_bindings":10000,"max_bindings_per_seed":5000,"max_entries_per_seed":5000,
 "max_memory_bytes":67108864,"max_payload_bytes":1048576,"max_profiles":64,
 "max_parent_batch":8,"deadline_ms":10000,"lock_timeout_ms":1000}';
BEGIN
 EXECUTE 'PREPARE native_stats AS /*+ GraphRoute(relational) */ '||q;
 base_rows:=(pg_temp.stat_plan('EXECUTE native_stats')->>'Plan Rows')::int;
 l:=test_graph_pgq_statistics_scope(q)::jsonb;
 p:=l||jsonb_build_object('scope_kind',2,'index_relid','t_out'::regclass::oid::bigint,'build_generation',1);
 lo:=pg_graph_statistic_create(l,'{"anchor_condition":{"version":1,"key":1}}');
 po:=pg_graph_statistic_create(p,'{"anchor_condition":{"version":1,"key":1}}');
 IF pg_graph_statistic_collect_path(lo,po,opts)->>'status'<>'READY' THEN RAISE EXCEPTION 'tree collection failed'; END IF;
 n:=pg_temp.stat_plan('EXECUTE native_stats');
 IF (n->>'Plan Rows')::int<>81 THEN RAISE EXCEPTION 'native prefix population not consumed: %',n; END IF;
 n:=pg_temp.stat_plan('/*+ GraphRoute(relational) */ '||replace(q,'SELECT DISTINCT','SELECT'));
 IF (n->>'Plan Rows')::int<>81 THEN RAISE EXCEPTION 'nonconvergent bag estimate lost: %',n; END IF;
 EXECUTE 'SELECT count(*) FROM ('||q||') x' INTO cnt;
 IF cnt<>81 THEN RAISE EXCEPTION 'tree result changed'; END IF;
 PERFORM set_config('enable_graph_btree','on',true);
 -- This minimal table has different width/correlation from the performance
 -- fixture. Assert shared cardinality here, not a hardwired access choice;
 -- the frozen 100k-edge benchmark separately checks AUTO route and regret.
 n:=pg_temp.stat_plan(q);
 IF (n->>'Plan Rows')::int<>81 THEN RAISE EXCEPTION 'AUTO tree cardinality incorrect: %',n; END IF;
 PERFORM set_config('enable_graph_btree','off',true);
 -- Estimates must not become semantic proofs or change DISTINCT/bag output.
 n:=pg_temp.stat_plan('/*+ GraphRoute(relational) */ '||replace(q,'a.id=1::bigint','a.id=2::bigint'));
 IF (n->>'Plan Rows')::int=81 THEN RAISE EXCEPTION 'wrong anchor borrowed profile'; END IF;
 PERFORM pg_graph_statistic_collect_path(lo,po,opts||'{"max_entries_per_seed":1}');
 n:=pg_temp.stat_plan('EXECUTE native_stats');
 IF (n->>'Plan Rows')::int<>base_rows THEN RAISE EXCEPTION 'partial profile did not invalidate native estimate'; END IF;
 EXECUTE 'DEALLOCATE native_stats';
 -- A convergent set is NOT a bag. The old 4-parallel-edge fixture has
 -- 256 paths; only an independently collected binding profile can price it.
 q:=replace(replace(q,'gt MATCH','g MATCH'),' IS t',' IS e');
 l:=test_graph_pgq_statistics_scope(q)::jsonb;
 l:=l||'{"result_mode":1}';
 p:=l||jsonb_build_object('scope_kind',2,'index_relid','e_out'::regclass::oid::bigint,'build_generation',1);
 lo:=pg_graph_statistic_create(l,'{"anchor_condition":{"version":1,"key":1}}');
 po:=pg_graph_statistic_create(p,'{"anchor_condition":{"version":1,"key":1}}');
 IF pg_graph_statistic_collect_path(lo,po,opts)->>'status'<>'READY' THEN RAISE EXCEPTION 'bag collection failed'; END IF;
 n:=pg_temp.stat_plan('/*+ GraphRoute(relational) */ '||replace(q,'SELECT DISTINCT','SELECT'));
 IF (n->>'Plan Rows')::int<>256 THEN RAISE EXCEPTION 'native bag population not consumed: %',n; END IF;
END $$;
RESET search_path;
DROP SCHEMA ggc_auto_stats CASCADE;
