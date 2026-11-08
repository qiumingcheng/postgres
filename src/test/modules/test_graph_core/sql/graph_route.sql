\set ECHO none
SET client_min_messages=warning;
LOAD 'test_graph_core';
DO $$
BEGIN
 IF current_setting('enable_graph_age_adjacency') <> 'off' OR
    (SELECT boot_val FROM pg_settings WHERE name='enable_graph_age_adjacency') <> 'off' THEN
   RAISE EXCEPTION 'AGE adjacency must be explicitly enabled';
 END IF;
 PERFORM set_config('enable_graph_age_adjacency','on',true);
 IF current_setting('enable_graph_age_adjacency') <> 'on' THEN
   RAISE EXCEPTION 'AGE adjacency GUC did not change';
 END IF;
END $$;
DO $$ BEGIN
 IF current_setting('enable_graph_age_adjacency') <> 'off' THEN
   RAISE EXCEPTION 'local AGE adjacency setting leaked out of its transaction';
 END IF;
END $$;
CREATE SCHEMA ggc_route;
SET search_path=ggc_route,public;
CREATE TABLE v(id bigint PRIMARY KEY);
CREATE TABLE e(id bigint PRIMARY KEY,src bigint NOT NULL,dst bigint NOT NULL,payload text);
INSERT INTO v VALUES(1),(2),(3),(4);
INSERT INTO e VALUES
 (11,1,2,'keep'),(12,1,2,'drop'),(13,2,3,'keep'),(14,3,1,'cycle'),
 (15,1,99,'missing destination'),(16,99,2,'missing source'),(17,2,2,'loop');
CREATE INDEX e_btree ON e(src);
CREATE INDEX e_adj ON e USING graph_adj(src,dst);
CREATE PROPERTY GRAPH g VERTEX TABLES(v KEY(id)) EDGE TABLES(e KEY(id)
 SOURCE KEY(src) REFERENCES v(id) DESTINATION KEY(dst) REFERENCES v(id));

CREATE FUNCTION pg_temp.route_check(q text, mode text, expected jsonb,
                                   required_join text DEFAULT NULL) RETURNS void
LANGUAGE plpgsql AS $$
DECLARE p jsonb; r record; got jsonb='[]'; forced text;
        before_flags text[]; after_flags text[]; graph_count int; adj_scans int; join_count int;
BEGIN
 before_flags := ARRAY[current_setting('enable_nestloop'),current_setting('enable_hashjoin'),
                       current_setting('enable_mergejoin'),current_setting('enable_memoize')];
 forced := format('/*+ GraphRoute(%s) */ ',mode)||q;
 EXECUTE 'EXPLAIN (FORMAT JSON) '||forced INTO p;
 IF p->0->>'Graph Route'<>mode OR p->0->>'Graph Route Forced'<>'true' THEN
   RAISE EXCEPTION 'missing route marker: %',p;
 END IF;
 WITH RECURSIVE plans(n) AS (
   SELECT p->0->'Plan' UNION ALL
   SELECT x FROM plans CROSS JOIN LATERAL jsonb_array_elements(n->'Plans') x
 ) SELECT count(*) FILTER(WHERE n->>'Custom Plan Provider' IN ('GraphExpandFixed','GraphExpandNative')),
          count(*) FILTER(WHERE n->>'Index Name'='e_adj'),
          count(*) FILTER(WHERE n->>'Node Type'=required_join)
   INTO graph_count,adj_scans,join_count FROM plans;
 IF required_join IS NOT NULL AND join_count=0 THEN
   RAISE EXCEPTION 'missing join method %, plan %',required_join,p;
 END IF;
 IF adj_scans<>0 THEN RAISE EXCEPTION 'graph_adj scan escaped route filter: %',p; END IF;
 IF mode='relational' THEN
   IF graph_count<>0 OR p->0->>'Graph Provider' IS DISTINCT FROM 'relational' THEN
     RAISE EXCEPTION 'wrong relational route: %',p;
   END IF;
 ELSE
   IF graph_count<>1 OR p->0->>'Graph Index'<>
      (CASE mode WHEN 'btree' THEN 'e_btree' ELSE 'e_adj' END) THEN
     RAISE EXCEPTION 'wrong graph route: %',p;
   END IF;
 END IF;
 FOR r IN EXECUTE forced LOOP got:=got||jsonb_build_array(to_jsonb(r)); END LOOP;
 IF got<>expected THEN RAISE EXCEPTION 'wrong graph bag: %, expected %',got,expected; END IF;
 after_flags := ARRAY[current_setting('enable_nestloop'),current_setting('enable_hashjoin'),
                      current_setting('enable_mergejoin'),current_setting('enable_memoize')];
 IF before_flags<>after_flags THEN RAISE EXCEPTION 'GUC leak'; END IF;
