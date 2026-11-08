\set ECHO none
SET client_min_messages=warning;
SET jit=off;
CREATE SCHEMA ggc_native;
SET search_path=ggc_native,public;
CREATE TABLE v(id text PRIMARY KEY,name text,tenant int);
CREATE TABLE e(id text PRIMARY KEY,src text,dst text,amount numeric,tenant int);
INSERT INTO v VALUES('a','first',1),('b','second',1),('c','hidden',2);
INSERT INTO e VALUES(1,'a','b',10,1),(2,'a','b',20,1),(3,'b','a',30,1),
 (4,'b','b',40,1),(5,NULL,'a',50,1),(6,'a',NULL,60,1),(7,'a','c',70,2);
CREATE INDEX e_src ON e(src);
CREATE PROPERTY GRAPH g VERTEX TABLES(v KEY(id)) EDGE TABLES(e KEY(id)
 SOURCE KEY(src) REFERENCES v(id) DESTINATION KEY(dst) REFERENCES v(id));
CREATE FUNCTION pg_temp.native_bag(q text) RETURNS jsonb LANGUAGE plpgsql AS $$
DECLARE r record; result jsonb='[]';
BEGIN
 FOR r IN EXECUTE q LOOP result:=result||jsonb_build_array(to_jsonb(r)); END LOOP;
 RETURN (SELECT coalesce(jsonb_agg(value ORDER BY value),'[]') FROM jsonb_array_elements(result));
END $$;
CREATE FUNCTION pg_temp.native_check(q text,oracle text) RETURNS void LANGUAGE plpgsql AS $$
DECLARE mode text; p jsonb; expected jsonb; graphs int;
BEGIN
 expected:=pg_temp.native_bag(oracle);
 FOREACH mode IN ARRAY ARRAY['relational','btree'] LOOP
   IF pg_temp.native_bag('/*+ GraphRoute('||mode||') */ '||q) IS DISTINCT FROM expected THEN
     RAISE EXCEPTION 'native graph bag mismatch: %, %',mode,q; END IF;
   EXECUTE 'EXPLAIN(ANALYZE,TIMING OFF,FORMAT JSON) /*+ GraphRoute('||mode||') */ '||q INTO p;
   WITH RECURSIVE plans(n) AS (SELECT p->0->'Plan' UNION ALL
     SELECT c FROM plans CROSS JOIN LATERAL jsonb_array_elements(n->'Plans') c)
   SELECT count(*) INTO graphs FROM plans WHERE n->>'Custom Plan Provider'='GraphExpandNative';
   IF (graphs>0)<>(mode='btree') THEN RAISE EXCEPTION 'typed graph not selected: %',p; END IF;
 END LOOP;
