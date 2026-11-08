-- Standalone resource/frontier acceptance; run in an isolated test database.
-- Requires installed test_graph_core and the ordinary B-tree provider only.
\set ON_ERROR_STOP on
SET client_min_messages=warning;
LOAD 'test_graph_core';
SET jit=off;
SET max_parallel_workers_per_gather=0;
SET work_mem='128MB';
CREATE SCHEMA ggc_resources;
SET search_path=ggc_resources,public;
CREATE TABLE v(id bigint PRIMARY KEY);
CREATE TABLE e(id bigint PRIMARY KEY,src bigint NOT NULL,dst bigint NOT NULL);
CREATE INDEX e_out ON e(src);
INSERT INTO v SELECT i FROM generate_series(1,100002) i;
INSERT INTO e SELECT i,1,i+1 FROM generate_series(1,100001) i;
CREATE PROPERTY GRAPH g VERTEX TABLES(v KEY(id)) EDGE TABLES(e KEY(id)
 SOURCE KEY(src) REFERENCES v(id) DESTINATION KEY(dst) REFERENCES v(id));
ANALYZE v;
ANALYZE e;

-- Count in the client-side PL/pgSQL loop, not a SQL aggregate wrapper:
-- wrapping DISTINCT in a subquery changes the endpoint-set admission proof.
CREATE FUNCTION pg_temp.resource_count(q text) RETURNS bigint LANGUAGE plpgsql AS $$
DECLARE r record; n bigint=0;
BEGIN
 FOR r IN EXECUTE q LOOP n:=n+1; END LOOP;
 RETURN n;
END $$;

-- One final-stage stream must not fail at the former hidden 100000-row cap.
DO $$
DECLARE n bigint; q text=$q$SELECT z FROM GRAPH_TABLE(g MATCH
 (a IS v WHERE a.id=1::bigint)-[x IS e]->(b IS v)
 COLUMNS(b.id AS z)) t$q$;
BEGIN
 n:=pg_temp.resource_count('/*+ GraphRoute(btree) */ '||q);
 IF n<>100001 THEN RAISE EXCEPTION 'lost streamed bindings: %',n; END IF;
END $$;

-- Same-depth distinct endpoints; every later layer revisits the same keys.
-- 64*4096 physical candidates exceed 100000 while only two frontiers are live.
TRUNCATE e;
INSERT INTO e SELECT i,1,i+1 FROM generate_series(1,4096) i;
INSERT INTO e SELECT 4096+i,i+1,i+1 FROM generate_series(1,4096) i;
ANALYZE e;
SET work_mem='4MB';
CREATE FUNCTION pg_temp.resource_query(hops integer, distinct_result boolean)
RETURNS text LANGUAGE plpgsql AS $$
DECLARE pattern text='(v0 IS v WHERE v0.id=1::bigint)';
BEGIN
 FOR i IN 1..hops LOOP
   pattern:=pattern||format('-[e%s IS e]->(v%s IS v)',i,i);
 END LOOP;
 RETURN 'SELECT '||CASE WHEN distinct_result THEN 'DISTINCT ' ELSE '' END||
   'endpoint FROM GRAPH_TABLE(g MATCH '||pattern||
   format(' COLUMNS(v%s.id AS endpoint)) gt',hops);
END $$;

DO $$
DECLARE q text=pg_temp.resource_query(64,true); p jsonb; n bigint;
        graph_plan jsonb; peak bigint;
