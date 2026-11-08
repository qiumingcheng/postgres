\set ECHO none
SET client_min_messages=warning;
SET jit=off;
CREATE SCHEMA ggc_binding_reuse;
SET search_path=ggc_binding_reuse,public;
CREATE TABLE v(id bigint PRIMARY KEY, name text, details jsonb);
CREATE TABLE e(id bigint PRIMARY KEY,src bigint NOT NULL,dst bigint NOT NULL,
 payload text,amount numeric);
INSERT INTO v SELECT i,'vertex-'||i,jsonb_build_object('id',i) FROM generate_series(1,204) s(i);
INSERT INTO e VALUES(1,1,2,'a',10),(2,1,2,'b',20),(3,1,2,NULL,30),(4,1,2,'n',NULL),
 (5,2,3,'a',5),(6,2,3,'b',15),(7,2,3,'c',25),(8,2,3,NULL,NULL);
INSERT INTO e SELECT 100+i,3,i,'branch',1 FROM generate_series(4,204) s(i);
INSERT INTO e SELECT 1000+i,i,i,'loop',2 FROM generate_series(4,204) s(i);
CREATE INDEX e_src ON e(src) INCLUDE(dst,id);
CREATE PROPERTY GRAPH g VERTEX TABLES(v KEY(id)) EDGE TABLES(e KEY(id)
 SOURCE KEY(src) REFERENCES v(id) DESTINATION KEY(dst) REFERENCES v(id));
ANALYZE;
CREATE FUNCTION pg_temp.binding_bag(q text) RETURNS jsonb LANGUAGE plpgsql AS $$
DECLARE r record; result jsonb='[]';
BEGIN
 FOR r IN EXECUTE q LOOP result:=result||jsonb_build_array(to_jsonb(r)); END LOOP;
 RETURN (SELECT coalesce(jsonb_agg(value ORDER BY value),'[]'::jsonb)
         FROM jsonb_array_elements(result));
END $$;
CREATE FUNCTION pg_temp.binding_check(predicate text) RETURNS void LANGUAGE plpgsql AS $$
DECLARE q text; wanted jsonb; actual jsonb; plan jsonb; early int; hits int;
BEGIN
 q:=format($q$SELECT * FROM GRAPH_TABLE(g MATCH
 (a IS v WHERE a.id=1::bigint)-[x IS e]->(b IS v)-[y IS e]->(c IS v)
 -[z IS e]->(d IS v)-[w IS e]->(f IS v) WHERE %s
 COLUMNS(a.name AS origin,b.details AS middle,x.id AS first_edge,y.id AS second_edge,
         x.amount AS first_amount,y.amount AS second_amount,f.id AS endpoint)) gt$q$,predicate);
 wanted:=pg_temp.binding_bag('/*+ GraphRoute(relational) */ '||q);
 actual:=pg_temp.binding_bag('/*+ GraphRoute(btree) */ '||q);
 IF actual IS DISTINCT FROM wanted THEN RAISE EXCEPTION 'binding bag differs: %',predicate; END IF;
 EXECUTE 'EXPLAIN(ANALYZE,TIMING OFF,FORMAT JSON) /*+ GraphRoute(btree) */ '||q INTO plan;
 WITH RECURSIVE p(n) AS (SELECT plan->0->'Plan' UNION ALL
   SELECT child FROM p CROSS JOIN LATERAL jsonb_array_elements(n->'Plans') child)
 SELECT coalesce(sum((n->>'Graph Early Binding Filters')::int),0),
        coalesce(sum((n->>'Graph Property Cache Hits')::int),0) INTO early,hits FROM p;
 IF early=0 OR hits=0 THEN RAISE EXCEPTION 'early binding/cache not used: %',plan; END IF;
END $$;
SELECT pg_temp.binding_check('y.amount>x.amount');
SELECT pg_temp.binding_check('y.amount IS NOT DISTINCT FROM x.amount');
SELECT pg_temp.binding_check('x.amount+y.amount IS NULL OR c.id>a.id');
SELECT pg_temp.binding_check('a.name<>c.name AND y.amount>x.amount');

-- Promote only after reuse, then switch identities: NULL by-reference values
-- and changing snapshots must not reuse the previous row's owned Datums.
INSERT INTO e SELECT 10000+i*10+j,i,i,'repeat',j
 FROM generate_series(4,8) i CROSS JOIN generate_series(1,2) j;
UPDATE v SET name=NULL,details=NULL WHERE id IN (4,6);
DO $$
DECLARE q text=$q$SELECT * FROM GRAPH_TABLE(g MATCH
 (a IS v WHERE a.id>=4::bigint AND a.id<=8::bigint)-[x IS e]->(b IS v)-[y IS e]->(c IS v)
 COLUMNS(a.name AS origin,b.details AS middle,c.name AS destination,
         x.id AS first_edge,y.id AS second_edge,x.amount AS amount)) gt$q$;
BEGIN
 FOR pass IN 1..2 LOOP
   IF pg_temp.binding_bag('/*+ GraphRoute(btree) */ '||q) IS DISTINCT FROM
      pg_temp.binding_bag('/*+ GraphRoute(relational) */ '||q) THEN
     RAISE EXCEPTION 'property identity/NULL transition changed bag';
   END IF;
   UPDATE v SET name='changed-'||id,details=jsonb_build_object('changed',id)
    WHERE id IN (4,6);
   UPDATE v SET name=NULL,details=NULL WHERE id IN (5,7);
 END LOOP;
END $$;

-- One property can exceed the optional cache without making the query fail.
SET work_mem='64kB';
ALTER TABLE v ALTER COLUMN name SET STORAGE PLAIN;
UPDATE v SET name=repeat('wide',1500) WHERE id=1;
DO $$
DECLARE q text;
BEGIN
 q:='SELECT * FROM GRAPH_TABLE(g MATCH (a IS v WHERE a.id=1::bigint)-[x IS e]->(b IS v)
       COLUMNS(a.name AS origin,x.id AS edge,b.details AS destination)) gt';
 IF pg_temp.binding_bag('/*+ GraphRoute(relational) */ '||q) IS DISTINCT FROM
    pg_temp.binding_bag('/*+ GraphRoute(btree) */ '||q) THEN RAISE EXCEPTION 'wide property differs'; END IF;
END $$;
RESET work_mem;
PREPARE binding_params(numeric) AS SELECT /*+ GraphRoute(btree) */ count(*) FROM GRAPH_TABLE(g MATCH
 (a IS v WHERE a.id=1::bigint)-[x IS e]->(b IS v)-[y IS e]->(c IS v)
 WHERE y.amount>x.amount+$1 COLUMNS(c.id AS endpoint)) gt;
SET plan_cache_mode=force_generic_plan;
EXECUTE binding_params(0);
EXECUTE binding_params(100);
EXECUTE binding_params(0);
DEALLOCATE binding_params;
RESET plan_cache_mode;
DROP SCHEMA ggc_binding_reuse CASCADE;
RESET search_path;
RESET client_min_messages;
RESET jit;
