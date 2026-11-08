\set ECHO none
\set ON_ERROR_STOP on
SET client_min_messages=warning;
LOAD 'test_graph_core';
SET jit=off;
SET max_parallel_workers_per_gather=0;
SET graph_btree_max_candidates=1;
SET work_mem='16MB';
CREATE SCHEMA ggc_limit_stream;
SET search_path=ggc_limit_stream,public;
CREATE TABLE v(id bigint PRIMARY KEY,payload text);
CREATE TABLE e(id bigint PRIMARY KEY,src bigint NOT NULL,dst bigint NOT NULL);
INSERT INTO v SELECT i,'vertex-'||i FROM generate_series(1,4097) i;
INSERT INTO e SELECT i,1,i+1 FROM generate_series(1,4096) i;
INSERT INTO e SELECT 4096+i,i+1,i+1 FROM generate_series(1,4096) i;
CREATE INDEX e_out ON e(src) INCLUDE(dst,id);
CREATE PROPERTY GRAPH g VERTEX TABLES(v KEY(id)) EDGE TABLES(e KEY(id)
 SOURCE KEY(src) REFERENCES v(id) DESTINATION KEY(dst) REFERENCES v(id));
VACUUM(FREEZE,ANALYZE) v;
VACUUM(FREEZE,ANALYZE) e;

CREATE FUNCTION pg_temp.stream_query() RETURNS text LANGUAGE sql AS $$
 SELECT $q$SELECT * FROM GRAPH_TABLE(g MATCH
  (a IS v WHERE a.id=1::bigint)-[x IS e]->(b IS v)
  -[y IS e]->(c IS v)-[z IS e]->(d IS v)
  COLUMNS(a.id AS anchor,x.id AS firstedge,y.id AS secondedge,
   z.id AS thirdedge,d.id AS endpoint,b.payload AS firstpayload,
   d.payload AS lastpayload)) gt$q$
$$;
CREATE FUNCTION pg_temp.stream_nodes(p jsonb) RETURNS SETOF jsonb LANGUAGE sql AS $$
 WITH RECURSIVE plans(n) AS (SELECT p->0->'Plan' UNION ALL
  SELECT c FROM plans CROSS JOIN LATERAL jsonb_array_elements(n->'Plans') c)
 SELECT n FROM plans
$$;
CREATE FUNCTION pg_temp.stream_plan(q text) RETURNS jsonb LANGUAGE plpgsql AS $$
DECLARE p jsonb;
BEGIN
 EXECUTE 'EXPLAIN(ANALYZE,VERBOSE,TIMING OFF,FORMAT JSON) '||q INTO p;
 RETURN p;
END $$;
-- Execute the supplied query directly: a SQL aggregate wrapper would clear
-- its tuple_fraction and would not exercise the limited candidate at all.
CREATE FUNCTION pg_temp.stream_bag(q text) RETURNS jsonb LANGUAGE plpgsql AS $$
DECLARE r record; rows jsonb[]='{}';
BEGIN
 FOR r IN EXECUTE q LOOP rows:=array_append(rows,to_jsonb(r)); END LOOP;
 RETURN (SELECT coalesce(jsonb_agg(j ORDER BY j),'[]'::jsonb) FROM unnest(rows) j);
END $$;

-- A plain outer LIMIT reaches graph planning through ordinary subquery pullup
-- or tuple_fraction propagation.  The high-fanout prefix makes the streaming
-- candidate win naturally; no route or cost switch forces Native to win.
DO $$
DECLARE q text=pg_temp.stream_query(); full_plan jsonb; limited_plan jsonb;
 fixed jsonb; native jsonb; full_bag jsonb; limited_bag jsonb; child_rows numeric;
