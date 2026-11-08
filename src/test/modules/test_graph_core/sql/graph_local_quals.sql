\set ECHO none
SET client_min_messages=warning;
LOAD 'test_graph_core';
SET jit=off;
CREATE SCHEMA ggc_local_quals;
SET search_path=ggc_local_quals,public;
CREATE TABLE v(id bigint PRIMARY KEY,name text,age integer,details jsonb);
CREATE TABLE e(id bigint PRIMARY KEY,src bigint NOT NULL,dst bigint NOT NULL,
 payload text,amount numeric,properties jsonb);
INSERT INTO v VALUES(1,'A',40,'{"city":"Rome"}'),(2,'B',20,'{}'),
 (3,'C',NULL,'{"city":"Paris"}'),(4,'D',17,NULL);
INSERT INTO e VALUES
 (11,1,2,'a',10,'{"type":"pay"}'),(12,1,2,NULL,1,'{"type":"link"}'),
 (13,2,3,'b',100,'{"type":"pay"}'),(14,3,1,'c',-5,'{}'),
 (15,1,99,'missing',99,'{}'),(16,99,2,'missing',99,'{}'),
 (17,2,2,'self',NULL,NULL),(18,2,4,'d',2,'{}');
CREATE INDEX e_btree ON e(src) INCLUDE(dst,id);
CREATE PROPERTY GRAPH g VERTEX TABLES(v KEY(id)) EDGE TABLES(e KEY(id)
 SOURCE KEY(src) REFERENCES v(id) DESTINATION KEY(dst) REFERENCES v(id));
CREATE TABLE business(id bigint,label text);
INSERT INTO business VALUES(2,'two'),(3,'three-a'),(3,'three-b'),(NULL,'null');
ANALYZE v;
ANALYZE e;
VACUUM v;

CREATE FUNCTION pg_temp.local_bag(q text) RETURNS jsonb LANGUAGE plpgsql AS $$
DECLARE r record; bag jsonb='[]';
BEGIN
 FOR r IN EXECUTE q LOOP bag:=bag||jsonb_build_array(to_jsonb(r)); END LOOP;
 RETURN (SELECT coalesce(jsonb_agg(value ORDER BY value),'[]'::jsonb)
         FROM jsonb_array_elements(bag));
END $$;

CREATE FUNCTION pg_temp.local_check(q text,oracle text) RETURNS void LANGUAGE plpgsql AS $$
DECLARE mode text; p jsonb; graphs int; filters int; sets int; actual jsonb; wanted jsonb;
BEGIN
 wanted:=pg_temp.local_bag(oracle);
 FOREACH mode IN ARRAY ARRAY['relational','btree'] LOOP
   EXECUTE 'EXPLAIN(ANALYZE,TIMING OFF,FORMAT JSON) /*+ GraphRoute('||mode||') */ '||q INTO p;
   WITH RECURSIVE plans(n) AS (
     SELECT p->0->'Plan' UNION ALL
     SELECT x FROM plans CROSS JOIN LATERAL jsonb_array_elements(n->'Plans') x
   ) SELECT count(*) FILTER(WHERE n->>'Custom Plan Provider'='GraphExpandFixed'),
            coalesce(sum((n->>'Graph Local Filters')::int),0),
            count(*) FILTER(WHERE n->>'Graph Prefix Representation' LIKE '%endpoint sets%')
      INTO graphs,filters,sets FROM plans;
   IF graphs<>(CASE WHEN mode='btree' THEN 1 ELSE 0 END) OR
      (mode='btree' AND filters=0) OR sets<>0 OR
      p->0->>'Graph Route' IS DISTINCT FROM mode THEN
     RAISE EXCEPTION 'local filter plan mismatch: %, %',q,p;
   END IF;
   actual:=pg_temp.local_bag('/*+ GraphRoute('||mode||') */ '||q);
   IF actual IS DISTINCT FROM wanted THEN
     RAISE EXCEPTION 'local filter bag mismatch: %, got %, wanted %',q,actual,wanted;
   END IF;
 END LOOP;
END $$;

