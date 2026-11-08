\set ECHO none
SET client_min_messages=warning;
SET jit=off;
CREATE SCHEMA ggc_mappings;
SET search_path=ggc_mappings,public;
-- No LOAD: this is the native PGQ planning entry, not a diagnostic shim.
-- Keys deliberately share values across different tables, not identities.
CREATE TABLE person(name text,id bigint UNIQUE NOT NULL,age integer);
CREATE TABLE company(details jsonb,id bigint UNIQUE NOT NULL,title text);
CREATE TABLE product(price numeric,title text,id bigint PRIMARY KEY);
CREATE TABLE work(pay numeric,dst bigint NOT NULL,id bigint PRIMARY KEY,src bigint NOT NULL);
CREATE TABLE offer(dst bigint NOT NULL,id bigint UNIQUE NOT NULL,src bigint NOT NULL);
CREATE TABLE link(dst bigint NOT NULL,id bigint UNIQUE NOT NULL,src bigint NOT NULL);
INSERT INTO person VALUES('Alice',1,40),('Bob',2,20),('Carol',3,30),('Dave',4,18);
INSERT INTO company VALUES('{"city":"Rome"}',1,'One'),('{}',2,'Two'),(NULL,3,'Three');
INSERT INTO product VALUES(100,'P1',1),(200,'P2',2),(NULL,'P3',3);
INSERT INTO work VALUES(10,2,11,1),(20,2,12,1),(30,1,13,2),(NULL,3,14,3),
 (40,99,15,1),(50,2,16,99);
INSERT INTO offer VALUES(1,21,2),(1,22,2),(3,23,2),(2,24,1),(99,25,1),(2,26,99);
INSERT INTO link VALUES(2,31,1),(2,32,1),(1,33,2),(2,34,2),(2,35,2),
 (3,36,2),(1,37,3),(99,38,1),(2,39,99);
CREATE INDEX work_out ON work(src);
CREATE INDEX work_in ON work(dst);
CREATE INDEX offer_out ON offer(src) INCLUDE(id,dst);
CREATE INDEX offer_in ON offer(dst);
CREATE INDEX link_out ON link(src) INCLUDE(id,dst);
CREATE INDEX link_in ON link(dst);
CREATE PROPERTY GRAPH g VERTEX TABLES(person KEY(id),company KEY(id),product KEY(id))
 EDGE TABLES(work KEY(id) SOURCE KEY(src) REFERENCES person(id) DESTINATION KEY(dst) REFERENCES company(id),
 offer KEY(id) SOURCE KEY(src) REFERENCES company(id) DESTINATION KEY(dst) REFERENCES product(id),
 link KEY(id) SOURCE KEY(src) REFERENCES person(id) DESTINATION KEY(dst) REFERENCES person(id));
ANALYZE person;
ANALYZE company;
ANALYZE product;
ANALYZE work;
ANALYZE offer;
ANALYZE link;

CREATE FUNCTION pg_temp.mapping_bag(q text) RETURNS jsonb LANGUAGE plpgsql AS $$
DECLARE r record; bag jsonb='[]';
BEGIN
 FOR r IN EXECUTE q LOOP bag:=bag||jsonb_build_array(to_jsonb(r)); END LOOP;
 RETURN (SELECT coalesce(jsonb_agg(value ORDER BY value),'[]'::jsonb)
         FROM jsonb_array_elements(bag));
END $$;
CREATE FUNCTION pg_temp.mapping_check(q text,oracle text,closed boolean=false)
 RETURNS void LANGUAGE plpgsql AS $$