BEGIN
 EXECUTE 'EXPLAIN(ANALYZE,FORMAT JSON,TIMING OFF) /*+ GraphRoute(btree) */ '||q INTO p;
 WITH RECURSIVE plans(x) AS (
   SELECT p->0->'Plan' UNION ALL
   SELECT c FROM plans CROSS JOIN LATERAL jsonb_array_elements(x->'Plans') c)
 SELECT x INTO graph_plan FROM plans
 WHERE x->>'Custom Plan Provider'='GraphExpandFixed';
 IF graph_plan IS NULL OR graph_plan->>'Graph Prefix Representation'<>
      'exact-k WALK endpoint sets (unordered)' THEN
   RAISE EXCEPTION 'resource test did not use endpoint sets: %',p;
 END IF;
 n:=(graph_plan->>'Actual Rows')::numeric::bigint;
 peak:=(graph_plan->>'Factorized Peak Bytes')::bigint;
 IF n<>4096 OR peak>4*1024*1024 THEN
   RAISE EXCEPTION 'wrong rows or retained historic frontiers: %, %',n,peak;
 END IF;
 -- SQL/PGQ installs a vertex check on each hop.  Identical edge access may
 -- reuse startup metadata without dropping those independent vertex checks.
 IF (graph_plan->>'Graph Startup Revalidations')::integer<>1 OR
    (graph_plan->>'Graph Startup Metadata Reuses')::integer<>63 THEN
   RAISE EXCEPTION 'identical PGQ stage metadata was not reused: %',graph_plan;
 END IF;
 -- Repeat after teardown to check that no dangling reader/hash state escapes.
 n:=pg_temp.resource_count('/*+ GraphRoute(btree) */ '||q);
 IF n<>4096 THEN RAISE EXCEPTION 'wrong repeated endpoint set: %',n; END IF;
END $$;

-- Fixed bag execution materializes prefix stages.  Its startup cost must
-- expose that work even when a separate streaming candidate can serve LIMIT.
SET graph_btree_max_candidates=1;
SET work_mem='16MB';
DO $$
DECLARE q2 text=pg_temp.resource_query(2,false);
 q3 text=pg_temp.resource_query(3,false); q text; p jsonb; g jsonb;
 plans jsonb[]='{}'; failed boolean=false; detail text;
BEGIN
 FOREACH q IN ARRAY ARRAY[q2,q3] LOOP
  EXECUTE 'EXPLAIN(ANALYZE,TIMING OFF,FORMAT JSON) /*+ GraphRoute(btree) */ '||q INTO p;
  WITH RECURSIVE nodes(n) AS (SELECT p->0->'Plan' UNION ALL
   SELECT c FROM nodes CROSS JOIN LATERAL jsonb_array_elements(n->'Plans') c)
  SELECT n INTO STRICT g FROM nodes WHERE n->>'Custom Plan Provider'='GraphExpandFixed';
  plans:=array_append(plans,g);
 END LOOP;
 IF (plans[2]->>'Actual Rows')::numeric::int<>4096 THEN
  RAISE EXCEPTION 'full result mismatch: %',plans;
 END IF;
 FOR i IN 1..2 LOOP
  IF substring(plans[2]->>format('Stage %s Batch Summary',i)
       from ' output=([0-9]+)')::int IS DISTINCT FROM 4096 THEN
   RAISE EXCEPTION 'missing expected prefix work: %',plans[2];
  END IF;
 END LOOP;
 IF (plans[2]->>'Startup Cost')::numeric < (plans[1]->>'Total Cost')::numeric THEN
  RAISE EXCEPTION 'startup/full costing mismatch: %',plans;
 END IF;
 PERFORM set_config('work_mem','64kB',true);
 BEGIN EXECUTE '/*+ GraphRoute(btree) */ '||q3;
 EXCEPTION WHEN program_limit_exceeded THEN
  GET STACKED DIAGNOSTICS detail=MESSAGE_TEXT;
  IF detail<>'fixed graph binding memory limit exceeded' THEN RAISE; END IF;
  failed:=true;
 END;
 IF NOT failed THEN RAISE EXCEPTION 'full query bypassed prefix memory cap'; END IF;
 PERFORM set_config('work_mem','16MB',true);
 IF pg_temp.resource_count('/*+ GraphRoute(btree) */ '||q3)<>4096 THEN
  RAISE EXCEPTION 'execution corrupted after memory rejection';
 END IF;
END $$;
RESET graph_btree_max_candidates;
SET work_mem='4MB';

-- Preserve bag multiplicities; compare the complete ordered key multiset.
INSERT INTO e VALUES(90000,1,2),(90001,2,2);
ANALYZE e;
DO $$
DECLARE q text=pg_temp.resource_query(2,false); actual bigint[]; expected bigint[];
BEGIN
 EXECUTE '/*+ GraphRoute(btree) */ SELECT array_agg(endpoint ORDER BY endpoint) FROM ('||q||') s'
   INTO actual;
 EXECUTE '/*+ GraphRoute(relational) */ SELECT array_agg(endpoint ORDER BY endpoint) FROM ('||q||') s'
   INTO expected;
 IF actual IS DISTINCT FROM expected THEN RAISE EXCEPTION 'bag multiplicity changed'; END IF;
