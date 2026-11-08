\set ECHO none
SET client_min_messages=warning;
LOAD 'test_graph_core';
SET jit=off;
CREATE SCHEMA ggc_directions;
SET search_path=ggc_directions,public;
CREATE TABLE v(id bigint PRIMARY KEY,name text,age integer);
CREATE TABLE e(id bigint PRIMARY KEY,src bigint NOT NULL,dst bigint NOT NULL,payload text,amount numeric);
INSERT INTO v VALUES(1,'a',30),(2,'b',20),(3,'c',40),(4,'d',NULL);
INSERT INTO e VALUES(11,1,2,'a',10),(12,1,2,NULL,2),(13,2,1,'reverse',4),
 (14,2,3,'b',8),(15,3,1,'c',9),(16,2,2,'self',NULL),(17,2,2,'parallel-self',5),
 (18,1,99,'missing',1),(19,99,2,'missing-in',1),(20,3,4,'tail',3);
CREATE INDEX e_out ON e(src) INCLUDE(dst,id);
CREATE INDEX e_in ON e(dst) INCLUDE(src,id);
CREATE PROPERTY GRAPH g VERTEX TABLES(v KEY(id)) EDGE TABLES(e KEY(id)
 SOURCE KEY(src) REFERENCES v(id) DESTINATION KEY(dst) REFERENCES v(id));
CREATE TABLE business(id bigint,label text);
INSERT INTO business VALUES(1,'a'),(2,'b'),(2,'b2'),(99,'missing');
ANALYZE v;
ANALYZE e;

CREATE FUNCTION pg_temp.direction_bag(q text) RETURNS jsonb LANGUAGE plpgsql AS $$
DECLARE r record; bag jsonb='[]';
BEGIN
 FOR r IN EXECUTE q LOOP bag:=bag||jsonb_build_array(to_jsonb(r)); END LOOP;
 RETURN (SELECT coalesce(jsonb_agg(value ORDER BY value),'[]'::jsonb) FROM jsonb_array_elements(bag));
END $$;
CREATE FUNCTION pg_temp.direction_check(q text,oracle text,stages int) RETURNS void LANGUAGE plpgsql AS $$
DECLARE mode text; p jsonb; graphs int; hops int; actual jsonb; wanted jsonb;
BEGIN
 wanted:=pg_temp.direction_bag(oracle);
 FOREACH mode IN ARRAY ARRAY['relational','btree'] LOOP
   EXECUTE 'EXPLAIN(ANALYZE,TIMING OFF,FORMAT JSON) /*+ GraphRoute('||mode||') */ '||q INTO p;
   WITH RECURSIVE plans(n) AS (
    SELECT p->0->'Plan' UNION ALL SELECT x FROM plans CROSS JOIN LATERAL jsonb_array_elements(n->'Plans') x
   ) SELECT count(*) FILTER(WHERE n->>'Custom Plan Provider'='GraphExpandFixed'),
     coalesce(sum((n->>'Graph Stages')::int),0) INTO graphs,hops FROM plans;
   IF graphs<>(CASE WHEN mode='btree' THEN 1 ELSE 0 END) OR (mode='btree' AND hops<>stages) THEN
    RAISE EXCEPTION 'direction plan mismatch: %, %',q,p; END IF;
   actual:=pg_temp.direction_bag('/*+ GraphRoute('||mode||') */ '||q);
   IF actual IS DISTINCT FROM wanted THEN
    RAISE EXCEPTION 'direction bag mismatch: %, got %, wanted %',q,actual,wanted; END IF;
 END LOOP;
END $$;

DO $$
DECLARE hops int; code int; n int; d int; i int; anchor int; pattern text; rels text;
 cols text; term text; q text; oracle text; wrapper text; cond text; edge_name text;
 input_name text; output_name text; pred text;