END $$;

CREATE FUNCTION pg_temp.route_reject(q text, state text) RETURNS void
LANGUAGE plpgsql AS $$
DECLARE actual text;
BEGIN
 BEGIN
   EXECUTE 'EXPLAIN '||q;
 EXCEPTION WHEN OTHERS THEN
   GET STACKED DIAGNOSTICS actual=RETURNED_SQLSTATE;
 END;
 IF actual IS DISTINCT FROM state THEN
   RAISE EXCEPTION 'expected SQLSTATE %, got %',state,actual;
 END IF;
END $$;

-- No-hint DML must stay ordinary with AUTO enabled or disabled.  Finding a
-- potential opener is not sufficient to activate a hint; quoted/commented
-- openers still require the full lexer.  Explicit DML hints still reject.
DO $test$
DECLARE flag text; affected bigint; value text;
BEGIN
 CREATE TEMP TABLE hint_dml(id int PRIMARY KEY, payload text) ON COMMIT DROP;
 FOREACH flag IN ARRAY ARRAY['off','on'] LOOP
   PERFORM set_config('enable_graph_btree',flag,true);
   EXECUTE 'INSERT INTO hint_dml VALUES (1,''plain'')';
   EXECUTE 'UPDATE hint_dml SET payload=''changed'' WHERE id=1';
   GET DIAGNOSTICS affected=ROW_COUNT;
   IF affected<>1 THEN RAISE EXCEPTION 'ordinary UPDATE changed % rows',affected; END IF;
   EXECUTE 'SELECT payload FROM hint_dml WHERE id=1' INTO value;
   IF value<>'changed' THEN RAISE EXCEPTION 'ordinary UPDATE failed'; END IF;
   EXECUTE 'DELETE FROM hint_dml WHERE id=1';
   GET DIAGNOSTICS affected=ROW_COUNT;
   IF affected<>1 THEN RAISE EXCEPTION 'ordinary DELETE changed % rows',affected; END IF;
   EXECUTE 'INSERT INTO hint_dml VALUES (2,$q$/*+ GraphRoute(btree) */$q$)';
   EXECUTE '/* outer /*+ GraphRoute(adj) */ */ DELETE FROM hint_dml WHERE id=2';
   EXECUTE E'-- /*+ GraphRoute(btree) */\nSELECT 1';
   EXECUTE 'SELECT 1 AS "/*+ GraphRoute(adj) */"';
   PERFORM pg_temp.route_reject('/*+ GraphRoute(btree) */ DELETE FROM hint_dml','0A000');
   PERFORM pg_temp.route_reject('/*+ GraphRoute(adj) */ UPDATE hint_dml SET payload=''x''','0A000');
   PERFORM pg_temp.route_reject('/*+ GraphRoute(relational) */ INSERT INTO hint_dml VALUES (3,''x'')','0A000');
   PERFORM pg_temp.route_reject('/*+ GraphRoute(unknown) */ DELETE FROM hint_dml','42601');
 END LOOP;
END $test$;

-- Check ordinary execution, not only EXPLAIN (which adds a leading token).
DO $$
DECLARE q text; expected text; actual text;
BEGIN
 FOR q,expected IN VALUES
   ('/*+ GraphRoute(btree) */ SELECT 1','0A000'),
   ('/*+ GraphRoute(unknown) */ SELECT 1','42601'),
   ('/*+ GraphRoute(nestloop) */ SELECT 1','42601'),
   ('SELECT /*+ GraphRoute(btree) */ 1','0A000'),
   ('/*+ GraphRoute(btree) */ /*+ GraphRoute(adj) */ SELECT 1','42601')
 LOOP
   actual:=NULL;
   BEGIN EXECUTE q;
   EXCEPTION WHEN OTHERS THEN GET STACKED DIAGNOSTICS actual=RETURNED_SQLSTATE;
   END;
   IF actual IS DISTINCT FROM expected THEN
     RAISE EXCEPTION 'direct hint missed: %, got % expected %',q,actual,expected;
   END IF;
 END LOOP;
 -- Semicolons inside strings/comments do not start another statement.
 EXECUTE 'SELECT ''/*+ GraphRoute(btree) */;''; SELECT 1';
 EXECUTE '/* nested /*+ GraphRoute(btree) */ ; */ SELECT 1; SELECT 2';
 -- The second statement must not inherit the valid first hint.
 EXECUTE '/*+ GraphRoute(relational) */ SELECT endpoint FROM GRAPH_TABLE(g '
   'MATCH (a IS v WHERE a.id=1)-[x IS e]->(b IS v) '
   'COLUMNS(b.id AS endpoint)); SELECT 1';
