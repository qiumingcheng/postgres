\set ECHO none
SET client_min_messages=warning;
LOAD 'test_graph_core';
SET jit=off;
SET graph_btree_max_candidates=1;
SET test_graph_core.vertex_probe_batch=off;
-- Isolate the role-scoped cost profile from other regression fixtures.
CREATE ROLE ggc_index_cost_role;
CREATE SCHEMA ggc_index_cost AUTHORIZATION ggc_index_cost_role;
SET ROLE ggc_index_cost_role;
SET search_path=ggc_index_cost,public;
CREATE TABLE v(id bigint NOT NULL,pad text);
INSERT INTO v SELECT i,md5(i::text)||md5((i+1)::text)||md5((i+2)::text)||md5((i+3)::text)
 FROM generate_series(1,6001) i;
CREATE UNIQUE INDEX v_wide ON v(id) INCLUDE(pad);
CREATE UNIQUE INDEX v_narrow ON v(id);
-- Non-canonical column order: pad is not the compact legacy payload column.
CREATE TABLE e(src bigint NOT NULL,dst bigint NOT NULL,id bigint PRIMARY KEY,pad text);
INSERT INTO e SELECT 1,i+1,i,NULL FROM generate_series(1,16) i;
INSERT INTO e SELECT i+1,i+99,i+16,NULL FROM generate_series(1,16) i;
INSERT INTO e SELECT i+1,1,i+32,NULL FROM generate_series(1,16) i;
INSERT INTO e SELECT i+99,i+1,i+48,NULL FROM generate_series(1,16) i;
INSERT INTO e SELECT i,i+1,i,
 (SELECT string_agg(md5((i+j)::text),'' ORDER BY j) FROM generate_series(1,32) j)
 FROM generate_series(1000,5999) i;
CREATE PROPERTY GRAPH g VERTEX TABLES(v KEY(id)) EDGE TABLES(e KEY(id)
 SOURCE KEY(src) REFERENCES v(id) DESTINATION KEY(dst) REFERENCES v(id));
VACUUM(FREEZE,ANALYZE) v;
VACUUM(FREEZE,ANALYZE) e;

CREATE FUNCTION pg_temp.index_bag(q text) RETURNS jsonb LANGUAGE plpgsql AS $$
DECLARE r record; values_json jsonb='[]';
BEGIN
 FOR r IN EXECUTE q LOOP values_json:=values_json||jsonb_build_array(to_jsonb(r)); END LOOP;
 RETURN (SELECT coalesce(jsonb_agg(value ORDER BY value),'[]'::jsonb)
         FROM jsonb_array_elements(values_json));
END $$;
CREATE FUNCTION pg_temp.index_graph(q text,analyze_query boolean DEFAULT false)
RETURNS jsonb LANGUAGE plpgsql AS $$
DECLARE p jsonb; g jsonb;
BEGIN
 EXECUTE 'EXPLAIN(FORMAT JSON,COSTS ON'||
   CASE WHEN analyze_query THEN ',ANALYZE,TIMING OFF' ELSE '' END||
   ') /*+ GraphRoute(btree) */ '||q INTO p;
 WITH RECURSIVE plans(n) AS (SELECT p->0->'Plan' UNION ALL
  SELECT c FROM plans CROSS JOIN LATERAL jsonb_array_elements(n->'Plans') c)
 SELECT n INTO STRICT g FROM plans WHERE n->>'Custom Plan Provider'='GraphExpandFixed';
 RETURN g;
END $$;
CREATE FUNCTION pg_temp.index_query(incoming boolean,bag boolean DEFAULT false)
RETURNS text LANGUAGE sql AS $$
 SELECT CASE WHEN bag THEN 'SELECT *' ELSE 'SELECT DISTINCT endpoint' END||
 ' FROM GRAPH_TABLE(g MATCH (a IS v WHERE a.id=1::bigint)'||
 CASE WHEN incoming THEN '<-[x IS e]-' ELSE '-[x IS e]->' END||
 '(b IS v)'||CASE WHEN incoming THEN '<-[y IS e]-' ELSE '-[y IS e]->' END||
 '(z IS v) COLUMNS('||CASE WHEN bag THEN 'x.id AS x,y.id AS y,' ELSE '' END||
 'z.id AS endpoint)) gt'
