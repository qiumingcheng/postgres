\set ECHO none
SET client_min_messages=warning;
LOAD 'test_graph_core';
SET jit=off;
CREATE SCHEMA ggc_regions;
SET search_path=ggc_regions,public;
CREATE TABLE v(id bigint PRIMARY KEY,name text);
CREATE TABLE e(id bigint PRIMARY KEY,src bigint NOT NULL,dst bigint NOT NULL,payload text);
INSERT INTO v VALUES(1,'one'),(2,'two'),(3,'three'),(4,'four');
INSERT INTO e VALUES(11,1,2,'a'),(12,1,2,'b'),(13,2,3,'c'),(14,2,4,'d'),(15,4,4,'self');
CREATE INDEX e_out ON e(src);
CREATE INDEX e_in ON e(dst);
CREATE PROPERTY GRAPH g VERTEX TABLES(v KEY(id)) EDGE TABLES(e KEY(id)
 SOURCE KEY(src) REFERENCES v(id) DESTINATION KEY(dst) REFERENCES v(id));
CREATE TABLE v2(id bigint PRIMARY KEY,name text);
CREATE TABLE e2(id bigint PRIMARY KEY,src bigint NOT NULL,dst bigint NOT NULL,payload text);
INSERT INTO v2 VALUES(2,'other-two'),(3,'other-three'),(4,'other-four'),(5,'other-five');
INSERT INTO e2 VALUES(21,2,3,'A'),(22,2,3,'B'),(23,3,5,'C'),(24,4,3,'D'),(25,3,3,'S');
CREATE INDEX e2_in ON e2(dst);
CREATE INDEX e2_out ON e2(src);
CREATE PROPERTY GRAPH g2 VERTEX TABLES(v2 KEY(id)) EDGE TABLES(e2 KEY(id)
 SOURCE KEY(src) REFERENCES v2(id) DESTINATION KEY(dst) REFERENCES v2(id));
ANALYZE v;
ANALYZE e;
ANALYZE v2;
ANALYZE e2;

CREATE FUNCTION pg_temp.region_bag(q text) RETURNS jsonb LANGUAGE plpgsql AS $$
DECLARE r record; bag jsonb='[]';
BEGIN
 FOR r IN EXECUTE q LOOP bag:=bag||jsonb_build_array(to_jsonb(r)); END LOOP;
 RETURN (SELECT coalesce(jsonb_agg(value ORDER BY value),'[]'::jsonb)
         FROM jsonb_array_elements(bag));
END $$;
CREATE FUNCTION pg_temp.region_count(q text) RETURNS int LANGUAGE plpgsql AS $$
DECLARE p jsonb; n int;
BEGIN
 EXECUTE 'EXPLAIN(FORMAT JSON) '||q INTO p;
 WITH RECURSIVE plans(x) AS (SELECT p->0->'Plan' UNION ALL
   SELECT c FROM plans CROSS JOIN LATERAL jsonb_array_elements(x->'Plans') c)
 SELECT count(*) INTO n FROM plans WHERE x->>'Custom Plan Provider'='GraphExpandFixed';
 RETURN n;
END $$;
CREATE FUNCTION pg_temp.region_check(q text,oracle text,expected_graphs int) RETURNS void LANGUAGE plpgsql AS $$
DECLARE mode text; hint text; n int; want jsonb; actual jsonb;
BEGIN
 want:=pg_temp.region_bag(oracle);
 FOREACH mode IN ARRAY ARRAY['relational','btree','auto'] LOOP
   hint:=CASE WHEN mode='auto' THEN '' ELSE '/*+ GraphRoute('||mode||') */ ' END;
   n:=pg_temp.region_count(hint||q);
   IF (mode='btree' AND n<>expected_graphs) OR (mode='relational' AND n<>0) THEN
     RAISE EXCEPTION 'region count: %, %, %',mode,n,q;
   END IF;
   actual:=pg_temp.region_bag(hint||q);
   IF actual IS DISTINCT FROM want THEN RAISE EXCEPTION 'region bag: %, %, %, %',mode,q,actual,want; END IF;
 END LOOP;
END $$;