END $$;

DO $$
DECLARE q text; mode text; predicate text; correct jsonb; got jsonb; r record;
BEGIN
 -- Keep the SQL body identical across all three routes and retain every path.
 q:=$q$SELECT * FROM GRAPH_TABLE(g
 MATCH (a IS v WHERE a.id=1)-[x IS e]->(b IS v)-[y IS e]->(c IS v)
 COLUMNS(x.id AS e1,b.id AS middle,y.id AS e2,c.id AS endpoint)) ORDER BY e1,e2$q$;
 correct:='[{"e1":11,"middle":2,"e2":13,"endpoint":3},
            {"e1":11,"middle":2,"e2":17,"endpoint":2},
            {"e1":12,"middle":2,"e2":13,"endpoint":3},
            {"e1":12,"middle":2,"e2":17,"endpoint":2}]';
 FOREACH mode IN ARRAY ARRAY['relational','btree','adj'] LOOP
   FOREACH predicate IN ARRAY ARRAY['a.id=1','a.id=1::bigint','1::smallint=a.id'] LOOP
     PERFORM pg_temp.route_check(replace(q,'a.id=1',predicate),mode,correct);
   END LOOP;
   PERFORM pg_temp.route_check(replace(q,'a.id=1','a.id=99'),mode,'[]');
 END LOOP;
 -- Relational chooses the execution family, not a particular join algorithm.
 -- Observe real join nodes, not merely GUCs restored after planning.
 FOR r IN SELECT * FROM (VALUES
     ('Nested Loop','on','off','off'),
     ('Hash Join','off','on','off'),
     ('Merge Join','off','off','on')) AS f(method,nest,hash,merge)
 LOOP
   PERFORM set_config('enable_nestloop',r.nest,true);
   PERFORM set_config('enable_hashjoin',r.hash,true);
   PERFORM set_config('enable_mergejoin',r.merge,true);
   PERFORM pg_temp.route_check(q,'relational',correct,r.method);
 END LOOP;
 RESET enable_hashjoin; RESET enable_mergejoin; RESET enable_nestloop;
 -- All-off caller settings must also be restored after a successful hint.
 SET LOCAL enable_hashjoin=off; SET LOCAL enable_mergejoin=off; SET LOCAL enable_nestloop=off;
 PERFORM pg_temp.route_check(q,'relational',correct);
 RESET enable_hashjoin; RESET enable_mergejoin; RESET enable_nestloop;
 -- The B-tree route now owns local filters; adj admission is unchanged.
 -- Assert the exact remaining paths rather than just removing old rejects.
 FOREACH mode IN ARRAY ARRAY['relational','btree'] LOOP
   PERFORM pg_temp.route_check(replace(q,'[x IS e]',
       '[x IS e WHERE x.payload=''keep'']'),mode,
       '[{"e1":11,"middle":2,"e2":13,"endpoint":3},
         {"e1":11,"middle":2,"e2":17,"endpoint":2}]');
   PERFORM pg_temp.route_check(replace(q,'(b IS v)',
       '(b IS v WHERE b.id=3)'),mode,'[]');
 END LOOP;
 PERFORM pg_temp.route_reject('/*+ GraphRoute(adj) */ '||
     replace(q,'[x IS e]','[x IS e WHERE x.payload=''keep'']'),'0A000');
 PERFORM pg_temp.route_reject('/*+ GraphRoute(adj) */ '||
     replace(q,'(b IS v)','(b IS v WHERE b.id=3)'),'0A000');
 ALTER TABLE e ENABLE ROW LEVEL SECURITY;
 PERFORM pg_temp.route_check(q,'btree',correct);
 PERFORM pg_temp.route_reject('/*+ GraphRoute(adj) */ '||q,'0A000');
 ALTER TABLE e DISABLE ROW LEVEL SECURITY;
 ALTER TABLE e ALTER COLUMN src DROP NOT NULL;
 PERFORM pg_temp.route_check(q,'btree',correct);
 PERFORM pg_temp.route_reject('/*+ GraphRoute(adj) */ '||q,'0A000');
 ALTER TABLE e ALTER COLUMN src SET NOT NULL;
 -- A cached graph plan must replan after index drop/recreate or REINDEX.
 FOREACH mode IN ARRAY ARRAY['btree','adj'] LOOP
   EXECUTE format('PREPARE route_cached AS /*+ GraphRoute(%s) */ ',mode)||q;
   EXECUTE 'EXECUTE route_cached';
   EXECUTE format('DROP INDEX e_%s',mode);
   IF mode='btree' THEN
     -- Dropping an access index selects native typed stages after replanning.
     EXECUTE 'EXECUTE route_cached';
     PERFORM pg_temp.route_check(q,'btree',correct);
   ELSE PERFORM pg_temp.route_reject('EXECUTE route_cached','0A000'); END IF;
   IF mode='btree' THEN CREATE INDEX e_btree ON e(src);
   ELSE CREATE INDEX e_adj ON e USING graph_adj(src,dst); END IF;
   EXECUTE format('REINDEX INDEX e_%s',mode);
   got:='[]';
   FOR r IN EXECUTE 'EXECUTE route_cached' LOOP
     got:=got||jsonb_build_array(to_jsonb(r));
   END LOOP;
   IF got<>correct THEN RAISE EXCEPTION 'cached route changed results'; END IF;
   EXECUTE 'DEALLOCATE route_cached';
 END LOOP;
 -- Visibility changes must still be checked by the actual graph runtime.
 DELETE FROM v WHERE id=2;
 FOREACH mode IN ARRAY ARRAY['relational','btree','adj'] LOOP
   PERFORM pg_temp.route_check(q,mode,'[]');
 END LOOP;