BEGIN
 full_plan:=pg_temp.stream_plan('/*+ GraphRoute(btree) */ '||q);
 SELECT n INTO STRICT fixed FROM pg_temp.stream_nodes(full_plan) n
  WHERE n->>'Custom Plan Provider'='GraphExpandFixed';
 IF (fixed->>'Actual Rows')::numeric<>4096 THEN
  RAISE EXCEPTION 'full graph lost bindings: %',fixed;
 END IF;
 FOR i IN 1..2 LOOP
  IF substring(fixed->>format('Stage %s Batch Summary',i)
       from ' output=([0-9]+)')::int IS DISTINCT FROM 4096 THEN
   RAISE EXCEPTION 'full Fixed prefix changed: %',fixed;
  END IF;
 END LOOP;
 full_bag:=pg_temp.stream_bag('/*+ GraphRoute(relational) */ '||q);
 IF jsonb_array_length(full_bag)<>4096 OR full_bag IS DISTINCT FROM
    pg_temp.stream_bag('/*+ GraphRoute(btree) */ '||q) THEN
  RAISE EXCEPTION 'full result bag changed';
 END IF;
 PERFORM set_config('work_mem','64kB',true);
 limited_plan:=pg_temp.stream_plan('/*+ GraphRoute(btree) */ '||q||' LIMIT 1');
 SELECT n INTO STRICT native FROM pg_temp.stream_nodes(limited_plan) n
  WHERE n->>'Custom Plan Provider'='GraphExpandNative';
 SELECT sum((n->>'Actual Rows')::numeric*(n->>'Actual Loops')::numeric)
  INTO child_rows FROM jsonb_array_elements(native->'Plans') n;
 IF (native->>'Actual Rows')::numeric<>1 OR child_rows IS NULL OR child_rows>32 OR
    (native->>'Startup Cost')::numeric >= (fixed->>'Startup Cost')::numeric OR
    EXISTS (SELECT FROM pg_temp.stream_nodes(limited_plan) n
            WHERE n->>'Node Type' IN ('Nested Loop','Hash Join','Merge Join')) OR
    limited_plan::text ~ '\$[0-9]+' THEN
  RAISE EXCEPTION 'LIMIT did not stream a legal native graph prefix: %',limited_plan;
 END IF;
 -- Unordered LIMIT does not promise the same chosen row across access paths.
 -- Check cardinality and exact row membership, including property payloads.
 limited_bag:=pg_temp.stream_bag('/*+ GraphRoute(btree) */ '||q||' LIMIT 1');
 IF jsonb_array_length(limited_bag)<>1 OR NOT full_bag @> limited_bag THEN
  RAISE EXCEPTION 'limited graph returned a nonmember: %',limited_bag;
 END IF;
 -- Cache controls and disabling ordinary nested loops do not disable the
 -- independent graph route or introduce a hidden relational join plan.
 PERFORM set_config('enable_nestloop','off',true);
 PERFORM set_config('enable_memoize','off',true);
 PERFORM set_config('enable_material','off',true);
 limited_plan:=pg_temp.stream_plan('/*+ GraphRoute(btree) */ '||q||' LIMIT 1');
 IF NOT EXISTS (SELECT FROM pg_temp.stream_nodes(limited_plan) n
     WHERE n->>'Custom Plan Provider'='GraphExpandNative') OR
    EXISTS (SELECT FROM pg_temp.stream_nodes(limited_plan) n
     WHERE n->>'Node Type' IN ('Nested Loop','Hash Join','Merge Join','Memoize','Materialize')) THEN
  RAISE EXCEPTION 'native LIMIT ignored graph/cache controls: %',limited_plan;
 END IF;
END $$;

-- Repeated early EOF and changing external LIMIT parameters exercise cached
-- plans without leaving child parameters or bindings alive across executions.
DO $$
DECLARE q text=pg_temp.stream_query(); mode text; actual jsonb; full_bag jsonb;
 p jsonb; limit_count integer;
