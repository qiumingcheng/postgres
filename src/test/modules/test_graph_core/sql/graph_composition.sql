\set ECHO none
SET client_min_messages=warning;
LOAD 'test_graph_core';
SET jit=off;
CREATE SCHEMA ggc_composition;
SET search_path=ggc_composition,public;
CREATE TABLE v(id bigint PRIMARY KEY);
CREATE TABLE e(id bigint PRIMARY KEY,src bigint NOT NULL,dst bigint NOT NULL,payload text);
INSERT INTO v VALUES(1),(2),(3),(4);
INSERT INTO e VALUES(11,1,2,'a'),(12,1,2,NULL),(13,2,3,'b'),(14,3,1,'c'),
 (15,1,99,'missing'),(16,99,2,'missing'),(17,2,2,'self');
CREATE INDEX e_btree ON e(src);
CREATE PROPERTY GRAPH g VERTEX TABLES(v KEY(id)) EDGE TABLES(e KEY(id)
 SOURCE KEY(src) REFERENCES v(id) DESTINATION KEY(dst) REFERENCES v(id));
CREATE TABLE business(vertex_id bigint,category text,enabled boolean);
INSERT INTO business VALUES(2,'two',true),(3,'three-a',true),(3,'three-b',true),
 (4,'no-match',true),(NULL,'null-key',true),(3,'disabled',false);
ANALYZE v;
ANALYZE e;
ANALYZE business;

CREATE FUNCTION pg_temp.composition_bag(q text) RETURNS jsonb LANGUAGE plpgsql AS $$
DECLARE r record; bag jsonb='[]';
BEGIN
 FOR r IN EXECUTE q LOOP bag:=bag||jsonb_build_array(to_jsonb(r)); END LOOP;
 RETURN (SELECT coalesce(jsonb_agg(value ORDER BY value),'[]'::jsonb)
         FROM jsonb_array_elements(bag));
END $$;

CREATE FUNCTION pg_temp.composition_check(q text, oracle text,
                                         required_node text DEFAULT NULL) RETURNS void
LANGUAGE plpgsql AS $$
DECLARE mode text; p jsonb; wanted jsonb; actual jsonb; graphs int; node_count int;
        before_flags text[]; after_flags text[];
BEGIN
 wanted:=pg_temp.composition_bag(oracle);
 FOREACH mode IN ARRAY ARRAY['relational','btree'] LOOP
   before_flags:=ARRAY[current_setting('enable_nestloop'),current_setting('enable_hashjoin'),
                       current_setting('enable_mergejoin'),current_setting('geqo')];
   EXECUTE 'EXPLAIN (ANALYZE,TIMING OFF,FORMAT JSON) /*+ GraphRoute('||mode||') */ '||q INTO p;
   WITH RECURSIVE plans(n) AS (
     SELECT p->0->'Plan' UNION ALL
     SELECT x FROM plans CROSS JOIN LATERAL jsonb_array_elements(n->'Plans') x
   ) SELECT count(*) FILTER(WHERE n->>'Custom Plan Provider'='GraphExpandFixed'),
            count(*) FILTER(WHERE n->>'Node Type'=required_node)
      INTO graphs,node_count FROM plans;
   IF graphs<>(CASE WHEN mode='btree' THEN 1 ELSE 0 END) OR
      p->0->>'Graph Route' IS DISTINCT FROM mode OR
      (required_node IS NOT NULL AND node_count=0) THEN
     RAISE EXCEPTION 'composition plan failed: %, %',q,p;
   END IF;
   actual:=pg_temp.composition_bag('/*+ GraphRoute('||mode||') */ '||q);
   IF actual IS DISTINCT FROM wanted THEN
     RAISE EXCEPTION 'composition bag mismatch: %, got %, wanted %',q,actual,wanted;
   END IF;
   after_flags:=ARRAY[current_setting('enable_nestloop'),current_setting('enable_hashjoin'),
                      current_setting('enable_mergejoin'),current_setting('geqo')];
   IF after_flags<>before_flags THEN RAISE EXCEPTION 'composition GUC leak'; END IF;
 END LOOP;
END $$;

DO $$
DECLARE graph text=$q$GRAPH_TABLE(g MATCH (a IS v WHERE a.id=1::bigint)
 -[x IS e]->(b IS v)-[y IS e]->(c IS v)
 COLUMNS(x.id AS e1,y.id AS e2,c.id AS endpoint,x.payload AS payload))$q$;
 oracle text=$q$(SELECT x.id AS e1,y.id AS e2,c.id AS endpoint,x.payload
 FROM v a JOIN e x ON a.id=x.src JOIN v b ON b.id=x.dst
 JOIN e y ON b.id=y.src JOIN v c ON c.id=y.dst WHERE a.id=1::bigint)$q$;
 pattern text; join_type text; i int;