BEGIN
 FOR hops IN 1..3 LOOP
  FOR code IN 0..(power(3,hops)::int-1) LOOP
   FOR anchor IN 0..1 LOOP
    pattern:='(v0 IS v'||CASE WHEN anchor=1 THEN ' WHERE v0.id=1::bigint' ELSE '' END||')';
    rels:='v v0'; cols:='v0.id AS origin'; n:=code;
    FOR i IN 1..hops LOOP
     d:=n%3; n:=n/3; edge_name:='e'||i; input_name:='v'||(i-1); output_name:='v'||i;
     pattern:=pattern||CASE d WHEN 0 THEN '-[' WHEN 1 THEN '<-[' ELSE '-[' END||edge_name||' IS e]'||
       CASE d WHEN 0 THEN '->' ELSE '-' END||'('||output_name||' IS v)';
     cond:=CASE d
       WHEN 0 THEN format('%s.src=%s.id AND %s.dst=%s.id',edge_name,input_name,edge_name,output_name)
       WHEN 1 THEN format('%s.dst=%s.id AND %s.src=%s.id',edge_name,input_name,edge_name,output_name)
       ELSE format('(%1$s.src=%2$s.id AND %1$s.dst=%3$s.id) OR (%1$s.dst=%2$s.id AND %1$s.src=%3$s.id)',edge_name,input_name,output_name)
     END;
     rels:=rels||format(' JOIN e %s ON true JOIN v %s ON (%s)',edge_name,output_name,cond);
     cols:=cols||format(',%1$s.id AS edge%2$s,%1$s.src AS src%2$s,%1$s.dst AS dst%2$s,%1$s.payload AS payload%2$s,%3$s.id AS vertex%2$s',edge_name,i,output_name);
    END LOOP;
    pred:=CASE WHEN anchor=1 THEN 'v0.id=1::bigint' ELSE 'true' END;
    q:=format('GRAPH_TABLE(g MATCH %s COLUMNS(%s))',pattern,cols);
    oracle:=format('(SELECT %s FROM %s WHERE %s)',cols,rels,pred);
    PERFORM pg_temp.direction_check('SELECT * FROM '||q||' gt','SELECT * FROM '||oracle||' gt',hops);
    term:=format('%s.id AS endpoint',output_name);
    q:=format('GRAPH_TABLE(g MATCH %s COLUMNS(%s))',pattern,term);
    oracle:=format('(SELECT %s FROM %s WHERE %s)',term,rels,pred);
    FOREACH wrapper IN ARRAY ARRAY['SELECT DISTINCT endpoint FROM @REGION@ gt','SELECT count(*) FROM @REGION@ gt',
      'SELECT t.*,gt.* FROM business t LEFT JOIN @REGION@ gt ON gt.endpoint=t.id'] LOOP
     PERFORM pg_temp.direction_check(replace(wrapper,'@REGION@',q),replace(wrapper,'@REGION@',oracle),hops);
    END LOOP;
   END LOOP;
  END LOOP;
 END LOOP;
END $$;

-- Local and history predicates use the real edge, not the traversal orientation.
DO $$ BEGIN
 PERFORM pg_temp.direction_check($q$SELECT * FROM GRAPH_TABLE(g MATCH
  (a IS v WHERE a.id=2::bigint)-[x IS e WHERE x.amount>1]-(b IS v WHERE b.age>10)
  <-[y IS e]-(c IS v) WHERE y.amount>x.amount
  COLUMNS(a.name AS origin,b.name AS middle,c.name AS endpoint,x.id AS e1,x.src AS src,y.dst AS dst)) gt$q$,
  'SELECT a.name AS origin,b.name AS middle,c.name AS endpoint,x.id AS e1,x.src,y.dst
   FROM v a JOIN e x ON true JOIN v b ON ((x.src=a.id AND x.dst=b.id) OR (x.dst=a.id AND x.src=b.id))
   JOIN e y ON y.dst=b.id JOIN v c ON c.id=y.src
   WHERE a.id=2 AND x.amount>1 AND b.age>10 AND y.amount>x.amount',2);
 -- Each of two different self-loop ids occurs exactly once per anchor.
 IF pg_temp.direction_bag($q$SELECT /*+ GraphRoute(btree) */ * FROM GRAPH_TABLE(g MATCH
   (a IS v WHERE a.id=2::bigint)-[x IS e]-(b IS v WHERE b.id=2::bigint) COLUMNS(x.id AS eid)) gt$q$)
   IS DISTINCT FROM '[{"eid":16},{"eid":17}]'::jsonb THEN
  RAISE EXCEPTION 'undirected self-loop was duplicated or collapsed'; END IF;
END $$;

-- Reverse index is a dependency; dropping it replans to typed native access.
PREPARE direction_cached AS SELECT /*+ GraphRoute(btree) */ * FROM GRAPH_TABLE(g MATCH
 (a IS v WHERE a.id=2::bigint)-[x IS e]-(b IS v) COLUMNS(x.id AS eid,b.id AS endpoint)) gt;
DO $$ BEGIN PERFORM pg_temp.direction_bag('EXECUTE direction_cached'); END $$;
DROP INDEX e_in;
DO $$
DECLARE p jsonb;
BEGIN
 IF pg_temp.direction_bag('EXECUTE direction_cached') IS DISTINCT FROM
   pg_temp.direction_bag('SELECT x.id AS eid,b.id AS endpoint FROM v a JOIN e x ON true JOIN v b
     ON ((x.src=a.id AND x.dst=b.id) OR (x.dst=a.id AND x.src=b.id)) WHERE a.id=2') THEN
   RAISE EXCEPTION 'missing reverse index changed results'; END IF;
 EXECUTE 'EXPLAIN(FORMAT JSON) EXECUTE direction_cached' INTO p;
 IF p::text NOT LIKE '%GraphExpandNative%' THEN RAISE EXCEPTION 'stale provider plan: %',p; END IF;
END $$;
CREATE INDEX e_in ON e(dst) INCLUDE(src,id);
DO $$ BEGIN
 IF pg_temp.direction_bag('EXECUTE direction_cached') IS DISTINCT FROM
   pg_temp.direction_bag('SELECT x.id AS eid,b.id AS endpoint FROM v a JOIN e x ON true JOIN v b
     ON ((x.src=a.id AND x.dst=b.id) OR (x.dst=a.id AND x.src=b.id)) WHERE a.id=2') THEN
   RAISE EXCEPTION 'direction DDL recovery mismatch'; END IF;
END $$;
DEALLOCATE direction_cached;
SET search_path=public;
DROP SCHEMA ggc_directions CASCADE;