END $$;

INSERT INTO v VALUES(2);
DO $$
DECLARE q text=$q$SELECT DISTINCT endpoint FROM GRAPH_TABLE(g
 MATCH (a IS v WHERE a.id=1)-[x IS e]->(b IS v)-[y IS e]->(c IS v)
 COLUMNS(c.id AS endpoint)) ORDER BY endpoint$q$;
 mode text; p jsonb; g jsonb;
BEGIN
 FOREACH mode IN ARRAY ARRAY['btree','adj'] LOOP
   PERFORM pg_temp.route_check(q,mode,'[{"endpoint":2},{"endpoint":3}]');
   EXECUTE format('EXPLAIN (ANALYZE,TIMING OFF,FORMAT JSON) /*+ GraphRoute(%s) */ ',mode)||q INTO p;
   WITH RECURSIVE plans(n) AS (
     SELECT p->0->'Plan' UNION ALL
     SELECT x FROM plans CROSS JOIN LATERAL jsonb_array_elements(n->'Plans') x
   ) SELECT n INTO STRICT g FROM plans WHERE n->>'Custom Plan Provider'='GraphExpandFixed';
   IF g->>'Graph Prefix Representation' IS DISTINCT FROM 'exact-k WALK endpoint sets (unordered)'
      OR (g->>'Actual Rows')::numeric<>2 OR (g->>'Factorized Arcs')::bigint<>0
      OR g->>'Stage 1 Vertex Visibility'<>'probes=2 rejected=1 index_only=0 heap_fetches=1 scan_starts=1'
      OR g->>'Stage 2 Vertex Visibility'<>'probes=2 rejected=0 index_only=0 heap_fetches=2 scan_starts=0' THEN
     RAISE EXCEPTION 'endpoint sets did not retain vertex semantics: %',g;
   END IF;
   PERFORM pg_temp.route_check(replace(q,'DISTINCT ',''),mode,
      '[{"endpoint":2},{"endpoint":2},{"endpoint":3},{"endpoint":3}]');
   EXECUTE format('EXPLAIN (FORMAT JSON) /*+ GraphRoute(%s) */ ',mode)||replace(q,'DISTINCT ','') INTO p;
   IF p::text LIKE '%exact-k WALK endpoint sets%' THEN
     RAISE EXCEPTION 'bag query incorrectly deduplicated';
   END IF;
 END LOOP;
END $$;