BEGIN
 FOREACH pattern IN ARRAY ARRAY[
   'SELECT gt.*,t.category FROM @REGION@ gt JOIN business t ON t.vertex_id=gt.endpoint WHERE t.enabled',
   'SELECT gt.*,t.category FROM @REGION@ gt JOIN business t ON t.vertex_id=gt.endpoint WHERE t.enabled AND gt.e2=13',
   'SELECT gt.*,t.category FROM @REGION@ gt JOIN business t ON t.vertex_id=gt.endpoint WHERE gt.e2=-1',
   'SELECT t.category,count(*) FROM @REGION@ gt JOIN business t ON t.vertex_id=gt.endpoint GROUP BY t.category ORDER BY t.category',
   'SELECT DISTINCT gt.endpoint FROM @REGION@ gt JOIN business t ON t.vertex_id=gt.endpoint',
   'SELECT t.category,gt.* FROM business t LEFT JOIN @REGION@ gt ON t.vertex_id=gt.endpoint AND gt.e2=13 WHERE t.enabled',
   'SELECT t.category,gt.* FROM business t RIGHT JOIN @REGION@ gt ON t.vertex_id=gt.endpoint AND t.enabled',
   'SELECT t.category,gt.* FROM business t FULL JOIN @REGION@ gt ON t.vertex_id=gt.endpoint AND t.enabled',
   'SELECT gt.*,t.category FROM @REGION@ gt CROSS JOIN business t',
   'SELECT gt.endpoint+10 AS value,coalesce(gt.payload,t.category) AS label,pg_typeof(gt.endpoint)::text AS typ FROM @REGION@ gt JOIN business t ON t.vertex_id=gt.endpoint',
   'SELECT gt.*,t.category FROM @REGION@ gt JOIN business t ON t.vertex_id=gt.endpoint ORDER BY e1,e2,category LIMIT 3',
   'SELECT gt.*,t.category FROM (SELECT * FROM @REGION@) gt JOIN business t ON t.vertex_id=gt.endpoint',
   'SELECT count(*) FROM @REGION@ gt CROSS JOIN business t'
 ] LOOP
   PERFORM pg_temp.composition_check(replace(pattern,'@REGION@',graph),
                                    replace(pattern,'@REGION@',oracle));
 END LOOP;
 -- Keep multi-anchor path multiplicity, including cycles and a self loop.
 pattern:='SELECT DISTINCT gt.endpoint,t.category FROM @REGION@ gt JOIN business t ON t.vertex_id=gt.endpoint';
 PERFORM pg_temp.composition_check(
   replace(pattern,'@REGION@',replace(graph,' WHERE a.id=1::bigint','')),
   replace(pattern,'@REGION@',replace(oracle,' WHERE a.id=1::bigint','')));
 -- Force each ordinary join implementation outside the graph boundary.
 FOR i IN 1..3 LOOP
   PERFORM set_config('enable_nestloop',CASE WHEN i=1 THEN 'on' ELSE 'off' END,true);
   PERFORM set_config('enable_hashjoin',CASE WHEN i=2 THEN 'on' ELSE 'off' END,true);
   PERFORM set_config('enable_mergejoin',CASE WHEN i=3 THEN 'on' ELSE 'off' END,true);
   join_type:=CASE i WHEN 1 THEN 'Nested Loop' WHEN 2 THEN 'Hash Join' ELSE 'Merge Join' END;
   pattern:='SELECT gt.*,t.category FROM @REGION@ gt JOIN business t ON t.vertex_id=gt.endpoint';
   PERFORM pg_temp.composition_check(replace(pattern,'@REGION@',graph),
                                    replace(pattern,'@REGION@',oracle),join_type);
 END LOOP;
END $$;

-- A partitioned business table contributes Append, not another graph region.
CREATE TABLE business_parts (LIKE business) PARTITION BY RANGE(vertex_id);
CREATE TABLE business_low PARTITION OF business_parts FOR VALUES FROM (0) TO (3);
CREATE TABLE business_high PARTITION OF business_parts FOR VALUES FROM (3) TO (10);
CREATE TABLE business_null PARTITION OF business_parts DEFAULT;
INSERT INTO business_parts SELECT * FROM business;
ANALYZE business_parts;
DO $$
DECLARE q text=$q$SELECT gt.endpoint,t.category FROM GRAPH_TABLE(g MATCH
 (a IS v WHERE a.id=1::bigint)-[x IS e]->(b IS v)
 COLUMNS(b.id AS endpoint)) gt JOIN business_parts t ON t.vertex_id=gt.endpoint$q$;
 oracle text=$q$SELECT b.id AS endpoint,t.category FROM v a JOIN e x ON a.id=x.src
 JOIN v b ON b.id=x.dst JOIN business_parts t ON t.vertex_id=b.id WHERE a.id=1::bigint$q$;