$$;

-- A batch-size change must affect costing only for the shapes the executor
-- can batch. Check actual array probes, not only a cheaper estimated cost.
DO $$
DECLARE shape text; incoming boolean; bag boolean; q text; a jsonb; b jsonb;
        expected jsonb; actual jsonb; heap_cost numeric;
BEGIN
 FOREACH shape IN ARRAY ARRAY['plain','include','multikey','descending'] LOOP
  EXECUTE 'CREATE INDEX e_out ON e'||CASE shape
   WHEN 'plain' THEN '(src)' WHEN 'include' THEN '(src) INCLUDE(dst,id)'
   WHEN 'multikey' THEN '(src,dst,id)' ELSE '(src DESC) INCLUDE(dst,id)' END;
  EXECUTE 'CREATE INDEX e_in ON e'||CASE shape
   WHEN 'plain' THEN '(dst)' WHEN 'include' THEN '(dst) INCLUDE(src,id)'
   WHEN 'multikey' THEN '(dst,src,id)' ELSE '(dst DESC) INCLUDE(src,id)' END;
  FOREACH incoming IN ARRAY ARRAY[false,true] LOOP
   q:=pg_temp.index_query(incoming);
   PERFORM set_config('test_graph_core.endpoint_batch_size','1',true);
   a:=pg_temp.index_graph(q);
   PERFORM set_config('test_graph_core.endpoint_batch_size','16',true);
   b:=pg_temp.index_graph(q,true);
   IF shape IN ('multikey','descending') THEN
    IF a->>'Total Cost' IS DISTINCT FROM b->>'Total Cost' THEN
     RAISE EXCEPTION 'scalar reader priced as array: %, %, %, %',shape,incoming,a,b;
    END IF;
    IF b ? 'Stage 2 Array Probes' THEN
     RAISE EXCEPTION 'scalar index unexpectedly batched: %',b;
    END IF;
   ELSIF coalesce(b->>'Stage 2 Array Probes','') NOT LIKE 'edge_batches=%' THEN
    RAISE EXCEPTION 'eligible index did not batch: %, %',shape,b;
   END IF;
   PERFORM set_config('test_graph_core.edge_index_only','off',true);
   heap_cost:=(pg_temp.index_graph(q)->>'Total Cost')::numeric;
   PERFORM set_config('test_graph_core.edge_index_only','on',true);
   IF shape<>'plain' AND (b->>'Total Cost')::numeric>=heap_cost THEN
    RAISE EXCEPTION 'covered all-visible reader did not price VM benefit: %, %',b,heap_cost;
   END IF;
   FOREACH bag IN ARRAY ARRAY[false,true] LOOP
    q:=pg_temp.index_query(incoming,bag);
    expected:=pg_temp.index_bag('/*+ GraphRoute(relational) */ '||q);
    actual:=pg_temp.index_bag('/*+ GraphRoute(btree) */ '||q);
    IF actual IS DISTINCT FROM expected THEN
     RAISE EXCEPTION 'index shape changed result multiplicity: %, %, %',shape,incoming,bag;
    END IF;
   END LOOP;
  END LOOP;
  DROP INDEX e_out;
  DROP INDEX e_in;
 END LOOP;
END $$;

-- Equal semantics, different physical widths, in BOTH creation orders.
-- 1KB non-compressible included values make the wider path strictly dearer;
-- a 256-byte fixture gives the singleton restriction equal one-page costs,
-- in which case choosing either index is correct and cannot test ranking.
-- The selected edge index is observed through transaction-local scan counts;
-- opening/costing an index does not increment this counter. Vertex identity
-- lookup likewise must not choose the first, wider unique index.
DO $$
DECLARE wide_first boolean; incoming boolean; before_wide bigint; before_narrow bigint;
        vertex_wide bigint; p jsonb; wide_name regclass; narrow_name regclass;
