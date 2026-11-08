\set ECHO none
SET client_min_messages=warning;
SET jit=off;
CREATE SCHEMA ggc_integer_acl;
SET search_path=ggc_integer_acl,public;

-- Standalone fixtures; no LOAD and no casts that change graph key semantics.
DO $$
DECLARE suffix text; vt text; et text; st text; dt text;
BEGIN
 FOR suffix,vt,et,st,dt IN VALUES
   ('i2','smallint','smallint','smallint','smallint'),
   ('i4','integer','integer','integer','integer'),
   ('i8','bigint','bigint','bigint','bigint'),
   ('wide','bigint','smallint','smallint','integer'),
   ('narrow','smallint','bigint','integer','bigint')
 LOOP
   EXECUTE format('CREATE TABLE v_%s(id %s PRIMARY KEY,name text,secret text)',suffix,vt);
   EXECUTE format('CREATE TABLE e_%s(id %s PRIMARY KEY,src %s NOT NULL,dst %s NOT NULL,
     amount numeric,note text,secret text)',suffix,et,st,dt);
   EXECUTE format($q$INSERT INTO v_%s VALUES
     (-32768,'min2','hidden'),(-2,'A','hidden'),(-1,'B','hidden'),(0,'C','hidden'),
     (1,'D','hidden'),(2,'E','hidden'),(3,'F','hidden'),(32767,'max2','hidden')$q$,suffix);
   IF vt<>'smallint' THEN
     EXECUTE format($q$INSERT INTO v_%s VALUES(-2147483648,'min4','hidden'),(2147483647,'max4','hidden')$q$,suffix);
   END IF;
   IF vt='bigint' THEN
     EXECUTE format($q$INSERT INTO v_%s VALUES(-9223372036854775808,'min8','hidden'),
       (9223372036854775807,'max8','hidden'),(2147483648,'outside4','hidden')$q$,suffix);
   END IF;
   EXECUTE format($q$INSERT INTO e_%s VALUES
     (-32768,-32768,-2,1,'min','hidden'),(-2,-2,-1,10,'parallel1','hidden'),
     (-1,-2,-1,20,'parallel2','hidden'),(0,-1,0,30,'zero','hidden'),
     (1,0,1,NULL,'null','hidden'),(2,1,2,40,'two','hidden'),(3,1,3,50,'three','hidden'),
     (4,3,-2,60,'cycle','hidden'),(5,0,0,70,'loop','hidden'),
     (6,0,99,80,'orphan','hidden'),(7,99,0,90,'missing-source','hidden'),
     (32767,2,32767,100,'max','hidden')$q$,suffix);
   IF suffix='i4' OR suffix='i8' THEN
     EXECUTE format($q$INSERT INTO e_%s VALUES(-2147483648,-2147483648,-2,1,'min4','hidden'),
       (2147483647,2,2147483647,1,'max4','hidden')$q$,suffix);
   END IF;
   IF suffix='i8' THEN
     EXECUTE $q$INSERT INTO e_i8 VALUES(-9223372036854775808,-9223372036854775808,-2,1,'min8','hidden'),
       (9223372036854775807,2,9223372036854775807,1,'max8','hidden')$q$;
   END IF;
   IF suffix='narrow' THEN
     INSERT INTO e_narrow VALUES(100,2147483647,-2,1,'unmatched-source','hidden'),
       (101,0,9223372036854775807,1,'unmatched-destination','hidden');
   END IF;
   EXECUTE format('CREATE INDEX ON e_%s(src) INCLUDE(id,dst)',suffix);
   EXECUTE format('CREATE INDEX ON e_%s(dst) INCLUDE(id,src)',suffix);
   EXECUTE format('CREATE PROPERTY GRAPH g_%1$s VERTEX TABLES(v_%1$s KEY(id)
     LABEL v PROPERTIES(id AS vid,name,secret)) EDGE TABLES(e_%1$s KEY(id)
     SOURCE KEY(src) REFERENCES v_%1$s(id) DESTINATION KEY(dst) REFERENCES v_%1$s(id)
     LABEL e PROPERTIES(id AS eid,src,dst,amount,note,secret AS edge_secret))',suffix);
   EXECUTE format('ANALYZE v_%s',suffix);
   EXECUTE format('ANALYZE e_%s',suffix);
 END LOOP;