BEGIN
 PERFORM pg_temp.composition_check(q,oracle,'Append');
END $$;

-- The graph is the inner side of a real repeated scan, not just Materialize.
DO $$
DECLARE q text=$q$SELECT t.category,gt.endpoint FROM business t LEFT JOIN
 GRAPH_TABLE(g MATCH (a IS v WHERE a.id=1::bigint)-[x IS e]->(b IS v)
 COLUMNS(b.id AS endpoint)) gt ON t.vertex_id=gt.endpoint$q$;
 oracle text=$q$SELECT t.category,gt.endpoint FROM business t LEFT JOIN
 (SELECT b.id AS endpoint FROM v a JOIN e x ON a.id=x.src JOIN v b ON b.id=x.dst
 WHERE a.id=1::bigint) gt ON t.vertex_id=gt.endpoint$q$;
 p jsonb; loops int; rescans int;
BEGIN
 PERFORM set_config('enable_material','off',true);
 PERFORM set_config('enable_nestloop','on',true);
 PERFORM set_config('enable_hashjoin','off',true);
 PERFORM set_config('enable_mergejoin','off',true);
 PERFORM pg_temp.composition_check(q,oracle,'Nested Loop');
 EXECUTE 'EXPLAIN(ANALYZE,TIMING OFF,FORMAT JSON) /*+ GraphRoute(btree) */ '||q INTO p;
 WITH RECURSIVE plans(n) AS (
   SELECT p->0->'Plan' UNION ALL
   SELECT x FROM plans CROSS JOIN LATERAL jsonb_array_elements(n->'Plans') x
 ) SELECT (n->>'Actual Loops')::int,(n->>'Graph Rescans')::int INTO loops,rescans
   FROM plans WHERE n->>'Custom Plan Provider'='GraphExpandFixed';
 IF loops IS NULL OR loops<2 OR rescans IS NULL OR rescans<1 THEN
   RAISE EXCEPTION 'no real graph rescan: %',p;
 END IF;
END $$;

-- RLS on an outside business table is still enforced by ordinary PG nodes.
CREATE ROLE regress_ggc_composition_reader;
GRANT USAGE ON SCHEMA ggc_composition TO regress_ggc_composition_reader;
GRANT SELECT ON v,e,g,business TO regress_ggc_composition_reader;
ALTER TABLE business ENABLE ROW LEVEL SECURITY;
CREATE POLICY visible_business ON business USING(enabled AND category<>'three-b');
SET ROLE regress_ggc_composition_reader;
DO $$
DECLARE q text=$q$SELECT gt.endpoint,t.category FROM GRAPH_TABLE(g MATCH
 (a IS v WHERE a.id=1::bigint)-[x IS e]->(b IS v)-[y IS e]->(c IS v)
 COLUMNS(c.id AS endpoint)) gt JOIN business t ON t.vertex_id=gt.endpoint$q$;
 oracle text=$q$SELECT c.id AS endpoint,t.category FROM v a JOIN e x ON a.id=x.src
 JOIN v b ON b.id=x.dst JOIN e y ON b.id=y.src JOIN v c ON c.id=y.dst
 JOIN business t ON t.vertex_id=c.id WHERE a.id=1::bigint$q$;
BEGIN
 IF (SELECT count(*) FROM business)<>4 THEN RAISE EXCEPTION 'RLS fixture is not restricted'; END IF;
 PERFORM pg_temp.composition_check(q,oracle);
 IF jsonb_array_length(pg_temp.composition_bag('/*+ GraphRoute(btree) */ '||q))<>4 THEN
   RAISE EXCEPTION 'outside RLS leaked hidden business rows';
 END IF;
END $$;
RESET ROLE;
ALTER TABLE business DISABLE ROW LEVEL SECURITY;
REVOKE SELECT ON e FROM regress_ggc_composition_reader;
SET ROLE regress_ggc_composition_reader;
DO $$
DECLARE caught boolean=false;
BEGIN
 BEGIN
   EXECUTE 'SELECT /*+ GraphRoute(btree) */ gt.endpoint,t.category FROM GRAPH_TABLE(g '
    'MATCH (a IS v)-[x IS e]->(b IS v) COLUMNS(b.id AS endpoint)) gt '
    'JOIN business t ON t.vertex_id=gt.endpoint';
 EXCEPTION WHEN insufficient_privilege OR feature_not_supported THEN caught:=true;
 END;
 IF NOT caught THEN RAISE EXCEPTION 'edge SELECT permission bypassed'; END IF;
END $$;
RESET ROLE;
DROP OWNED BY regress_ggc_composition_reader;
DROP ROLE regress_ggc_composition_reader;