DO $$
DECLARE predicates text[]; q text; oracle text; base_q text; base_oracle text; mode int;
BEGIN
 FOREACH predicates SLICE 1 IN ARRAY ARRAY[
   ARRAY['x.payload=''a''','','',''],
   ARRAY['x.payload IS NULL','','',''],
   ARRAY['x.amount>5','','',''],
   ARRAY['x.properties->>''type''=''pay''','','',''],
   ARRAY['x.id>0','b.age>=18','',''],
   ARRAY['','b.details->>''city'' IS NULL','',''],
   ARRAY['','','y.amount>=10',''],
   ARRAY['','','y.amount IS NULL',''],
   ARRAY['','','y.payload IS NOT NULL','c.age IS NULL'],
   ARRAY['','','','c.details->>''city''=''Paris'''],
   ARRAY['x.amount>0','b.age>18','y.amount<5','c.age<18'],
   ARRAY['x.amount>0','','','c.name IN (''A'',''B'')'],
   ARRAY['','','','c.age>100'],
   ARRAY['x.amount>0','','','c.id>0']
 ] LOOP
   base_q:=format('GRAPH_TABLE(g MATCH (a IS v WHERE a.id=1::bigint)
     -[x IS e%s]->(b IS v%s)-[y IS e%s]->(c IS v%s)
     COLUMNS(x.id AS e1,y.id AS e2,c.id AS endpoint,x.payload AS payload))',
     CASE WHEN predicates[1]='' THEN '' ELSE ' WHERE '||predicates[1] END,
     CASE WHEN predicates[2]='' THEN '' ELSE ' WHERE '||predicates[2] END,
     CASE WHEN predicates[3]='' THEN '' ELSE ' WHERE '||predicates[3] END,
     CASE WHEN predicates[4]='' THEN '' ELSE ' WHERE '||predicates[4] END);
   base_oracle:='(SELECT x.id AS e1,y.id AS e2,c.id AS endpoint,x.payload
     FROM v a JOIN e x ON x.src=a.id JOIN v b ON b.id=x.dst
     JOIN e y ON y.src=b.id JOIN v c ON c.id=y.dst WHERE a.id=1::bigint';
   FOR mode IN 1..4 LOOP
     IF predicates[mode]<>'' THEN base_oracle:=base_oracle||' AND ('||predicates[mode]||')'; END IF;
   END LOOP;
   base_oracle:=base_oracle||')';
   FOR mode IN 1..4 LOOP
     q:=CASE mode
       WHEN 1 THEN 'SELECT * FROM @REGION@ gt'
       WHEN 2 THEN 'SELECT DISTINCT endpoint FROM @REGION@ gt'
       WHEN 3 THEN 'SELECT count(*) FROM @REGION@ gt'
       ELSE 'SELECT gt.*,t.label FROM business t LEFT JOIN @REGION@ gt ON t.id=gt.endpoint'
     END;
     PERFORM pg_temp.local_check(replace(q,'@REGION@',base_q),replace(q,'@REGION@',base_oracle));
   END LOOP;
   -- Multiple anchors may produce the same endpoint: preserve the full bag.
   PERFORM pg_temp.local_check('SELECT * FROM '||replace(base_q,' WHERE a.id=1::bigint',''),
     'SELECT * FROM '||replace(base_oracle,'WHERE a.id=1::bigint','WHERE true')||' gt');
 END LOOP;
END $$;

-- A real rescan through an outer Nested Loop, without a Materialize cache.
SET enable_hashjoin=off;
SET enable_mergejoin=off;
SET enable_material=off;
DO $$
DECLARE q text=$q$SELECT t.label,gt.* FROM business t CROSS JOIN GRAPH_TABLE(g MATCH
 (a IS v WHERE a.id=1::bigint)-[x IS e WHERE x.amount>5]->(b IS v)
 -[y IS e]->(c IS v WHERE c.age IS NULL)
 COLUMNS(x.id AS e1,y.id AS e2,c.id AS endpoint)) gt$q$;
BEGIN
 PERFORM pg_temp.local_check(q,$q$SELECT t.label,x.id AS e1,y.id AS e2,c.id AS endpoint
 FROM business t CROSS JOIN v a JOIN e x ON x.src=a.id JOIN v b ON b.id=x.dst
 JOIN e y ON y.src=b.id JOIN v c ON c.id=y.dst WHERE a.id=1 AND x.amount>5 AND c.age IS NULL$q$);
END $$;
RESET enable_hashjoin;
RESET enable_mergejoin;
RESET enable_material;

-- Generic parameters must be read on every execution, not captured at planning.
PREPARE local_params(numeric,integer) AS
SELECT /*+ GraphRoute(btree) */ * FROM GRAPH_TABLE(g MATCH
 (a IS v WHERE a.id=1::bigint)-[x IS e WHERE x.amount>$1]->(b IS v WHERE b.age>$2)
 -[y IS e]->(c IS v) COLUMNS(x.id AS e1,y.id AS e2,c.id AS endpoint)) gt;