END $$;

-- First anchor has a branched frontier but no second-hop result; another
-- anchor succeeds.  Check the actual route before attributing this to
-- endpoint-set reclamation (generic/native fallback is not that evidence).
INSERT INTO v VALUES(0);
INSERT INTO e VALUES(90002,0,99999),(90003,0,100000);
ANALYZE v;
ANALYZE e;
DO $$
DECLARE q text=replace(pg_temp.resource_query(2,true),'v0.id=1::bigint',
                       'v0.id IN (0::bigint,1::bigint)');
        actual bigint[]='{}'; expected bigint[]='{}'; r record; p jsonb;
        graph_plan jsonb;
BEGIN
 EXECUTE 'EXPLAIN(FORMAT JSON) /*+ GraphRoute(btree) */ '||q INTO p;
 WITH RECURSIVE plans(x) AS (
   SELECT p->0->'Plan' UNION ALL
   SELECT c FROM plans CROSS JOIN LATERAL jsonb_array_elements(x->'Plans') c)
 SELECT x INTO graph_plan FROM plans
 WHERE x->>'Custom Plan Provider'='GraphExpandFixed';
 IF graph_plan->>'Graph Prefix Representation' IS DISTINCT FROM
      'exact-k WALK endpoint sets (unordered)' THEN
   RAISE NOTICE 'multi-anchor case checks bag/native semantics, not endpoint lifetime';
 END IF;
 FOR r IN EXECUTE '/*+ GraphRoute(btree) */ '||q||' ORDER BY endpoint' LOOP
   actual:=array_append(actual,r.endpoint);
 END LOOP;
 FOR r IN EXECUTE '/*+ GraphRoute(relational) */ '||q||' ORDER BY endpoint' LOOP
   expected:=array_append(expected,r.endpoint);
 END LOOP;
 IF actual IS DISTINCT FROM expected THEN RAISE EXCEPTION 'anchor frontier lifetime mismatch'; END IF;
END $$;

-- Explicit low-memory rejection, not truncation or a runtime route switch.
-- Each failed statement rolls back to the PL/pgSQL exception subtransaction.
DO $$
DECLARE n bigint;
BEGIN
 EXECUTE 'PREPARE resource_cached AS /*+ GraphRoute(btree) */ '||pg_temp.resource_query(2,true);
 n:=pg_temp.resource_count('EXECUTE resource_cached');
 IF n<>4096 THEN RAISE EXCEPTION 'wrong prepared result'; END IF;
END $$;
SET work_mem='64kB';
DO $$
DECLARE q text; failed boolean; detail text;
BEGIN
 FOREACH q IN ARRAY ARRAY[pg_temp.resource_query(2,true),pg_temp.resource_query(2,false)] LOOP
   failed:=false;
   BEGIN
     EXECUTE '/*+ GraphRoute(btree) */ '||q;
   EXCEPTION WHEN program_limit_exceeded THEN
     GET STACKED DIAGNOSTICS detail=MESSAGE_TEXT;
     IF detail NOT LIKE '%memory limit%' THEN RAISE; END IF;
     failed:=true;
   END;
   IF NOT failed THEN RAISE EXCEPTION 'expected explicit memory rejection'; END IF;
 END LOOP;
 failed:=false;
 BEGIN
   EXECUTE 'EXECUTE resource_cached';
 EXCEPTION WHEN program_limit_exceeded THEN
   GET STACKED DIAGNOSTICS detail=MESSAGE_TEXT;
   IF detail NOT LIKE '%memory limit%' THEN RAISE; END IF;
   failed:=true;
 END;
 IF NOT failed THEN RAISE EXCEPTION 'cached plan ignored execution-time work_mem'; END IF;
END $$;
SET work_mem='4MB';
DO $$
DECLARE n bigint;
BEGIN
 n:=pg_temp.resource_count('/*+ GraphRoute(btree) */ '||pg_temp.resource_query(2,true));
 IF n<>4096 THEN RAISE EXCEPTION 'state corrupted after low-memory errors'; END IF;
 n:=pg_temp.resource_count('EXECUTE resource_cached');
 IF n<>4096 THEN RAISE EXCEPTION 'cached plan failed after restoring work_mem'; END IF;