BEGIN
 FOREACH wide_first IN ARRAY ARRAY[true,false] LOOP
  IF wide_first THEN
   CREATE INDEX e_wide_out ON e(src) INCLUDE(dst,id,pad);
   CREATE INDEX e_wide_in ON e(dst) INCLUDE(src,id,pad);
  END IF;
  CREATE INDEX e_narrow_out ON e(src) INCLUDE(dst,id);
  CREATE INDEX e_narrow_in ON e(dst) INCLUDE(src,id);
  IF NOT wide_first THEN
   CREATE INDEX e_wide_out ON e(src) INCLUDE(dst,id,pad);
   CREATE INDEX e_wide_in ON e(dst) INCLUDE(src,id,pad);
  END IF;
  FOREACH incoming IN ARRAY ARRAY[false,true] LOOP
   wide_name:=CASE WHEN incoming THEN 'e_wide_in' ELSE 'e_wide_out' END::regclass;
   narrow_name:=CASE WHEN incoming THEN 'e_narrow_in' ELSE 'e_narrow_out' END::regclass;
   before_wide:=pg_stat_get_xact_numscans(wide_name);
   before_narrow:=pg_stat_get_xact_numscans(narrow_name);
   vertex_wide:=pg_stat_get_xact_numscans('v_wide'::regclass);
   p:=pg_temp.index_graph(pg_temp.index_query(incoming),true);
   IF pg_stat_get_xact_numscans(wide_name)<>before_wide OR
      pg_stat_get_xact_numscans(narrow_name)<=before_narrow OR
      pg_stat_get_xact_numscans('v_wide'::regclass)<>vertex_wide THEN
    RAISE EXCEPTION 'index choice follows OID instead of cost: order=%, incoming=%, wide=%->%, narrow=%->%, vertexwide=%->%, plan=%',
     wide_first,incoming,before_wide,pg_stat_get_xact_numscans(wide_name),
     before_narrow,pg_stat_get_xact_numscans(narrow_name),
     vertex_wide,pg_stat_get_xact_numscans('v_wide'::regclass),p;
   END IF;
  END LOOP;
  -- A cached plan must replan when its selected index disappears.
  EXECUTE 'PREPARE graph_chosen_index AS /*+ GraphRoute(btree) */ '||pg_temp.index_query(false);
  p:=pg_temp.index_graph('EXECUTE graph_chosen_index',true);
  DROP INDEX e_narrow_out;
  p:=pg_temp.index_graph('EXECUTE graph_chosen_index',true);
  DEALLOCATE graph_chosen_index;
  DROP INDEX e_narrow_in;
  DROP INDEX e_wide_out;
  DROP INDEX e_wide_in;
 END LOOP;
END $$;
RESET test_graph_core.vertex_probe_batch;
-- Correct blocking startup costs must not leave a redundant Unique merely
-- because a sorted alternative wins PG's fuzzy path-cost comparison.  Keep
-- the actual Sort/order, and do not apply singleton proofs to multiple roots
-- or to unrelated AUTO/relational candidates.
CREATE TABLE u_v(id bigint PRIMARY KEY);
CREATE TABLE u_e(id bigint PRIMARY KEY,src bigint NOT NULL,dst bigint NOT NULL);
INSERT INTO u_v VALUES(1),(2),(3),(4);
INSERT INTO u_e VALUES(11,1,2),(12,1,2),(13,2,3),(14,3,1),(15,1,99),
 (16,99,4),(17,2,2),(18,2,98),(19,3,4),(20,4,3);
CREATE INDEX u_out ON u_e(src);
CREATE PROPERTY GRAPH u_g VERTEX TABLES(u_v KEY(id)) EDGE TABLES(u_e KEY(id)
 SOURCE KEY(src) REFERENCES u_v(id) DESTINATION KEY(dst) REFERENCES u_v(id));
VACUUM(FREEZE,ANALYZE) u_v;
DO $$
DECLARE
 q text=$query$SELECT DISTINCT endpoint FROM GRAPH_TABLE(u_g MATCH
 (a IS u_v WHERE a.id=1::bigint)-[x IS u_e]->(b IS u_v)-[y IS u_e]->(c IS u_v)
 COLUMNS(c.id AS endpoint)) gt$query$;
 sql text; p jsonb; duplicate_removal boolean; ordered boolean;
 auto_route boolean; singleton boolean; actual bigint[]; expected bigint[]; value bigint;