DECLARE budget int; p jsonb; graphs int; sets int; wanted jsonb; actual jsonb;
BEGIN
 wanted:=pg_temp.mapping_bag(oracle);
 IF pg_temp.mapping_bag('/*+ GraphRoute(relational) */ '||q) IS DISTINCT FROM wanted THEN
   RAISE EXCEPTION 'relational mapping oracle mismatch: %',q;
 END IF;
 FOREACH budget IN ARRAY ARRAY[1,8] LOOP
   PERFORM set_config('graph_btree_max_candidates',budget::text,true);
   actual:=pg_temp.mapping_bag('/*+ GraphRoute(btree) */ '||q);
   IF actual IS DISTINCT FROM wanted THEN
     RAISE EXCEPTION 'mapping bag mismatch: %, %, got %, wanted %',budget,q,actual,wanted;
   END IF;
   EXECUTE 'EXPLAIN(ANALYZE,TIMING OFF,FORMAT JSON) /*+ GraphRoute(btree) */ '||q INTO p;
   WITH RECURSIVE plans(n) AS (SELECT p->0->'Plan' UNION ALL
     SELECT c FROM plans CROSS JOIN LATERAL jsonb_array_elements(n->'Plans') c)
   SELECT count(*) FILTER(WHERE n->>'Custom Plan Provider'='GraphExpandFixed'),
          count(*) FILTER(WHERE n->>'Graph Prefix Representation' LIKE '%endpoint sets%')
     INTO graphs,sets FROM plans;
   IF graphs<>1 OR (closed AND sets<>0) THEN
     RAISE EXCEPTION 'mapping plan mismatch: %, %',q,p;
   END IF;
 END LOOP;
END $$;