-- The relational route is independent of graph IR and accepts normal plan containers.
DO $$
DECLARE q text; expected jsonb; actual jsonb;
BEGIN
 FOREACH q IN ARRAY ARRAY[
   'SELECT b.id FROM GRAPH_TABLE(g MATCH (a IS v)-[x IS e]-(b IS v) COLUMNS(b.id)) b',
   'SELECT endpoint FROM GRAPH_TABLE(g MATCH (a IS v)<-[x IS e]-(b IS v) COLUMNS(b.id AS endpoint))',
   'SELECT * FROM business UNION ALL SELECT * FROM business',
   'WITH RECURSIVE r(n) AS (VALUES(1) UNION ALL SELECT n+1 FROM r WHERE n<3) SELECT * FROM r',
   'SELECT 1 AS value'
 ] LOOP
   expected:=pg_temp.composition_bag(q);
   actual:=pg_temp.composition_bag('/*+ GraphRoute(relational) */ '||q);
   IF actual IS DISTINCT FROM expected THEN RAISE EXCEPTION 'relational changed result: %',q; END IF;
 END LOOP;
END $$;

-- Parameterized outer restrictions remain outside the graph subquery.
PREPARE composition_cached(bigint) AS
 SELECT /*+ GraphRoute(btree) */ gt.e1,gt.e2,t.category
 FROM GRAPH_TABLE(g MATCH (a IS v WHERE a.id=1::bigint)-[x IS e]->(b IS v)-[y IS e]->(c IS v)
 COLUMNS(x.id AS e1,y.id AS e2,c.id AS endpoint)) gt
 JOIN business t ON t.vertex_id=gt.endpoint WHERE gt.e2=$1;
DO $$
DECLARE cache_mode text; q text; p jsonb; expected jsonb; actual jsonb;
BEGIN
 FOREACH cache_mode IN ARRAY ARRAY['force_custom_plan','force_generic_plan'] LOOP
   PERFORM set_config('plan_cache_mode',cache_mode,true);
   FOREACH q IN ARRAY ARRAY['EXECUTE composition_cached(13)','EXECUTE composition_cached(17)'] LOOP
     EXECUTE 'EXPLAIN(FORMAT JSON) '||q INTO p;
     IF p->0->>'Graph Route'<>'btree' OR p::text NOT LIKE '%GraphExpandFixed%' THEN
       RAISE EXCEPTION 'prepared graph boundary lost: %',p;
     END IF;
     expected:=pg_temp.composition_bag(
       'SELECT x.id AS e1,y.id AS e2,t.category FROM v a JOIN e x ON a.id=x.src '
       'JOIN v b ON b.id=x.dst JOIN e y ON b.id=y.src JOIN v c ON c.id=y.dst '
       'JOIN business t ON t.vertex_id=c.id WHERE a.id=1 AND y.id='||
       CASE WHEN q LIKE '%(13)' THEN '13' ELSE '17' END);
     actual:=pg_temp.composition_bag(q);
     IF actual IS DISTINCT FROM expected THEN RAISE EXCEPTION 'prepared bag mismatch'; END IF;
   END LOOP;
 END LOOP;
END $$;
REINDEX INDEX e_btree;
DO $$ BEGIN
 IF jsonb_array_length(pg_temp.composition_bag('EXECUTE composition_cached(13)'))<>6 THEN
   RAISE EXCEPTION 'REINDEX lost cached composition';
 END IF;
END $$;
DEALLOCATE composition_cached;

-- Failure must restore the statement hooks and leave the next query usable.
DO $$
DECLARE caught boolean=false; q text; before_plan jsonb; after_plan jsonb;
BEGIN
 q:='SELECT * FROM v a JOIN e x ON a.id=x.src JOIN v b ON b.id=x.dst';
 EXECUTE 'EXPLAIN(FORMAT JSON) '||q INTO before_plan;
 BEGIN
   EXECUTE 'SELECT /*+ GraphRoute(btree) */ gt.*,t.category FROM GRAPH_TABLE(g '
     'MATCH (a IS v)-[x IS e WHERE x.payload=''a'' AND 1/0>0]->(b IS v) '
     'COLUMNS(b.id AS endpoint)) gt JOIN business t ON t.vertex_id=gt.endpoint';
 EXCEPTION WHEN division_by_zero THEN caught:=true;
 END;
 IF NOT caught THEN RAISE EXCEPTION 'planner expression error was swallowed'; END IF;
 EXECUTE 'EXPLAIN(FORMAT JSON) '||q INTO after_plan;
 IF before_plan<>after_plan THEN RAISE EXCEPTION 'composition hook leaked after ERROR'; END IF;
END $$;
RESET search_path;
RESET jit;
DROP SCHEMA ggc_composition CASCADE;