DO $$
DECLARE first text=$q$GRAPH_TABLE(g MATCH (a IS v WHERE a.id=1::bigint)
 -[x IS e]->(b IS v)-[y IS e]->(c IS v)
 COLUMNS(x.id AS e1,y.id AS e2,c.id AS endpoint,b.name AS name))$q$;
 second text=$q$GRAPH_TABLE(g2 MATCH (a IS v2 WHERE a.id=3::bigint)
 -[x IS e2]-(b IS v2) COLUMNS(x.id AS eid,b.id AS endpoint,b.name AS name))$q$;
 first_native text=$q$(SELECT x.id AS e1,y.id AS e2,c.id AS endpoint,b.name
 FROM v a JOIN e x ON a.id=x.src JOIN v b ON b.id=x.dst
 JOIN e y ON b.id=y.src JOIN v c ON c.id=y.dst WHERE a.id=1)$q$;
 second_native text=$q$(SELECT x.id AS eid,b.id AS endpoint,b.name
 FROM v2 a JOIN e2 x ON true JOIN v2 b ON
 ((x.src=a.id AND x.dst=b.id) OR (x.dst=a.id AND x.src=b.id)) WHERE a.id=3)$q$;
 shape text; flag text; q text; oracle text;
BEGIN
 IF current_setting('enable_graph_btree')<>'off' THEN RAISE EXCEPTION 'automatic graph planning must default off'; END IF;
 FOREACH flag IN ARRAY ARRAY['off','on'] LOOP
   PERFORM set_config('enable_graph_btree',flag,true);
   FOREACH shape IN ARRAY ARRAY[
     'SELECT aa.e1,aa.e2,aa.endpoint AS left_endpoint,aa.name AS left_name,bb.eid,bb.endpoint AS right_endpoint,bb.name AS right_name FROM @A@ aa JOIN @B@ bb ON aa.endpoint=bb.endpoint',
     'SELECT aa.e1,aa.e2,aa.endpoint AS left_endpoint,aa.name AS left_name,bb.eid,bb.endpoint AS right_endpoint,bb.name AS right_name FROM @A@ aa LEFT JOIN @B@ bb ON aa.endpoint=bb.endpoint',
     'SELECT aa.e1,aa.e2,aa.endpoint AS left_endpoint,aa.name AS left_name,bb.eid,bb.endpoint AS right_endpoint,bb.name AS right_name FROM @A@ aa FULL JOIN @B@ bb ON aa.endpoint=bb.endpoint',
     'SELECT aa.e1,aa.e2,aa.endpoint AS left_endpoint,aa.name AS left_name,bb.eid,bb.endpoint AS right_endpoint,bb.name AS right_name FROM @A@ aa CROSS JOIN @B@ bb WHERE aa.e1+bb.eid>33',
     'SELECT aa.endpoint,count(*),max(bb.name) FROM @A@ aa JOIN @B@ bb ON aa.endpoint=bb.endpoint GROUP BY aa.endpoint',
     'SELECT aa.e1,bb.eid FROM @A@ aa CROSS JOIN @B@ bb ORDER BY aa.e1,aa.e2,bb.eid LIMIT 4',
     'SELECT endpoint,name FROM @A@ UNION ALL SELECT endpoint,name FROM @B@',
     'SELECT endpoint,name FROM @A@ UNION SELECT endpoint,name FROM @B@'
   ] LOOP
     q:=replace(replace(shape,'@A@',first),'@B@',second);
     oracle:=replace(replace(shape,'@A@',first_native),'@B@',second_native);
     PERFORM pg_temp.region_check(q,oracle,2);
     IF flag='on' AND shape NOT LIKE '%UNION%' AND pg_temp.region_count(q)<>0 THEN
       RAISE EXCEPTION 'automatic mixed query must preserve unrestricted native planning';
     END IF;
   END LOOP;
 END LOOP;
 -- Independent identities/scopes survive rescanning an inner region.
 PERFORM set_config('enable_material','off',true);
 PERFORM set_config('enable_hashjoin','off',true);
 PERFORM set_config('enable_mergejoin','off',true);
 shape:='SELECT aa.e1,aa.e2,aa.endpoint AS left_endpoint,aa.name AS left_name,bb.eid,bb.endpoint AS right_endpoint,bb.name AS right_name FROM @A@ aa LEFT JOIN @B@ bb ON aa.endpoint=bb.endpoint';
 PERFORM pg_temp.region_check(replace(replace(shape,'@A@',first),'@B@',second),
   replace(replace(shape,'@A@',first_native),'@B@',second_native),2);
END $$;

-- Natural planning really compares costs; do not disable joins or modify costs.
CREATE TABLE cv(id bigint PRIMARY KEY);
CREATE TABLE ce(id bigint PRIMARY KEY,src bigint NOT NULL,dst bigint NOT NULL,payload text);
INSERT INTO cv SELECT generate_series(1,5);
INSERT INTO ce SELECT (a-1)*32+b,a,a+1,'edge' FROM generate_series(1,4) a,generate_series(1,32) b;
CREATE INDEX ce_out ON ce(src);
CREATE PROPERTY GRAPH cg VERTEX TABLES(cv KEY(id)) EDGE TABLES(ce KEY(id)
 SOURCE KEY(src) REFERENCES cv(id) DESTINATION KEY(dst) REFERENCES cv(id));