END $$;
-- Two graph edge elements, one heap, genuinely different endpoint mappings.
CREATE PROPERTY GRAPH g_alias VERTEX TABLES(v_i8 KEY(id) LABEL v PROPERTIES(id AS vid,name))
 EDGE TABLES(
 e_i8 AS forwards KEY(id) SOURCE KEY(src) REFERENCES v_i8(id) DESTINATION KEY(dst) REFERENCES v_i8(id)
   LABEL f PROPERTIES(id AS eid,src,dst,amount,note),
 e_i8 AS backwards KEY(id) SOURCE KEY(dst) REFERENCES v_i8(id) DESTINATION KEY(src) REFERENCES v_i8(id)
   LABEL r PROPERTIES(id AS eid,src,dst,amount,note));
VACUUM ANALYZE;

CREATE FUNCTION pg_temp.integration_bag(q text) RETURNS jsonb LANGUAGE plpgsql AS $$
DECLARE r record; result jsonb='[]';
BEGIN
 FOR r IN EXECUTE q LOOP result:=result||jsonb_build_array(to_jsonb(r)); END LOOP;
 RETURN (SELECT coalesce(jsonb_agg(value ORDER BY value),'[]') FROM jsonb_array_elements(result));
END $$;
CREATE FUNCTION pg_temp.integration_check(q text,oracle text) RETURNS void LANGUAGE plpgsql AS $$
DECLARE expected jsonb; p jsonb; graph_count int; budget int;
BEGIN
 expected:=pg_temp.integration_bag(oracle);
 IF pg_temp.integration_bag('/*+ GraphRoute(relational) */ '||q) IS DISTINCT FROM expected THEN
   RAISE EXCEPTION 'relational oracle mismatch: %',q;
 END IF;
 FOREACH budget IN ARRAY ARRAY[1,8] LOOP
   PERFORM set_config('graph_btree_max_candidates',budget::text,true);
   IF pg_temp.integration_bag('/*+ GraphRoute(btree) */ '||q) IS DISTINCT FROM expected THEN
     RAISE EXCEPTION 'graph oracle mismatch: %, %',budget,q;
   END IF;
   EXECUTE 'EXPLAIN(ANALYZE,TIMING OFF,FORMAT JSON) /*+ GraphRoute(btree) */ '||q INTO p;
   WITH RECURSIVE plans(n) AS (SELECT p->0->'Plan' UNION ALL
     SELECT c FROM plans CROSS JOIN LATERAL jsonb_array_elements(n->'Plans') c)
   SELECT count(*) INTO graph_count FROM plans WHERE n->>'Custom Plan Provider'='GraphExpandFixed';
   IF graph_count=0 THEN RAISE EXCEPTION 'graph route not selected: %, %',q,p; END IF;
 END LOOP;
END $$;
CREATE FUNCTION pg_temp.integration_denied(q text) RETURNS void LANGUAGE plpgsql AS $$
DECLARE mode text; denied boolean;
BEGIN
 FOREACH mode IN ARRAY ARRAY['relational','btree'] LOOP
   denied:=false;
   BEGIN EXECUTE '/*+ GraphRoute('||mode||') */ '||q;
   EXCEPTION WHEN insufficient_privilege OR feature_not_supported THEN denied:=true;
   END;
   IF NOT denied THEN RAISE EXCEPTION 'unauthorized query succeeded: %, %',mode,q; END IF;
 END LOOP;
END $$;
-- Multi-label UNION now preserves one graph region per native branch.