DO $$
DECLARE mode text; threshold int; actual jsonb; wanted jsonb;
BEGIN
 FOREACH mode IN ARRAY ARRAY['force_generic_plan','force_custom_plan'] LOOP
   PERFORM set_config('plan_cache_mode',mode,true);
   FOREACH threshold IN ARRAY ARRAY[0,5,20,0] LOOP
     actual:=pg_temp.local_bag(format('EXECUTE local_params(%s,18)',threshold));
     wanted:=pg_temp.local_bag(format('SELECT x.id AS e1,y.id AS e2,c.id AS endpoint
       FROM v a JOIN e x ON x.src=a.id JOIN v b ON b.id=x.dst
       JOIN e y ON y.src=b.id JOIN v c ON c.id=y.dst
       WHERE a.id=1 AND x.amount>%s AND b.age>18',threshold));
     IF actual IS DISTINCT FROM wanted THEN RAISE EXCEPTION 'parameter filter mismatch'; END IF;
   END LOOP;
 END LOOP;
END $$;
DEALLOCATE local_params;

-- Prepared function dependencies remain visible to setrefs and invalidation.
CREATE FUNCTION allow_amount(numeric) RETURNS boolean LANGUAGE sql IMMUTABLE
AS 'SELECT $1>5';
PREPARE local_function AS SELECT /*+ GraphRoute(btree) */ * FROM GRAPH_TABLE(g MATCH
 (a IS v WHERE a.id=1::bigint)-[x IS e WHERE allow_amount(x.amount)]->(b IS v)
 COLUMNS(x.id AS eid)) gt;
DO $$ BEGIN
 IF pg_temp.local_bag('EXECUTE local_function') <> '[{"eid":11}]'::jsonb THEN
   RAISE EXCEPTION 'initial function filter mismatch'; END IF;
END $$;
CREATE OR REPLACE FUNCTION allow_amount(numeric) RETURNS boolean LANGUAGE sql IMMUTABLE
AS 'SELECT $1>500';
DO $$ BEGIN
 IF pg_temp.local_bag('EXECUTE local_function') <> '[]'::jsonb THEN
   RAISE EXCEPTION 'function invalidation lost'; END IF;
END $$;
REINDEX INDEX e_btree;
DEALLOCATE local_function;

-- Volatile internal predicates must not be discarded or relocated. Typed
-- outputs and cross-variable predicates are covered by graph_properties.
CREATE SEQUENCE effects;
DO $$
DECLARE q text; actual jsonb;
BEGIN
 FOREACH q IN ARRAY ARRAY[
   $q$SELECT * FROM GRAPH_TABLE(g MATCH (a IS v)-[x IS e WHERE nextval('effects')>0]->(b IS v) COLUMNS(x.id AS eid)) gt$q$
 ] LOOP
   actual:=pg_temp.local_bag('/*+ GraphRoute(btree) */ '||q);
   IF actual IS DISTINCT FROM pg_temp.local_bag('/*+ GraphRoute(relational) */ '||q) THEN
     RAISE EXCEPTION 'native volatile filter lost bag'; END IF;
   IF NOT (SELECT is_called FROM effects) THEN
     RAISE EXCEPTION 'native volatile filter was discarded'; END IF;
 END LOOP;
END $$;

-- Ordinary error propagation must clean up borrowed slots/scans for reuse.
DO $$
DECLARE failed boolean=false;
BEGIN
 BEGIN
   EXECUTE $q$SELECT /*+ GraphRoute(btree) */ * FROM GRAPH_TABLE(g MATCH
     (a IS v WHERE a.id=1::bigint)-[x IS e WHERE (1/(x.amount-10))>0]->(b IS v)
     COLUMNS(x.id AS eid)) gt$q$;
 EXCEPTION WHEN division_by_zero THEN failed:=true; END;
 IF NOT failed THEN RAISE EXCEPTION 'filter did not report division by zero'; END IF;
 PERFORM pg_temp.local_check($q$SELECT * FROM GRAPH_TABLE(g MATCH
   (a IS v WHERE a.id=1::bigint)-[x IS e WHERE x.amount>5]->(b IS v)
   COLUMNS(x.id AS eid)) gt$q$,
   'SELECT x.id AS eid FROM v a JOIN e x ON x.src=a.id JOIN v b ON b.id=x.dst WHERE a.id=1 AND x.amount>5');
END $$;

-- A terminal key read only by its local predicate has no output binding
-- position.  It is supplied by the local vertex slot, not the prefix map.
DO $$BEGIN
 PERFORM pg_temp.local_check($q$SELECT * FROM GRAPH_TABLE(g MATCH
   (a IS v WHERE a.id=1::bigint)-[x IS e]->(b IS v WHERE b.id>1::bigint)
   COLUMNS(a.id AS start_id)) gt$q$,
   'SELECT a.id AS start_id FROM v a JOIN e x ON x.src=a.id JOIN v b ON b.id=x.dst WHERE a.id=1 AND b.id>1');
END $$;

SET search_path=public;
DROP SCHEMA ggc_local_quals CASCADE;