-- Heterogeneous mappings, property types, filtered and unfiltered bags,
-- reverse/middle starts, catalog-directed undirected syntax, PG upper nodes.
DO $$
DECLARE shape text; pred text; cols text; outerq text; graph text; oracle text;
BEGIN
 FOREACH shape IN ARRAY ARRAY[
   '(a IS person)-[x IS work]->(b IS company)-[y IS offer]->(c IS product)',
   '(c IS product)<-[y IS offer]-(b IS company)<-[x IS work]-(a IS person)',
   '(a IS person)-[x IS work]-(b IS company)-[y IS offer]-(c IS product)',
   '(c IS product)-[y IS offer]-(b IS company)-[x IS work]-(a IS person)'
 ] LOOP
 FOREACH pred IN ARRAY ARRAY['true','a.id=1::bigint','b.id=2::bigint',
   'c.id=1::bigint AND x.pay>10','a.age<x.pay AND c.price>x.pay'] LOOP
 FOREACH cols IN ARRAY ARRAY[
   'a.id AS a,b.id AS b,c.id AS c,x.id AS x,y.id AS y',
   'a.name AS name,b.details AS details,c.title AS title,x.pay AS pay,y.src AS source,y.dst AS destination'
 ] LOOP
   graph:=format('GRAPH_TABLE(g MATCH %s WHERE %s COLUMNS(%s))',shape,pred,cols);
   oracle:=format('(SELECT %s FROM person a JOIN work x ON a.id=x.src
     JOIN company b ON x.dst=b.id JOIN offer y ON b.id=y.src JOIN product c ON y.dst=c.id WHERE %s)',cols,pred);
   FOREACH outerq IN ARRAY ARRAY['SELECT * FROM @R@ gt','SELECT count(*) FROM @R@ gt','SELECT DISTINCT * FROM @R@ gt'] LOOP
     PERFORM pg_temp.mapping_check(replace(outerq,'@R@',graph),replace(outerq,'@R@',oracle));
   END LOOP;
 END LOOP;
 END LOOP;
 END LOOP;
 -- Non-first terminal key: safe endpoint sets and the upper DISTINCT proof.
 PERFORM pg_temp.mapping_check($q$SELECT DISTINCT endpoint FROM GRAPH_TABLE(g MATCH
   (a IS person WHERE a.id=1::bigint)-[x IS work]->(b IS company)-[y IS offer]->(c IS product)
   COLUMNS(c.id AS endpoint)) gt$q$,
   'SELECT DISTINCT c.id AS endpoint FROM person a JOIN work x ON a.id=x.src JOIN company b ON b.id=x.dst
    JOIN offer y ON y.src=b.id JOIN product c ON c.id=y.dst WHERE a.id=1');
 -- An ordinary outer join remains a normal PG operator over the graph region.
 PERFORM pg_temp.mapping_check($q$SELECT p.id,gt.* FROM person p LEFT JOIN GRAPH_TABLE(g MATCH
   (a IS person)-[x IS work]->(b IS company)-[y IS offer]->(c IS product)
   COLUMNS(a.id AS origin,c.id AS endpoint)) gt ON p.id=gt.origin$q$,
   'SELECT p.id,gt.* FROM person p LEFT JOIN (SELECT a.id AS origin,c.id AS endpoint FROM person a
    JOIN work x ON a.id=x.src JOIN company b ON b.id=x.dst JOIN offer y ON y.src=b.id
    JOIN product c ON c.id=y.dst) gt ON p.id=gt.origin');
END $$;

-- Reuse the SAME vertex variable, not a new vertex constrained only by WHERE.
-- Parallel edges, self loops and revisits preserve the full path multiplicity.
DO $$
DECLARE d1 text; d2 text; xjoin text; yjoin text; shape text; pred text; graph text; oracle text; outerq text;
 cols text='a.id AS a,b.id AS b,x.id AS x,y.id AS y,a.name AS name,x.src AS xs,x.dst AS xd,y.src AS ys,y.dst AS yd';
BEGIN
 FOREACH d1 IN ARRAY ARRAY['out','in','both'] LOOP
 FOREACH d2 IN ARRAY ARRAY['out','in','both'] LOOP
   xjoin:=CASE d1 WHEN 'out' THEN 'x.src=a.id AND x.dst=b.id' WHEN 'in' THEN 'x.dst=a.id AND x.src=b.id'
     ELSE '((x.src=a.id AND x.dst=b.id) OR (x.dst=a.id AND x.src=b.id))' END;
   yjoin:=CASE d2 WHEN 'out' THEN 'y.src=b.id AND y.dst=a.id' WHEN 'in' THEN 'y.dst=b.id AND y.src=a.id'
     ELSE '((y.src=b.id AND y.dst=a.id) OR (y.dst=b.id AND y.src=a.id))' END;
   shape:='(a IS person)'||CASE d1 WHEN 'out' THEN '-[x IS link]->' WHEN 'in' THEN '<-[x IS link]-' ELSE '-[x IS link]-' END||
     '(b IS person)'||CASE d2 WHEN 'out' THEN '-[y IS link]->' WHEN 'in' THEN '<-[y IS link]-' ELSE '-[y IS link]-' END||'(a)';
   FOREACH pred IN ARRAY ARRAY['true','a.id=1::bigint','b.id=2::bigint AND x.id<y.id','a.age>b.age'] LOOP
     graph:=format('GRAPH_TABLE(g MATCH %s WHERE %s COLUMNS(%s))',shape,pred,cols);
     oracle:=format('(SELECT %s FROM person a JOIN link x ON true JOIN person b ON %s JOIN link y ON %s WHERE %s)',cols,xjoin,yjoin,pred);
     FOREACH outerq IN ARRAY ARRAY['SELECT * FROM @R@ gt','SELECT count(*) FROM @R@ gt','SELECT DISTINCT a FROM @R@ gt'] LOOP
       PERFORM pg_temp.mapping_check(replace(outerq,'@R@',graph),replace(outerq,'@R@',oracle),true);
     END LOOP;
   END LOOP;
 END LOOP;
 END LOOP;
 FOREACH shape IN ARRAY ARRAY['(a IS person)-[x IS link]->(a)',
   '(a IS person)<-[x IS link]-(a)','(a IS person)-[x IS link]-(a)'] LOOP
   PERFORM pg_temp.mapping_check(format('SELECT * FROM GRAPH_TABLE(g MATCH %s COLUMNS(a.id AS a,x.id AS x,a.name AS name,x.src AS src,x.dst AS dst)) gt',shape),
     'SELECT a.id AS a,x.id AS x,a.name AS name,x.src,x.dst FROM person a JOIN link x ON x.src=a.id AND x.dst=a.id',true);
 END LOOP;
 PERFORM pg_temp.mapping_check($q$SELECT * FROM GRAPH_TABLE(g MATCH (a IS person)-[x IS link]->(b IS person)
   -[y IS link]->(c IS person)-[z IS link]->(a)-[w IS link]->(b)
   COLUMNS(a.id AS a,b.id AS b,c.id AS c,x.id AS x,y.id AS y,z.id AS z,w.id AS w)) gt$q$,
   'SELECT a.id AS a,b.id AS b,c.id AS c,x.id AS x,y.id AS y,z.id AS z,w.id AS w FROM person a
    JOIN link x ON x.src=a.id JOIN person b ON b.id=x.dst JOIN link y ON y.src=b.id
    JOIN person c ON c.id=y.dst JOIN link z ON z.src=c.id AND z.dst=a.id JOIN link w ON w.src=a.id AND w.dst=b.id',true);
 PERFORM pg_temp.mapping_check($q$SELECT * FROM GRAPH_TABLE(g MATCH (a IS person)-[x IS work]->(b IS company)
   <-[y IS work]-(a) WHERE y.pay>=x.pay COLUMNS(a.name AS name,b.title AS title,x.id AS x,y.id AS y)) gt$q$,
   'SELECT a.name,b.title,x.id AS x,y.id AS y FROM person a JOIN work x ON x.src=a.id
    JOIN company b ON b.id=x.dst JOIN work y ON y.dst=b.id AND y.src=a.id WHERE y.pay>=x.pay',true);
END $$;

PREPARE mapped(bigint) AS /*+ GraphRoute(btree) */ SELECT * FROM GRAPH_TABLE(g MATCH
 (a IS person)-[x IS work]->(b IS company)-[y IS offer]->(c IS product) WHERE b.id=$1
 COLUMNS(a.name AS name,b.details AS details,c.id AS endpoint,x.pay AS pay,y.id AS edge)) gt;
DO $$
DECLARE mode text; k int; expected jsonb;
BEGIN
 FOREACH mode IN ARRAY ARRAY['force_generic_plan','force_custom_plan'] LOOP
   PERFORM set_config('plan_cache_mode',mode,true);
   FOREACH k IN ARRAY ARRAY[1,2,3,2] LOOP
     expected:=pg_temp.mapping_bag(format('SELECT a.name,b.details,c.id AS endpoint,x.pay,y.id AS edge FROM person a
       JOIN work x ON x.src=a.id JOIN company b ON b.id=x.dst JOIN offer y ON y.src=b.id
       JOIN product c ON c.id=y.dst WHERE b.id=%s',k));
     IF pg_temp.mapping_bag(format('EXECUTE mapped(%s)',k)) IS DISTINCT FROM expected THEN
       RAISE EXCEPTION 'saved mapping mismatch';
     END IF;
   END LOOP;
 END LOOP;
END $$;
REINDEX INDEX company_id_key;
REINDEX INDEX offer_out;
DO $$
BEGIN
 IF pg_temp.mapping_bag('EXECUTE mapped(2)') IS DISTINCT FROM pg_temp.mapping_bag(
   'SELECT a.name,b.details,c.id AS endpoint,x.pay,y.id AS edge FROM person a JOIN work x ON x.src=a.id
    JOIN company b ON b.id=x.dst JOIN offer y ON y.src=b.id JOIN product c ON c.id=y.dst WHERE b.id=2') THEN
   RAISE EXCEPTION 'reindexed mapping mismatch';
 END IF;
END $$;
DEALLOCATE mapped;
RESET plan_cache_mode;
RESET graph_btree_max_candidates;

-- Writes and rescans must consult the current snapshot, not cached identities.
BEGIN;
UPDATE work SET src=2,dst=1 WHERE id=11;
UPDATE company SET details='{"city":"changed"}' WHERE id=1;
DELETE FROM product WHERE id=1;
INSERT INTO product VALUES(321,'replacement',1);
DO $$
BEGIN
 PERFORM pg_temp.mapping_check($q$SELECT * FROM GRAPH_TABLE(g MATCH (a IS person)-[x IS work]->(b IS company)
   -[y IS offer]->(c IS product) COLUMNS(a.name AS name,b.details AS details,c.title AS title,x.pay AS pay)) gt$q$,
   'SELECT a.name,b.details,c.title,x.pay FROM person a JOIN work x ON x.src=a.id
    JOIN company b ON b.id=x.dst JOIN offer y ON y.src=b.id JOIN product c ON c.id=y.dst');
END $$;
ROLLBACK;

CREATE FUNCTION pg_temp.mapping_native(q text) RETURNS void LANGUAGE plpgsql AS $$
DECLARE p jsonb; graphs int;
BEGIN
 IF pg_temp.mapping_bag('/*+ GraphRoute(btree) */ '||q) IS DISTINCT FROM
    pg_temp.mapping_bag('/*+ GraphRoute(relational) */ '||q) THEN
   RAISE EXCEPTION 'typed native mapping bag mismatch: %',q;
 END IF;
 EXECUTE 'EXPLAIN(FORMAT JSON) /*+ GraphRoute(btree) */ '||q INTO p;
 WITH RECURSIVE plans(n) AS (SELECT p->0->'Plan' UNION ALL
   SELECT c FROM plans CROSS JOIN LATERAL jsonb_array_elements(n->'Plans') c)
 SELECT count(*) INTO graphs FROM plans WHERE n->>'Custom Plan Provider'='GraphExpandNative';
 IF graphs<>1 THEN RAISE EXCEPTION 'typed access did not execute a graph region: %',p; END IF;
 -- AUTO still compares/retains native plans for non-fast-path shapes.
 PERFORM set_config('enable_graph_btree','on',true);
 IF pg_temp.mapping_bag(q) IS DISTINCT FROM pg_temp.mapping_bag('/*+ GraphRoute(relational) */ '||q) THEN
   RAISE EXCEPTION 'unsafe mapping fallback mismatch: %',q;
 END IF;
END $$;

-- Typed stages preserve NULL semantics without relaxing integer-provider proofs.
CREATE TABLE nullable_edge(id bigint PRIMARY KEY,src bigint,dst bigint);
INSERT INTO nullable_edge VALUES(1,1,2),(2,NULL,2),(3,1,NULL);
CREATE INDEX nullable_out ON nullable_edge(src);
CREATE INDEX nullable_in ON nullable_edge(dst);
CREATE PROPERTY GRAPH gn VERTEX TABLES(person KEY(id)) EDGE TABLES(nullable_edge KEY(id)
 SOURCE KEY(src) REFERENCES person(id) DESTINATION KEY(dst) REFERENCES person(id));
DO $$ BEGIN
 PERFORM pg_temp.mapping_native('SELECT * FROM GRAPH_TABLE(gn MATCH (a IS person)-[x IS nullable_edge]->(b IS person) COLUMNS(b.id AS endpoint)) gt');
END $$;

-- Text keys use typed native stages, never int64 surrogates.
CREATE TABLE text_vertex(id text PRIMARY KEY);
CREATE TABLE text_edge(id bigint PRIMARY KEY,src text NOT NULL,dst text NOT NULL);
INSERT INTO text_vertex VALUES('one'),('two');
INSERT INTO text_edge VALUES(1,'one','two');
CREATE INDEX text_out ON text_edge(src);
CREATE PROPERTY GRAPH gt VERTEX TABLES(text_vertex KEY(id)) EDGE TABLES(text_edge KEY(id)
 SOURCE KEY(src) REFERENCES text_vertex(id) DESTINATION KEY(dst) REFERENCES text_vertex(id) PROPERTIES(src,dst));
DO $$ BEGIN
 PERFORM pg_temp.mapping_native('SELECT * FROM GRAPH_TABLE(gt MATCH (a IS text_vertex)-[x IS text_edge]->(b IS text_vertex) COLUMNS(b.id AS endpoint)) g');
END $$;

-- A partial/expression/deferred unique index is NOT a global immediate
-- identity proof. Adding a qualifying ordinary unique index enables the
-- integer fast path; without it the native stage keeps the full relational bag.
CREATE TABLE weak_vertex(id bigint NOT NULL,name text,UNIQUE(id) DEFERRABLE INITIALLY DEFERRED);
INSERT INTO weak_vertex VALUES(1,'one'),(2,'two');
CREATE UNIQUE INDEX weak_partial ON weak_vertex(id) WHERE id>0;
CREATE UNIQUE INDEX weak_expression ON weak_vertex((abs(id)));
CREATE PROPERTY GRAPH gw VERTEX TABLES(weak_vertex KEY(id)) EDGE TABLES(link KEY(id)
 SOURCE KEY(src) REFERENCES weak_vertex(id) DESTINATION KEY(dst) REFERENCES weak_vertex(id));
DO $$ BEGIN
 PERFORM pg_temp.mapping_native('SELECT * FROM GRAPH_TABLE(gw MATCH (a IS weak_vertex)-[x IS link]->(b IS weak_vertex) COLUMNS(b.id AS endpoint,b.name AS name)) g');
END $$;
CREATE UNIQUE INDEX weak_valid ON weak_vertex(id);
DO $$ BEGIN
 PERFORM pg_temp.mapping_check('SELECT * FROM GRAPH_TABLE(gw MATCH (a IS weak_vertex)-[x IS link]->(b IS weak_vertex) COLUMNS(b.id AS endpoint,b.name AS name)) g',
   'SELECT b.id AS endpoint,b.name FROM weak_vertex a JOIN link x ON x.src=a.id JOIN weak_vertex b ON b.id=x.dst');
END $$;
DROP INDEX weak_valid;
DO $$ BEGIN
 PERFORM pg_temp.mapping_native('SELECT * FROM GRAPH_TABLE(gw MATCH (a IS weak_vertex)-[x IS link]->(b IS weak_vertex) COLUMNS(b.id AS endpoint,b.name AS name)) g');
END $$;

CREATE TABLE composite_vertex(tenant bigint,id bigint,PRIMARY KEY(tenant,id));
CREATE TABLE composite_edge(id bigint PRIMARY KEY,src_tenant bigint NOT NULL,src bigint NOT NULL,
 dst_tenant bigint NOT NULL,dst bigint NOT NULL);
INSERT INTO composite_vertex VALUES(1,1),(2,1);
INSERT INTO composite_edge VALUES(1,1,1,2,1);
CREATE PROPERTY GRAPH gc VERTEX TABLES(composite_vertex KEY(tenant,id)) EDGE TABLES(composite_edge KEY(id)
 SOURCE KEY(src_tenant,src) REFERENCES composite_vertex(tenant,id)
 DESTINATION KEY(dst_tenant,dst) REFERENCES composite_vertex(tenant,id));
DO $$ BEGIN
 PERFORM pg_temp.mapping_native('SELECT * FROM GRAPH_TABLE(gc MATCH (a IS composite_vertex)-[x IS composite_edge]->(b IS composite_vertex) COLUMNS(b.tenant AS tenant,b.id AS endpoint)) g');
END $$;

ALTER TABLE company ENABLE ROW LEVEL SECURITY;
DO $$ BEGIN
 PERFORM pg_temp.mapping_native('SELECT * FROM GRAPH_TABLE(g MATCH (a IS person)-[x IS work]->(b IS company)-[y IS offer]->(c IS product) COLUMNS(c.id AS endpoint)) g');
END $$;
ALTER TABLE company DISABLE ROW LEVEL SECURITY;

-- Canonical three-column edges also work; dropped fourth columns are not payloads.
CREATE TABLE simple(id bigint PRIMARY KEY,src bigint NOT NULL,dst bigint NOT NULL,discard text);
INSERT INTO simple SELECT id,src,dst,'unused' FROM link;
ALTER TABLE simple DROP COLUMN discard;
CREATE INDEX simple_out ON simple(src);
CREATE INDEX simple_in ON simple(dst);
CREATE PROPERTY GRAPH gs VERTEX TABLES(person KEY(id)) EDGE TABLES(simple KEY(id)
 SOURCE KEY(src) REFERENCES person(id) DESTINATION KEY(dst) REFERENCES person(id));
DO $$
BEGIN
 PERFORM pg_temp.mapping_check($q$SELECT * FROM GRAPH_TABLE(gs MATCH (a IS person)-[x IS simple]->(b IS person)
   -[y IS simple]->(a) COLUMNS(a.id AS a,b.id AS b,x.id AS x,y.id AS y)) gt$q$,
   'SELECT a.id AS a,b.id AS b,x.id AS x,y.id AS y FROM person a JOIN simple x ON x.src=a.id
    JOIN person b ON b.id=x.dst JOIN simple y ON y.src=b.id AND y.dst=a.id',true);
END $$;
DROP SCHEMA ggc_mappings CASCADE;