ANALYZE cv;
ANALYZE ce;
SET enable_graph_btree=on;
-- Earlier statistics tests publish a role-scoped synthetic cost profile.
-- A new role guarantees this check uses only PG/default fallback costs.
CREATE ROLE regress_ggc_regions_cost;
GRANT USAGE ON SCHEMA ggc_regions TO regress_ggc_regions_cost;
GRANT SELECT ON v,e,g,cv,ce,cg TO regress_ggc_regions_cost;
SET ROLE regress_ggc_regions_cost;
DO $$
DECLARE q text=$q$SELECT DISTINCT endpoint FROM GRAPH_TABLE(cg MATCH
 (a IS cv WHERE a.id=1::bigint)-[x1 IS ce]->(b IS cv)-[x2 IS ce]->(c IS cv)
 -[x3 IS ce]->(d IS cv)-[x4 IS ce]->(z IS cv) COLUMNS(z.id AS endpoint))$q$;
 tiny text=$q$SELECT * FROM GRAPH_TABLE(g MATCH (a IS v WHERE a.id=1::bigint)
 -[x IS e]->(b IS v) COLUMNS(b.id AS endpoint))$q$;
BEGIN
 IF NOT current_setting('enable_nestloop')::boolean OR
    NOT current_setting('enable_hashjoin')::boolean OR
    NOT current_setting('enable_mergejoin')::boolean OR
    current_setting('random_page_cost')<>'4' THEN RAISE EXCEPTION 'nonstandard CBO costs'; END IF;
 IF pg_temp.region_count(q)<>1 THEN RAISE EXCEPTION 'convergence graph candidate did not win naturally'; END IF;
 IF pg_temp.region_bag(q) IS DISTINCT FROM pg_temp.region_bag('/*+ GraphRoute(relational) */ '||q) THEN
   RAISE EXCEPTION 'natural graph result mismatch';
 END IF;
 IF pg_temp.region_count(tiny)<>0 THEN RAISE EXCEPTION 'tiny relational candidate did not win naturally'; END IF;
 PERFORM pg_temp.region_check(tiny,'SELECT b.id AS endpoint FROM v a JOIN e x ON a.id=x.src JOIN v b ON b.id=x.dst WHERE a.id=1',1);
END $$;
-- A delayed DISTINCT alternative belongs to this planner only. Single-anchor
-- uniqueness must not remove global DISTINCT for multiple anchors, an outer
-- COUNT, or a generic parameter. Ordering remains an ordinary PG responsibility.
DO $$
DECLARE q text; condition text; tail text; want jsonb;
BEGIN
 FOREACH condition IN ARRAY ARRAY['a.id=1::bigint','a.id>=1::bigint','a.id=99::bigint'] LOOP
   FOREACH tail IN ARRAY ARRAY['',' ORDER BY endpoint',' ORDER BY endpoint LIMIT 1'] LOOP
     q:='SELECT DISTINCT endpoint FROM GRAPH_TABLE(g MATCH (a IS v)-[x IS e]->(b IS v)-[y IS e]->(z IS v) WHERE '||condition||' COLUMNS(z.id AS endpoint)) gt'||tail;
     want:=pg_temp.region_bag('/*+ GraphRoute(relational) */ '||q);
     IF pg_temp.region_bag(q) IS DISTINCT FROM want THEN RAISE EXCEPTION 'AUTO DISTINCT contract: %',q; END IF;
   END LOOP;
 END LOOP;
 q:='SELECT count(*) FROM GRAPH_TABLE(g MATCH (a IS v WHERE a.id=1::bigint)-[x IS e]->(b IS v)-[y IS e]->(z IS v) COLUMNS(z.id AS endpoint)) gt';
 IF pg_temp.region_bag(q) IS DISTINCT FROM pg_temp.region_bag('/*+ GraphRoute(relational) */ '||q) THEN
   RAISE EXCEPTION 'AUTO must preserve bag count';
 END IF;
END $$;
PREPARE regions_distinct(bigint) AS SELECT DISTINCT endpoint FROM GRAPH_TABLE(g MATCH
 (a IS v WHERE a.id=$1)-[x IS e]->(b IS v)-[y IS e]->(z IS v) COLUMNS(z.id AS endpoint)) gt;