BEGIN
 full_bag:=pg_temp.stream_bag('/*+ GraphRoute(relational) */ '||q);
 EXECUTE 'PREPARE stream_cached(bigint) AS /*+ GraphRoute(btree) */ '||q||' LIMIT $1';
 PERFORM set_config('work_mem','64kB',true);
 FOREACH mode IN ARRAY ARRAY['force_generic_plan','force_custom_plan'] LOOP
  PERFORM set_config('plan_cache_mode',mode,true);
  FOREACH limit_count IN ARRAY ARRAY[1,2,0,1] LOOP
   actual:=pg_temp.stream_bag(format('EXECUTE stream_cached(%s)',limit_count));
   IF jsonb_array_length(actual)<>limit_count OR NOT full_bag @> actual THEN
    RAISE EXCEPTION 'cached limited result changed: %, %, %',mode,limit_count,actual;
   END IF;
  END LOOP;
  p:=pg_temp.stream_plan('EXECUTE stream_cached(1)');
  IF NOT EXISTS (SELECT FROM pg_temp.stream_nodes(p) n
      WHERE n->>'Custom Plan Provider'='GraphExpandNative') THEN
   RAISE EXCEPTION 'cached LIMIT did not retain native graph plan: %, %',mode,p;
  END IF;
 END LOOP;
 DEALLOCATE stream_cached;
END $$;

-- LIMIT ALL does not add a partial-retrieval candidate.  OFFSET 0 preserves
-- a subquery boundary, where blocking outer operations stop tuple_fraction
-- propagation.  Without that boundary, graph_program can be pulled up into
-- the same root as Sort/Agg/Distinct: either graph path may win normal costs.
-- In both cases deterministic ordered results must match the reference.
DO $$
DECLARE q text=pg_temp.stream_query(); query text; p jsonb; query_number integer=0;
BEGIN
 FOREACH query IN ARRAY ARRAY[
  q||' LIMIT ALL',
  'SELECT * FROM ('||q||' OFFSET 0) s ORDER BY firstedge,secondedge,thirdedge LIMIT 5 OFFSET 2',
  'SELECT DISTINCT endpoint FROM ('||q||' OFFSET 0) s ORDER BY endpoint LIMIT 5',
  'SELECT count(*) AS n FROM ('||q||' OFFSET 0) s LIMIT 1',
  q||' ORDER BY firstedge,secondedge,thirdedge LIMIT 5 OFFSET 2',
  'SELECT DISTINCT endpoint FROM ('||q||') s ORDER BY endpoint LIMIT 5',
  'SELECT count(*) AS n FROM ('||q||') s LIMIT 1'
 ] LOOP
  query_number:=query_number+1;
  p:=pg_temp.stream_plan('/*+ GraphRoute(btree) */ '||query);
  IF query_number<=4 AND (NOT EXISTS (SELECT FROM pg_temp.stream_nodes(p) n
      WHERE n->>'Custom Plan Provider'='GraphExpandFixed') OR
     EXISTS (SELECT FROM pg_temp.stream_nodes(p) n
      WHERE n->>'Custom Plan Provider'='GraphExpandNative')) THEN
   RAISE EXCEPTION 'outer operation changed full-retrieval graph search: %',p;
  END IF;
  IF (SELECT count(*) FROM pg_temp.stream_nodes(p) n WHERE
       n->>'Custom Plan Provider' IN ('GraphExpandFixed','GraphExpandNative'))<>1 THEN
   RAISE EXCEPTION 'missing or duplicate graph region: %',p;
  END IF;
  IF pg_temp.stream_bag('/*+ GraphRoute(btree) */ '||query) IS DISTINCT FROM
     pg_temp.stream_bag('/*+ GraphRoute(relational) */ '||query) THEN
   RAISE EXCEPTION 'limited ordered/set/aggregate result changed: %',query;
  END IF;
 END LOOP;
END $$;

DROP SCHEMA ggc_limit_stream CASCADE;
RESET search_path;
RESET work_mem;
RESET graph_btree_max_candidates;
RESET max_parallel_workers_per_gather;
RESET jit;
RESET client_min_messages;
\echo graph partial retrieval, native startup, and cached LIMIT: ok