-- Versioned plan metadata is consumed both by the post-planner route check
-- and by ExecutorStart.  Repeated stages use backward references; a generic
-- saved plan must retain the envelope and the exact terminal result set.
DO $$
DECLARE q text; mode text; r record; got jsonb;
BEGIN
 q := 'SELECT DISTINCT endpoint FROM GRAPH_TABLE(g MATCH (a IS v WHERE a.id=1)';
 FOR i IN 1..16 LOOP
   q := q || format('-[x%s IS e]->(v%s IS v)',i,i);
 END LOOP;
 q := q || ' COLUMNS(v16.id AS endpoint)) ORDER BY endpoint';
 PERFORM set_config('plan_cache_mode','force_generic_plan',true);
 FOREACH mode IN ARRAY ARRAY['btree','adj'] LOOP
   PERFORM pg_temp.route_check(q,mode,'[{"endpoint":1},{"endpoint":2},{"endpoint":3}]');
   EXECUTE format('PREPARE route_versioned AS /*+ GraphRoute(%s) */ ',mode)||q;
   FOR attempt IN 1..2 LOOP
     got := '[]';
     FOR r IN EXECUTE 'EXECUTE route_versioned' LOOP
       got := got || jsonb_build_array(to_jsonb(r));
     END LOOP;
     IF got <> '[{"endpoint":1},{"endpoint":2},{"endpoint":3}]'::jsonb THEN
       RAISE EXCEPTION 'versioned cached graph plan changed results: %',got;
     END IF;
   END LOOP;
   EXECUTE 'DEALLOCATE route_versioned';
 END LOOP;
END $$;

RESET search_path;
DROP SCHEMA ggc_route CASCADE;

-- Statistics are estimates, not uniqueness proofs. Exercise both providers
-- with no graph statistics and with real ordinary ANALYZE statistics.
CREATE SCHEMA ggc_route_estimate;
SET search_path=ggc_route_estimate,public;
CREATE TABLE v(id bigint PRIMARY KEY);
CREATE TABLE e(id bigint PRIMARY KEY,src bigint NOT NULL,dst bigint NOT NULL,payload text);
INSERT INTO v SELECT i FROM generate_series(0,1000) s(i);
INSERT INTO e SELECT i,i-1,i,'' FROM generate_series(1,1000) s(i);
CREATE INDEX e_btree ON e(src);
CREATE INDEX e_adj ON e USING graph_adj(src,dst);
CREATE PROPERTY GRAPH g VERTEX TABLES(v KEY(id)) EDGE TABLES(e KEY(id)
 SOURCE KEY(src) REFERENCES v(id) DESTINATION KEY(dst) REFERENCES v(id));
VACUUM (FREEZE,ANALYZE) v;
VACUUM (FREEZE,ANALYZE) e;
DO $$
DECLARE q text='SELECT DISTINCT endpoint FROM GRAPH_TABLE(g MATCH (v0 IS v WHERE v0.id=0)';
 mode text; p jsonb; g jsonb; multi text;
BEGIN
 FOR i IN 1..10 LOOP
   q:=q||format('-[e%s IS e]->(v%s IS v)',i,i);
 END LOOP;
 q:=q||' COLUMNS(v10.id AS endpoint))';
 FOREACH mode IN ARRAY ARRAY['btree','adj'] LOOP
   PERFORM pg_temp.route_check(q,mode,'[{"endpoint":10}]');
   EXECUTE format('EXPLAIN (ANALYZE,TIMING OFF,FORMAT JSON) /*+ GraphRoute(%s) */ ',mode)||q INTO p;
   g:=p->0->'Plan';
   IF g->>'Custom Plan Provider' IS DISTINCT FROM 'GraphExpandFixed'
      OR (g->>'Plan Rows')::numeric<>1
      OR g->>'Stage 1 Vertex Visibility'<>'probes=1 rejected=0 index_only=1 heap_fetches=0 scan_starts=1'
      OR g->>'Stage 10 Vertex Visibility'<>'probes=1 rejected=0 index_only=1 heap_fetches=0 scan_starts=0' THEN
     RAISE EXCEPTION 'ordinary statistics / unique output / vertex VM optimization failed: %',g;
   END IF;
   multi:=replace(q,' WHERE v0.id=0','');
   EXECUTE format('EXPLAIN (FORMAT JSON) /*+ GraphRoute(%s) */ ',mode)||multi INTO p;
   IF p->0->'Plan'->>'Node Type' NOT IN ('Unique','Aggregate') THEN
     RAISE EXCEPTION 'multiple anchors lost global DISTINCT: %',p;
   END IF;
 END LOOP;
END $$;
RESET search_path;
DROP SCHEMA ggc_route_estimate CASCADE;