BEGIN
 FOREACH auto_route IN ARRAY ARRAY[false,true] LOOP
  PERFORM set_config('enable_graph_btree',auto_route::text,true);
  FOREACH singleton IN ARRAY ARRAY[true,false] LOOP
   FOREACH ordered IN ARRAY ARRAY[false,true] LOOP
    sql:=CASE WHEN singleton THEN q ELSE replace(q,' WHERE a.id=1::bigint','') END||
      CASE WHEN ordered THEN ' ORDER BY endpoint DESC' ELSE '' END;
    expected:='{}'::bigint[];
    FOR value IN EXECUTE '/*+ GraphRoute(relational) */ '||sql LOOP
      expected:=array_append(expected,value);
    END LOOP;
    sql:=CASE WHEN auto_route THEN '' ELSE '/*+ GraphRoute(btree) */ ' END||sql;
    EXECUTE 'EXPLAIN(FORMAT JSON) '||sql INTO p;
    WITH RECURSIVE plans(n) AS (SELECT p->0->'Plan' UNION ALL
      SELECT c FROM plans CROSS JOIN LATERAL jsonb_array_elements(n->'Plans') c)
    SELECT bool_or(n->>'Node Type' IN ('Aggregate','Unique')) INTO duplicate_removal FROM plans;
    IF NOT auto_route AND singleton AND duplicate_removal THEN
      RAISE EXCEPTION 'redundant DISTINCT survived singleton proof: %',p;
    END IF;
    IF NOT singleton AND NOT duplicate_removal THEN
      RAISE EXCEPTION 'singleton proof leaked across multiple anchors: %',p;
    END IF;
    -- Keep the route hint at the outermost statement: wrapping it in another
    -- SELECT would test hint propagation instead of this path transformation.
    actual:='{}'::bigint[];
    FOR value IN EXECUTE sql LOOP
      actual:=array_append(actual,value);
    END LOOP;
    IF ordered THEN
      IF actual IS DISTINCT FROM expected THEN RAISE EXCEPTION 'ordered result changed'; END IF;
    ELSE
      IF (SELECT array_agg(v ORDER BY v) FROM unnest(actual) v) IS DISTINCT FROM
         (SELECT array_agg(v ORDER BY v) FROM unnest(expected) v) THEN
        RAISE EXCEPTION 'distinct result changed'; END IF;
    END IF;
   END LOOP;
  END LOOP;
 END LOOP;
 PERFORM set_config('enable_graph_btree','off',true);
END $$;
-- A broad frontier has fractional graph estimates before Path construction.
-- PG rounds the same input in estimate_num_groups().  Preserve the ordinary
-- Path row-count convention so a redundant hash DISTINCT cannot win a fuzzy
-- cost tie merely by having fewer fractional estimated rows.
CREATE TABLE f_v(id bigint PRIMARY KEY);
CREATE TABLE f_e(id bigint PRIMARY KEY,src bigint NOT NULL,dst bigint NOT NULL);
INSERT INTO f_e
 SELECT i,1,i+1 FROM generate_series(1,4096) s(i)
 UNION ALL SELECT i+4096,i+1,i+1 FROM generate_series(1,4096) s(i)
 UNION ALL SELECT i,i+1000000,i+1000001 FROM generate_series(8193,100000) s(i);
INSERT INTO f_v SELECT src FROM f_e UNION SELECT dst FROM f_e;
CREATE INDEX f_out ON f_e(src);
CREATE PROPERTY GRAPH f_g VERTEX TABLES(f_v KEY(id)) EDGE TABLES(f_e KEY(id)
 SOURCE KEY(src) REFERENCES f_v(id) DESTINATION KEY(dst) REFERENCES f_v(id));
VACUUM(FREEZE,ANALYZE) f_v;
VACUUM(FREEZE,ANALYZE) f_e;
DO $$
DECLARE
 q text=$query$SELECT DISTINCT endpoint FROM GRAPH_TABLE(f_g MATCH
 (a IS f_v WHERE a.id=1::bigint)-[x1 IS f_e]->(b IS f_v)
 -[x2 IS f_e]->(c IS f_v)-[x3 IS f_e]->(d IS f_v)
 -[x4 IS f_e]->(e IS f_v)-[x5 IS f_e]->(f IS f_v)
 -[x6 IS f_e]->(g IS f_v)-[x7 IS f_e]->(h IS f_v)
 -[x8 IS f_e]->(i IS f_v) COLUMNS(i.id AS endpoint)) gt$query$;
 p jsonb; duplicate_removal boolean; hash_enabled boolean;
 expected bigint[]; actual bigint[]; value bigint;