END $$;
DO $$
DECLARE q text; oracle text; pred text;
BEGIN
 FOREACH pred IN ARRAY ARRAY['true','a.id=''a''','a.id=NULL::text','false'] LOOP
   q:='SELECT * FROM GRAPH_TABLE(g MATCH (a IS v)-[x IS e]->(b IS v)-[y IS e]->(c IS v) WHERE '||pred||
      ' COLUMNS(a.name AS name,b.id AS mid,c.id AS endpoint,x.id AS x,y.id AS y)) gt';
   oracle:='SELECT a.name,b.id AS mid,c.id AS endpoint,x.id AS x,y.id AS y FROM v a JOIN e x ON x.src=a.id
     JOIN v b ON b.id=x.dst JOIN e y ON y.src=b.id JOIN v c ON c.id=y.dst WHERE '||pred;
   PERFORM pg_temp.native_check(q,oracle);
 END LOOP;
 PERFORM pg_temp.native_check($q$SELECT * FROM GRAPH_TABLE(g MATCH
   (a IS v)-[x IS e]->(b IS v) COLUMNS(a.name AS name,b.id AS endpoint,
   generate_series(1,2) AS n)) gt$q$,
   'SELECT a.name,b.id AS endpoint,generate_series(1,2) AS n FROM v a
     JOIN e x ON x.src=a.id JOIN v b ON b.id=x.dst');
 PERFORM pg_temp.native_check($q$SELECT r.id,t.* FROM (VALUES('a'::text),('b'),(NULL),('missing')) r(id)
   LEFT JOIN LATERAL (SELECT * FROM GRAPH_TABLE(g MATCH (a IS v WHERE a.id=r.id)-[x IS e]->(b IS v)
     COLUMNS(b.id AS endpoint,x.id AS edge)) gt) t ON true$q$,
   $q$SELECT r.id,t.* FROM (VALUES('a'::text),('b'),(NULL),('missing')) r(id)
     LEFT JOIN LATERAL (SELECT b.id AS endpoint,x.id AS edge FROM v a JOIN e x ON x.src=a.id
       JOIN v b ON b.id=x.dst WHERE a.id=r.id) t ON true$q$);
END $$;
CREATE VIEW vv WITH(security_barrier=true) AS SELECT * FROM v WHERE tenant=1;
CREATE PROPERTY GRAPH gv VERTEX TABLES(vv KEY(id)) EDGE TABLES(e KEY(id)
 SOURCE KEY(src) REFERENCES vv(id) DESTINATION KEY(dst) REFERENCES vv(id));
DO $$ BEGIN
 PERFORM pg_temp.native_check($q$SELECT * FROM GRAPH_TABLE(gv MATCH (a IS vv)-[x IS e]->(b IS vv)
 COLUMNS(a.name AS name,b.id AS endpoint)) gt$q$,
 'SELECT a.name,b.id AS endpoint FROM vv a JOIN e x ON x.src=a.id JOIN vv b ON b.id=x.dst');
END $$;
CREATE ROLE regress_ggc_native;
GRANT USAGE ON SCHEMA ggc_native TO regress_ggc_native;
GRANT SELECT ON v,e,g TO regress_ggc_native;
ALTER TABLE v ENABLE ROW LEVEL SECURITY;
ALTER TABLE e ENABLE ROW LEVEL SECURITY;
CREATE POLICY vp ON v USING(tenant=1);
CREATE POLICY ep ON e USING(tenant=1);
SET ROLE regress_ggc_native;
DO $$ BEGIN
 PERFORM pg_temp.native_check($q$SELECT * FROM GRAPH_TABLE(g MATCH (a IS v)-[x IS e]->(b IS v)
 COLUMNS(a.name AS name,b.id AS endpoint,x.id AS edge)) gt$q$,
 'SELECT a.name,b.id AS endpoint,x.id AS edge FROM v a JOIN e x ON x.src=a.id JOIN v b ON b.id=x.dst');
END $$;
RESET ROLE;
-- Composite mapping with duplicate local id values in different tenants.
CREATE TABLE vc(tenant int,id text,name text,PRIMARY KEY(tenant,id));
CREATE TABLE ec(id text PRIMARY KEY,st int,src text,dt int,dst text);
INSERT INTO vc VALUES(1,'a','one'),(2,'a','two');
INSERT INTO ec VALUES(1,1,'a',2,'a'),(2,NULL,'a',2,'a'),(3,1,'a',2,NULL);
CREATE PROPERTY GRAPH gc VERTEX TABLES(vc KEY(tenant,id)) EDGE TABLES(ec KEY(id)
 SOURCE KEY(st,src) REFERENCES vc(tenant,id) DESTINATION KEY(dt,dst) REFERENCES vc(tenant,id));
DO $$ BEGIN
 PERFORM pg_temp.native_check($q$SELECT * FROM GRAPH_TABLE(gc MATCH (a IS vc)-[x IS ec]->(b IS vc)
 COLUMNS(a.name AS origin,b.name AS destination)) gt$q$,
 'SELECT a.name AS origin,b.name AS destination FROM vc a JOIN ec x ON x.st=a.tenant AND x.src=a.id
 JOIN vc b ON b.tenant=x.dt AND b.id=x.dst');
END $$;
RESET search_path;
RESET jit;
DROP SCHEMA ggc_native CASCADE;
DROP ROLE regress_ggc_native;