-- Direction, signed/range boundary, exact bag, typed projection and filters.
DO $$
DECLARE suffix text; d1 text; d2 text; j1 text; j2 text; pred text; cols text; q text; oracle text;
BEGIN
 FOREACH suffix IN ARRAY ARRAY['i2','i4','i8','wide','narrow'] LOOP
 FOREACH d1 IN ARRAY ARRAY['out','in','both'] LOOP
 FOREACH d2 IN ARRAY ARRAY['out','in','both'] LOOP
   j1:=CASE d1 WHEN 'out' THEN 'a.id=x.src AND x.dst=b.id' WHEN 'in' THEN 'a.id=x.dst AND x.src=b.id'
     ELSE '(a.id=x.src AND x.dst=b.id OR a.id=x.dst AND x.src=b.id)' END;
   j2:=CASE d2 WHEN 'out' THEN 'b.id=y.src AND y.dst=c.id' WHEN 'in' THEN 'b.id=y.dst AND y.src=c.id'
     ELSE '(b.id=y.src AND y.dst=c.id OR b.id=y.dst AND y.src=c.id)' END;
   cols:='a.vid AS s,b.vid AS m,c.vid AS t,x.eid AS x,y.eid AS y,
     pg_typeof(a.vid) AS st,pg_typeof(x.eid) AS xt,x.src AS xs,x.dst AS xd';
   FOREACH pred IN ARRAY ARRAY['true','a.vid=-2','c.vid=0 AND x.amount>10','y.amount>x.amount'] LOOP
     q:=format('SELECT * FROM GRAPH_TABLE(g_%s MATCH (a IS v)%s(b IS v)%s(c IS v) WHERE %s COLUMNS(%s)) gt',
       suffix,CASE d1 WHEN 'out' THEN '-[x IS e]->' WHEN 'in' THEN '<-[x IS e]-' ELSE '-[x IS e]-' END,
       CASE d2 WHEN 'out' THEN '-[y IS e]->' WHEN 'in' THEN '<-[y IS e]-' ELSE '-[y IS e]-' END,pred,cols);
     oracle:=format('SELECT %s FROM v_%s a,e_%s x,v_%s b,e_%s y,v_%s c WHERE %s AND %s AND %s',
       replace(replace(cols,'.vid','.id'),'.eid','.id'),suffix,suffix,suffix,suffix,suffix,j1,j2,
       replace(pred,'.vid','.id'));
     PERFORM pg_temp.integration_check(q,oracle);
     PERFORM pg_temp.integration_check('SELECT count(*) FROM ('||q||') z','SELECT count(*) FROM ('||oracle||') z');
   END LOOP;
 END LOOP; END LOOP;
 -- Endpoint sets must also work with integer array/range scans and coverage.
 PERFORM pg_temp.integration_check(format($q$SELECT DISTINCT endpoint FROM GRAPH_TABLE(g_%s MATCH
   (a IS v WHERE a.vid=-2)-[x IS e]->(b IS v)-[y IS e]->(c IS v)-[z IS e]->(d IS v)
   COLUMNS(d.vid AS endpoint)) gt$q$,suffix),format('SELECT DISTINCT d.id AS endpoint FROM v_%1$s a
   JOIN e_%1$s x ON a.id=x.src JOIN v_%1$s b ON b.id=x.dst JOIN e_%1$s y ON y.src=b.id
   JOIN v_%1$s c ON c.id=y.dst JOIN e_%1$s z ON z.src=c.id JOIN v_%1$s d ON d.id=z.dst WHERE a.id=-2',suffix));
 END LOOP;
END $$;

-- Exact element identity, not an arbitrary catalog row for the same heap.
DO $$
DECLARE l1 text; l2 text; j1 text; j2 text;
BEGIN
 FOREACH l1 IN ARRAY ARRAY['f','r'] LOOP
 FOREACH l2 IN ARRAY ARRAY['f','r'] LOOP
   j1:=CASE l1 WHEN 'f' THEN 'a.id=x.src AND x.dst=b.id' ELSE 'a.id=x.dst AND x.src=b.id' END;
   j2:=CASE l2 WHEN 'f' THEN 'b.id=y.src AND y.dst=c.id' ELSE 'b.id=y.dst AND y.src=c.id' END;
   PERFORM pg_temp.integration_check(format('SELECT * FROM GRAPH_TABLE(g_alias MATCH
     (a IS v)-[x IS %s]->(b IS v)-[y IS %s]->(c IS v)
     COLUMNS(a.vid AS a,b.vid AS b,c.vid AS c,x.eid AS x,y.eid AS y,x.src AS src,x.dst AS dst)) gt',l1,l2),
     format('SELECT a.id AS a,b.id AS b,c.id AS c,x.id AS x,y.id AS y,x.src,x.dst
       FROM v_i8 a,e_i8 x,v_i8 b,e_i8 y,v_i8 c WHERE %s AND %s',j1,j2));
 END LOOP; END LOOP;
 PERFORM pg_temp.integration_check($q$SELECT * FROM GRAPH_TABLE(g_alias MATCH
   (a IS v)-[x IS f|r]->(b IS v) COLUMNS(a.vid AS a,b.vid AS b,x.eid AS x)) gt$q$,
   'SELECT a.id AS a,b.id AS b,x.id AS x FROM v_i8 a JOIN e_i8 x ON a.id=x.src JOIN v_i8 b ON b.id=x.dst
    UNION ALL SELECT a.id,b.id,x.id FROM v_i8 a JOIN e_i8 x ON a.id=x.dst JOIN v_i8 b ON b.id=x.src');
END $$;

-- No whole-table grant, including an unreferenced fourth-column payload.
CREATE ROLE regress_graph_columns;
GRANT USAGE ON SCHEMA ggc_integer_acl TO regress_graph_columns;
GRANT SELECT ON g_i4,g_alias TO regress_graph_columns;
GRANT SELECT(id,name) ON v_i4,v_i8 TO regress_graph_columns;
GRANT SELECT(id,src,dst) ON e_i4,e_i8 TO regress_graph_columns;
SET ROLE regress_graph_columns;
DO $$ BEGIN
 PERFORM pg_temp.integration_check($q$SELECT * FROM GRAPH_TABLE(g_i4 MATCH
   (a IS v)-[x IS e]->(b IS v)-[y IS e]->(c IS v) COLUMNS(a.vid AS a,c.vid AS c,y.eid AS y)) gt$q$,
   'SELECT a.id AS a,c.id AS c,y.id AS y FROM v_i4 a JOIN e_i4 x ON x.src=a.id
    JOIN v_i4 b ON b.id=x.dst JOIN e_i4 y ON y.src=b.id JOIN v_i4 c ON c.id=y.dst');
 PERFORM pg_temp.integration_check($q$SELECT * FROM GRAPH_TABLE(g_alias MATCH
   (a IS v)-[x IS r]->(b IS v) COLUMNS(a.name AS a,b.name AS b,x.eid AS x)) gt$q$,
   'SELECT a.name AS a,b.name AS b,x.id AS x FROM v_i8 a JOIN e_i8 x ON a.id=x.dst JOIN v_i8 b ON b.id=x.src');
 PERFORM pg_temp.integration_denied($q$SELECT * FROM GRAPH_TABLE(g_i4 MATCH
   (a IS v)-[x IS e]->(b IS v) COLUMNS(x.amount AS amount)) gt$q$);
END $$;
RESET ROLE;
GRANT SELECT(amount,note) ON e_i4 TO regress_graph_columns;
SET ROLE regress_graph_columns;
DO $$ BEGIN
 PERFORM pg_temp.integration_check($q$SELECT * FROM GRAPH_TABLE(g_i4 MATCH
   (a IS v)-[x IS e WHERE x.amount>10]->(b IS v WHERE b.name<>'hidden')-[y IS e]->(c IS v)
   WHERE y.amount>x.amount COLUMNS(a.name AS a,c.name AS c,x.amount AS amount,y.note AS note)) gt$q$,
   'SELECT a.name AS a,c.name AS c,x.amount AS amount,y.note AS note FROM v_i4 a JOIN e_i4 x ON x.src=a.id
    JOIN v_i4 b ON b.id=x.dst JOIN e_i4 y ON y.src=b.id JOIN v_i4 c ON c.id=y.dst
    WHERE x.amount>10 AND b.name<>''hidden'' AND y.amount>x.amount');
 PERFORM pg_temp.integration_denied($q$SELECT * FROM GRAPH_TABLE(g_i4 MATCH
   (a IS v)-[x IS e]->(b IS v) COLUMNS(b.secret AS secret)) gt$q$);
 PERFORM pg_temp.integration_denied($q$SELECT * FROM GRAPH_TABLE(g_i4 MATCH
   (a IS v)-[x IS e WHERE x.edge_secret='hidden']->(b IS v) COLUMNS(b.vid AS id)) gt$q$);
END $$;
PREPARE column_plan AS /*+ GraphRoute(btree) */ SELECT * FROM GRAPH_TABLE(g_i4 MATCH
 (a IS v)-[x IS e]->(b IS v) COLUMNS(b.name AS name,x.note AS note)) gt;
DO $$ BEGIN EXECUTE 'EXECUTE column_plan'; END $$;
RESET ROLE;
REVOKE SELECT(note) ON e_i4 FROM regress_graph_columns;
SET ROLE regress_graph_columns;
DO $$ DECLARE denied boolean=false; BEGIN
 BEGIN EXECUTE 'EXECUTE column_plan';
 EXCEPTION WHEN insufficient_privilege OR feature_not_supported THEN denied:=true; END;
 IF NOT denied THEN RAISE EXCEPTION 'cached plan ignored revoked column grant'; END IF;
END $$;
RESET ROLE;
DEALLOCATE column_plan;
DROP OWNED BY regress_graph_columns;
DROP ROLE regress_graph_columns;
DROP SCHEMA ggc_integer_acl CASCADE;
RESET search_path;