DO $$
DECLARE mode text; anchor int; q text;
BEGIN
 FOREACH mode IN ARRAY ARRAY['force_generic_plan','force_custom_plan'] LOOP
   PERFORM set_config('plan_cache_mode',mode,true);
   FOREACH anchor IN ARRAY ARRAY[1,2,99,1] LOOP
     q:=format('SELECT DISTINCT z.id AS endpoint FROM v a JOIN e x ON x.src=a.id JOIN v b ON b.id=x.dst JOIN e y ON y.src=b.id JOIN v z ON z.id=y.dst WHERE a.id=%s',anchor);
     IF pg_temp.region_bag(format('EXECUTE regions_distinct(%s)',anchor)) IS DISTINCT FROM pg_temp.region_bag(q) THEN
       RAISE EXCEPTION 'AUTO prepared DISTINCT contract';
     END IF;
   END LOOP;
 END LOOP;
END $$;
DEALLOCATE regions_distinct;
RESET plan_cache_mode;
RESET ROLE;
DROP OWNED BY regress_ggc_regions_cost;
DROP ROLE regress_ggc_regions_cost;

-- Missing access or unsupported internal semantics retain the native plan.
DROP INDEX e2_out;
DO $$
DECLARE q text=$q$SELECT * FROM GRAPH_TABLE(g2 MATCH (a IS v2)-[x IS e2]-(b IS v2)
 COLUMNS(x.id AS eid,b.id AS endpoint))$q$;
 p jsonb;
BEGIN
 IF pg_temp.region_count(q)<>0 OR pg_temp.region_bag(q) IS DISTINCT FROM
    pg_temp.region_bag('/*+ GraphRoute(relational) */ '||q) THEN RAISE EXCEPTION 'missing index fallback'; END IF;
 IF pg_temp.region_bag('/*+ GraphRoute(btree) */ '||q) IS DISTINCT FROM
    pg_temp.region_bag('/*+ GraphRoute(relational) */ '||q) THEN
   RAISE EXCEPTION 'typed missing-index bag mismatch'; END IF;
 EXECUTE 'EXPLAIN(FORMAT JSON) /*+ GraphRoute(btree) */ '||q INTO p;
 IF p::text NOT LIKE '%GraphExpandNative%' THEN RAISE EXCEPTION 'missing typed stages'; END IF;
END $$;
CREATE INDEX e2_out ON e2(src);
DO $$
DECLARE q text=$q$SELECT * FROM GRAPH_TABLE(g MATCH
 (a IS v)-[x IS e WHERE random()>=0]->(b IS v) COLUMNS(x.id AS eid))$q$;
BEGIN
 IF pg_temp.region_count(q)<>0 THEN RAISE EXCEPTION 'volatile internal qual entered graph'; END IF;
END $$;

-- Different scopes in a cached plan, a DDL invalidation and repeated parameters.
SET plan_cache_mode=force_generic_plan;
PREPARE regions(bigint) AS /*+ GraphRoute(btree) */ SELECT aa.eid,bb.eid AS other
 FROM GRAPH_TABLE(g MATCH (a IS v WHERE a.id=$1)-[x IS e]->(b IS v)
 COLUMNS(x.id AS eid,b.id AS endpoint)) aa JOIN
 GRAPH_TABLE(g2 MATCH (a IS v2 WHERE a.id=3::bigint)<-[x IS e2]-(b IS v2)
 COLUMNS(x.id AS eid,b.id AS endpoint)) bb ON aa.endpoint=bb.endpoint;
DO $$
DECLARE anchor int;
BEGIN
 FOREACH anchor IN ARRAY ARRAY[1,2,4,1] LOOP
   IF pg_temp.region_count(format('EXECUTE regions(%s)',anchor))<>2 OR
      pg_temp.region_bag(format('EXECUTE regions(%s)',anchor)) IS DISTINCT FROM
      pg_temp.region_bag(format('SELECT x.id AS eid,y.id AS other FROM v a JOIN e x ON a.id=x.src JOIN v b ON b.id=x.dst JOIN v2 c ON c.id=b.id JOIN e2 y ON y.src=c.id JOIN v2 d ON d.id=y.dst WHERE a.id=%s AND d.id=3',anchor)) THEN
     RAISE EXCEPTION 'cached regions mismatch';
   END IF;
 END LOOP;
END $$;
REINDEX INDEX e2_in;
DO $$ BEGIN IF pg_temp.region_count('EXECUTE regions(1)')<>2 THEN RAISE EXCEPTION 'cached region DDL'; END IF; END $$;
DEALLOCATE regions;
RESET plan_cache_mode;
RESET enable_graph_btree;
RESET jit;
RESET search_path;
DROP SCHEMA ggc_regions CASCADE;
\echo graph regions and natural planning: ok
