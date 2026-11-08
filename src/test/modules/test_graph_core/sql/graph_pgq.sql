\set ECHO none
SET client_min_messages=warning;
CREATE SCHEMA ggc_pgq;
SET search_path=ggc_pgq,public;
CREATE TABLE v(id bigint PRIMARY KEY);
CREATE TABLE e(id bigint PRIMARY KEY,src bigint NOT NULL,dst bigint NOT NULL,payload text);
INSERT INTO v VALUES(1),(2),(3);
INSERT INTO e VALUES(11,1,2,NULL),(12,1,2,'parallel'),(13,2,3,'end'),
 (14,1,99,'absent'),(15,99,3,'absent source');
CREATE INDEX e_out ON e(src);
CREATE PROPERTY GRAPH g VERTEX TABLES(v KEY(id)) EDGE TABLES(e KEY(id)
 SOURCE KEY(src) REFERENCES v(id) DESTINATION KEY(dst) REFERENCES v(id));
DO $$
DECLARE plan text; line text; n bigint;
BEGIN
 PERFORM test_graph_cbo_config('v','e','e_out','g'::regclass,
   (SELECT oid FROM pg_propgraph_element WHERE pgepgid='g'::regclass AND pgekind='e'),
   77,7,11,1,7070,2,1);
 SET LOCAL enable_nestloop=off;
 SET LOCAL enable_hashjoin=off;
 SET LOCAL enable_mergejoin=off;
 plan := '';
 FOR line IN EXECUTE 'EXPLAIN SELECT * FROM GRAPH_TABLE(g
   MATCH (a IS v)-[x IS e]->(b IS v)-[y IS e]->(c IS v)
   COLUMNS(a.id AS a,x.id AS x,b.id AS b,y.id AS y,c.id AS c))'
 LOOP plan := plan || line; END LOOP;
 IF position('GraphExpandFixed' in plan)=0 THEN RAISE EXCEPTION 'graph plan not selected'; END IF;
 SELECT count(*) INTO n FROM GRAPH_TABLE(g
   MATCH (a IS v)-[x IS e]->(b IS v)-[y IS e]->(c IS v)
   COLUMNS(a.id AS a,x.id AS x,b.id AS b,y.id AS y,c.id AS c));
 IF n<>2 THEN RAISE EXCEPTION 'incorrect graph bag: %',n; END IF;
 DELETE FROM v WHERE id=2;
 SELECT count(*) INTO n FROM GRAPH_TABLE(g
   MATCH (a IS v)-[x IS e]->(b IS v)-[y IS e]->(c IS v)
   COLUMNS(a.id AS a,x.id AS x,b.id AS b,y.id AS y,c.id AS c));
 IF n<>0 THEN RAISE EXCEPTION 'invisible vertex escaped: %',n; END IF;
END $$;
RESET search_path;
DROP SCHEMA ggc_pgq CASCADE;