END $$;
DEALLOCATE resource_cached;

-- Exercise all 64 key bits, sign, zero and the int64 extrema.  Parallel edges
-- cause repeated insert-or-find requests; growth must preserve the endpoint
-- set and (on the separate binding route) the complete bag multiplicities.
TRUNCATE e,v;
INSERT INTO v SELECT sign*(i::bigint*4294967296+i%4)
 FROM generate_series(1,512) i CROSS JOIN (VALUES(-1),(1)) s(sign);
INSERT INTO v VALUES(1),(0),('-9223372036854775808'),('9223372036854775807');
INSERT INTO e SELECT row_number() OVER(),1,id FROM v CROSS JOIN generate_series(1,3) n
 WHERE id<>1;
INSERT INTO e SELECT 10000+row_number() OVER(),id,id FROM v
 CROSS JOIN generate_series(1,3) n WHERE id<>1;
ANALYZE v;
ANALYZE e;
DO $$
DECLARE q text; actual bigint[]; expected bigint[]; r record;
BEGIN
 FOREACH q IN ARRAY ARRAY[pg_temp.resource_query(2,true),pg_temp.resource_query(2,false)] LOOP
   actual:='{}'; expected:='{}';
   FOR r IN EXECUTE '/*+ GraphRoute(btree) */ '||q||' ORDER BY endpoint' LOOP
     actual:=array_append(actual,r.endpoint);
   END LOOP;
   FOR r IN EXECUTE '/*+ GraphRoute(relational) */ '||q||' ORDER BY endpoint' LOOP
     expected:=array_append(expected,r.endpoint);
   END LOOP;
   IF actual IS DISTINCT FROM expected THEN RAISE EXCEPTION 'int64 hash/bag mismatch'; END IF;
 END LOOP;
END $$;

-- An initially ordered frontier must remain searchable when a later batch
-- revisits an interior key, adds a smaller key, or names an invisible vertex.
-- The prefix spans several node chunks; switching to the hash index must not
-- move those nodes or lose their handles.  int64 comparisons must not overflow.
TRUNCATE e,v;
INSERT INTO v SELECT i FROM generate_series(100,355) i;
INSERT INTO v VALUES(1),(0),('-9223372036854775808'),('9223372036854775807');
INSERT INTO e SELECT i,1,i FROM generate_series(100,355) i;
INSERT INTO e VALUES
 (1000,1,150),(1001,1,155),(1002,1,400),
 (1003,1,'9223372036854775807'),(1004,1,'-9223372036854775808'),
 (1005,1,0),(1006,1,355);
INSERT INTO e SELECT 2000+row_number() OVER(),id,id FROM v WHERE id<>1;
ANALYZE v;
ANALYZE e;
DO $$
DECLARE q text; actual bigint[]; expected bigint[]; r record;
        batch integer; batched boolean;
BEGIN
 FOREACH batch IN ARRAY ARRAY[1,17,128] LOOP
  PERFORM set_config('test_graph_core.vertex_batch_size',batch::text,false);
  FOREACH batched IN ARRAY ARRAY[false,true] LOOP
   PERFORM set_config('test_graph_core.vertex_probe_batch',batched::text,false);
   FOREACH q IN ARRAY ARRAY[pg_temp.resource_query(3,true),pg_temp.resource_query(3,false)] LOOP
    actual:='{}'; expected:='{}';
    FOR r IN EXECUTE '/*+ GraphRoute(btree) */ '||q||' ORDER BY endpoint' LOOP
      actual:=array_append(actual,r.endpoint);
    END LOOP;
    FOR r IN EXECUTE '/*+ GraphRoute(relational) */ '||q||' ORDER BY endpoint' LOOP
      expected:=array_append(expected,r.endpoint);
    END LOOP;
    IF actual IS DISTINCT FROM expected THEN
      RAISE EXCEPTION 'ordered/hash transition mismatch: batch %, batched %',batch,batched;
    END IF;
   END LOOP;
  END LOOP;
 END LOOP;
END $$;
RESET test_graph_core.vertex_batch_size;
RESET test_graph_core.vertex_probe_batch;

RESET search_path;
DROP SCHEMA ggc_resources CASCADE;
RESET work_mem;
RESET max_parallel_workers_per_gather;
RESET jit;
SELECT 'graph resource/frontier checks passed';