BEGIN
 SELECT array_agg(i::bigint ORDER BY i) INTO expected FROM generate_series(2,4097) s(i);
 FOREACH hash_enabled IN ARRAY ARRAY[true,false] LOOP
  PERFORM set_config('enable_hashagg',hash_enabled::text,true);
  EXECUTE 'EXPLAIN(FORMAT JSON) /*+ GraphRoute(btree) */ '||q INTO p;
  WITH RECURSIVE plans(n) AS (SELECT p->0->'Plan' UNION ALL
    SELECT c FROM plans CROSS JOIN LATERAL jsonb_array_elements(n->'Plans') c)
  SELECT bool_or(n->>'Node Type' IN ('Aggregate','Unique')) INTO duplicate_removal FROM plans;
  IF duplicate_removal THEN
   RAISE EXCEPTION 'fractional estimate kept redundant DISTINCT: %',p;
  END IF;
  actual:='{}'::bigint[];
  FOR value IN EXECUTE '/*+ GraphRoute(btree) */ '||q LOOP
   actual:=array_append(actual,value);
  END LOOP;
  IF (SELECT array_agg(v ORDER BY v) FROM unnest(actual) v) IS DISTINCT FROM expected THEN
   RAISE EXCEPTION 'wide frontier result changed';
  END IF;
 END LOOP;
END $$;
-- Equal physical column statistics do not imply equal alias estimates: a
-- partial unique-index proof can hold for only some edge aliases. The native
-- CASSERT recomputation checks both the scalar selectivity and full cost.
CREATE TABLE proof_v(id bigint PRIMARY KEY);
CREATE TABLE proof_e(id bigint PRIMARY KEY,src bigint NOT NULL,dst bigint NOT NULL,flag integer NOT NULL);
INSERT INTO proof_v SELECT i FROM generate_series(1,1000) i;
INSERT INTO proof_e SELECT i,i,i%1000+1,1 FROM generate_series(1,1000) i;
CREATE INDEX proof_out ON proof_e(src);
CREATE UNIQUE INDEX proof_partial ON proof_e(src) WHERE flag=1;
ALTER TABLE proof_e ALTER COLUMN src SET (n_distinct=100);
CREATE PROPERTY GRAPH proof_g VERTEX TABLES(proof_v KEY(id)) EDGE TABLES(proof_e KEY(id)
 SOURCE KEY(src) REFERENCES proof_v(id) DESTINATION KEY(dst) REFERENCES proof_v(id)
 PROPERTIES(id,flag));
ANALYZE proof_v;
ANALYZE proof_e;
DO $$
DECLARE q text=$query$SELECT DISTINCT endpoint FROM GRAPH_TABLE(proof_g MATCH
 (a IS proof_v WHERE a.id=1::bigint)-[x1 IS proof_e WHERE x1.flag>=0]->(b IS proof_v)
 -[x2 IS proof_e WHERE x2.flag=1]->(c IS proof_v)
 -[x3 IS proof_e WHERE x3.flag>=0]->(d IS proof_v)
 -[x4 IS proof_e WHERE x4.flag=1]->(z IS proof_v)
 COLUMNS(z.id AS endpoint)) gt$query$;
BEGIN
 PERFORM pg_temp.index_graph(q,true);
 IF pg_temp.index_bag('/*+ GraphRoute(btree) */ '||q) IS DISTINCT FROM
    pg_temp.index_bag('/*+ GraphRoute(relational) */ '||q) THEN
  RAISE EXCEPTION 'alias uniqueness proof changed graph results';
 END IF;
END $$;
RESET graph_btree_max_candidates;
RESET jit;
RESET search_path;
DROP FUNCTION pg_temp.index_bag(text), pg_temp.index_graph(text,boolean),
 pg_temp.index_query(boolean,boolean);
RESET ROLE;
DROP SCHEMA ggc_index_cost CASCADE;
DROP ROLE ggc_index_cost_role;
\echo graph index capabilities and costed choices: ok