-- DISTINCT's sort must not survive a global uniqueness proof just because
-- add_path's fuzzy comparison favors its otherwise unused pathkeys.  A high
-- page cost makes that tie reproducible; it is not a performance setting.
CREATE SCHEMA ggc_route_sort;
SET search_path=ggc_route_sort,public;
CREATE TABLE v(id bigint PRIMARY KEY);
CREATE TABLE e(id bigint PRIMARY KEY,src bigint NOT NULL,dst bigint NOT NULL,payload text);
INSERT INTO v SELECT i FROM generate_series(0,1000) s(i);
INSERT INTO e SELECT i,i-1,i,'' FROM generate_series(1,1000) s(i);
INSERT INTO e VALUES(1001,0,2,''),(1002,0,3,''),(1003,0,2,'');
CREATE INDEX e_btree ON e(src);
CREATE INDEX e_adj ON e USING graph_adj(src,dst);
CREATE PROPERTY GRAPH g VERTEX TABLES(v KEY(id)) EDGE TABLES(e KEY(id)
 SOURCE KEY(src) REFERENCES v(id) DESTINATION KEY(dst) REFERENCES v(id));
VACUUM (FREEZE,ANALYZE) v;
VACUUM (FREEZE,ANALYZE) e;
DO $$
DECLARE q text='SELECT DISTINCT endpoint FROM GRAPH_TABLE(g MATCH
 (a IS v WHERE a.id=0::bigint)-[x IS e]->(b IS v) COLUMNS(b.id AS endpoint))';
 mode text; hashagg text; direction text; forced text; p jsonb;
 r record; got bigint[]; expected bigint[];
BEGIN
 PERFORM set_config('random_page_cost','10000',true);
 FOREACH mode IN ARRAY ARRAY['btree','adj'] LOOP
   FOREACH hashagg IN ARRAY ARRAY['on','off'] LOOP
     PERFORM set_config('enable_hashagg',hashagg,true);
     forced:=format('/*+ GraphRoute(%s) */ ',mode)||q;
     EXECUTE 'EXPLAIN (FORMAT JSON) '||forced INTO p;
     IF p->0->'Plan'->>'Custom Plan Provider' IS DISTINCT FROM 'GraphExpandFixed' THEN
       RAISE EXCEPTION 'unneeded DISTINCT sort survived: %',p;
     END IF;
     got:='{}';
     FOR r IN EXECUTE forced LOOP got:=array_append(got,r.endpoint); END LOOP;
     SELECT array_agg(x ORDER BY x) INTO got FROM unnest(got) s(x);
     IF got IS DISTINCT FROM ARRAY[1,2,3]::bigint[] THEN
       RAISE EXCEPTION 'unique endpoint results changed: %',got;
     END IF;
     FOREACH direction IN ARRAY ARRAY['ASC','DESC'] LOOP
       EXECUTE 'EXPLAIN (FORMAT JSON) '||forced||' ORDER BY endpoint '||direction INTO p;
       IF p->0->'Plan'->>'Node Type' IS DISTINCT FROM 'Sort'
          OR p->0->'Plan'->'Plans'->0->>'Custom Plan Provider' IS DISTINCT FROM 'GraphExpandFixed' THEN
         RAISE EXCEPTION 'required ordering lost or redundant Unique retained: %',p;
       END IF;
       expected:=CASE direction WHEN 'ASC' THEN ARRAY[1,2,3] ELSE ARRAY[3,2,1] END;
       got:='{}';
       FOR r IN EXECUTE forced||' ORDER BY endpoint '||direction LOOP
         got:=array_append(got,r.endpoint);
       END LOOP;
       IF got IS DISTINCT FROM expected THEN RAISE EXCEPTION 'ORDER BY changed: %',got; END IF;
       got:='{}';
       FOR r IN EXECUTE forced||' ORDER BY endpoint '||direction||' LIMIT 2' LOOP
         got:=array_append(got,r.endpoint);
       END LOOP;
       IF got IS DISTINCT FROM expected[1:2] THEN RAISE EXCEPTION 'ordered LIMIT changed: %',got; END IF;
     END LOOP;
     EXECUTE 'EXPLAIN (FORMAT JSON) '||replace(forced,'a.id=0::bigint','a.id IN (0,1)') INTO p;
     IF p->0->'Plan'->>'Node Type' NOT IN ('Unique','Aggregate') THEN
       RAISE EXCEPTION 'multiple anchors lost global DISTINCT: %',p;
     END IF;
   END LOOP;
 END LOOP;
END $$;
RESET search_path;
DROP SCHEMA ggc_route_sort CASCADE;
