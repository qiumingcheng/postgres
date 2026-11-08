\set ECHO none
CREATE FUNCTION pg_temp.graph_plan_metadata() RETURNS boolean
AS '$libdir/test_graph_core', 'test_graph_plan_metadata' LANGUAGE C;
CREATE FUNCTION pg_temp.graph_custom_parameters() RETURNS boolean
AS '$libdir/test_graph_core', 'test_graph_custom_parameters' LANGUAGE C;
CREATE FUNCTION pg_temp.graph_native_metadata() RETURNS boolean
AS '$libdir/test_graph_core', 'test_graph_native_metadata' LANGUAGE C;
DO $$BEGIN
 IF NOT pg_temp.graph_plan_metadata() OR NOT pg_temp.graph_custom_parameters()
    OR NOT pg_temp.graph_native_metadata() THEN
   RAISE EXCEPTION 'graph plan metadata validation failed';
 END IF;
END $$;

-- No concrete provider is required by the C codec tests above.  When the
-- B-tree test fragment is present, also exercise real cached Native Plans.
SET client_min_messages=warning;
DO $$
DECLARE mode text; result bigint; p jsonb;
BEGIN
 IF NOT EXISTS (SELECT FROM pg_proc WHERE proname='test_graph_btree_provider') THEN
   RETURN;
 END IF;
 CREATE SCHEMA ggc_native_plan;
 CREATE TABLE ggc_native_plan.v(id text PRIMARY KEY, name text);
 CREATE TABLE ggc_native_plan.e(id integer PRIMARY KEY, src text, dst text);
 INSERT INTO ggc_native_plan.v VALUES('a','Alice'),('b','Bob'),('c','Carol');
 INSERT INTO ggc_native_plan.e VALUES(1,'a','b'),(2,'a','b'),(3,'b','c');
 CREATE INDEX ON ggc_native_plan.e(src);
 EXECUTE 'CREATE PROPERTY GRAPH ggc_native_plan.g
   VERTEX TABLES(ggc_native_plan.v KEY(id) LABEL v PROPERTIES(id,name))
   EDGE TABLES(ggc_native_plan.e KEY(id)
   SOURCE KEY(src) REFERENCES v(id)
   DESTINATION KEY(dst) REFERENCES v(id)
   LABEL e PROPERTIES(id AS edgeid))';
 EXECUTE $q$PREPARE native_plan_test(text) AS /*+ GraphRoute(btree) */
   SELECT count(*) FROM GRAPH_TABLE(ggc_native_plan.g MATCH
   (a IS v WHERE a.id=$1)-[x IS e]->(b IS v)
   COLUMNS(b.name AS name,x.edgeid AS edgeid)) gt$q$;
 FOREACH mode IN ARRAY ARRAY['force_generic_plan','force_custom_plan'] LOOP
   PERFORM set_config('plan_cache_mode',mode,true);
   EXECUTE 'EXECUTE native_plan_test(''a'')' INTO result;
   IF result <> 2 THEN RAISE EXCEPTION 'native cached bag changed: %',mode; END IF;
   EXECUTE 'EXECUTE native_plan_test(''b'')' INTO result;
   IF result <> 1 THEN RAISE EXCEPTION 'native cached parameter changed: %',mode; END IF;
   EXECUTE 'EXECUTE native_plan_test(NULL)' INTO result;
   IF result <> 0 THEN RAISE EXCEPTION 'native cached NULL parameter changed: %',mode; END IF;
   EXECUTE 'EXPLAIN(VERBOSE, FORMAT JSON) EXECUTE native_plan_test(''a'')' INTO p;
   IF p::text NOT LIKE '%GraphExpandNative%' THEN
     RAISE EXCEPTION 'native cached route missing: %',p;
   END IF;
 END LOOP;
 ALTER TABLE ggc_native_plan.v ADD COLUMN extra text;
 INSERT INTO ggc_native_plan.e VALUES(4,'a','b');
 FOREACH mode IN ARRAY ARRAY['force_generic_plan','force_custom_plan'] LOOP
   PERFORM set_config('plan_cache_mode',mode,true);
   EXECUTE 'EXECUTE native_plan_test(''a'')' INTO result;
   IF result <> 3 THEN RAISE EXCEPTION 'native cached DDL/data invalidation changed: %',mode; END IF;
 END LOOP;
 DEALLOCATE native_plan_test;
 DROP SCHEMA ggc_native_plan CASCADE;
END $$;
RESET client_min_messages;
