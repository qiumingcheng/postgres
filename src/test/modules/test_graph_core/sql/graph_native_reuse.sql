\set ECHO none
SET client_min_messages = warning;
SET jit = off;
SET max_parallel_workers_per_gather = 0;
CREATE SCHEMA ggc_native_reuse;
SET search_path = ggc_native_reuse, public;

-- Text keys deliberately choose typed native access rather than the integer
-- provider.  Distinct edges share destination keys, but their bag multiplicity
-- must remain intact when an access child is cached.
CREATE TABLE v(id text COLLATE "C" PRIMARY KEY, name text, tenant int);
CREATE TABLE e(id int PRIMARY KEY, src text COLLATE "C", dst text COLLATE "C");
INSERT INTO v SELECT 'v' || i, repeat('name-' || i, 3), i % 2
FROM generate_series(1, 400) i;
INSERT INTO e SELECT i, 'v1', 'v' || (2 + i % 5)
FROM generate_series(1, 4000) i;
INSERT INTO e VALUES (4001, NULL, 'v2'), (4002, 'v1', NULL),
                     (4003, 'v2', 'v3'), (4004, 'v2', 'v4'),
                     (4005, 'v1', NULL);
CREATE INDEX e_src ON e(src);
CREATE PROPERTY GRAPH g VERTEX TABLES (v KEY(id)) EDGE TABLES (e KEY(id)
 SOURCE KEY(src) REFERENCES v(id) DESTINATION KEY(dst) REFERENCES v(id)
 PROPERTIES(id AS edgeid));
ANALYZE v;
ANALYZE e;

CREATE FUNCTION pg_temp.reuse_bag(q text, route text) RETURNS jsonb
LANGUAGE plpgsql AS $$
DECLARE result jsonb;
BEGIN
 EXECUTE '/*+ GraphRoute(' || route || ') */ SELECT coalesce('
   || 'jsonb_agg(to_jsonb(r) ORDER BY to_jsonb(r)), ''[]''::jsonb) FROM ('
   || q || ') r' INTO result;
 RETURN result;
END $$;
CREATE FUNCTION pg_temp.reuse_plan(q text) RETURNS jsonb LANGUAGE plpgsql AS $$
DECLARE result jsonb; line text;
BEGIN
 EXECUTE 'EXPLAIN (ANALYZE, VERBOSE, TIMING OFF, FORMAT JSON) /*+ GraphRoute(btree) */ '
   || q INTO result;
 -- No query supplied to this helper has an external $n parameter.  A $n
 -- here is therefore an unresolved internal PARAM_EXEC, not valid SQL.
 IF result::text ~ '\$[0-9]+' THEN
   RAISE EXCEPTION 'native child EXPLAIN lost a parameter source: %', result;
 END IF;
 FOR line IN EXECUTE 'EXPLAIN (VERBOSE, COSTS OFF, FORMAT TEXT) /*+ GraphRoute(btree) */ ' || q LOOP
   IF line ~ '\$[0-9]+' THEN
     RAISE EXCEPTION 'native text EXPLAIN lost a parameter source: %', line;
   END IF;
 END LOOP;
 RETURN result;
END $$;
CREATE FUNCTION pg_temp.reuse_nodes(p jsonb) RETURNS SETOF jsonb LANGUAGE sql AS $$
 WITH RECURSIVE plans(n) AS (
   SELECT p->0->'Plan'
   UNION ALL
   SELECT child FROM plans CROSS JOIN LATERAL jsonb_array_elements(n->'Plans') child
 ) SELECT n FROM plans
$$;
CREATE FUNCTION pg_temp.reuse_native_nodes(p jsonb) RETURNS SETOF jsonb LANGUAGE sql AS $$
 SELECT child FROM pg_temp.reuse_nodes(p) native
 CROSS JOIN LATERAL pg_temp.reuse_nodes(
   jsonb_build_array(jsonb_build_object('Plan', native))) child
 WHERE native->>'Custom Plan Provider' = 'GraphExpandNative'
$$;
CREATE FUNCTION pg_temp.reuse_compare(q text) RETURNS void LANGUAGE plpgsql AS $$
BEGIN
 IF pg_temp.reuse_bag(q, 'btree') IS DISTINCT FROM pg_temp.reuse_bag(q, 'relational') THEN
   RAISE EXCEPTION 'native reuse changed the result bag: %', q;
 END IF;
END $$;

DO $$
DECLARE
 q text := $q$SELECT * FROM GRAPH_TABLE(g MATCH
   (a IS v WHERE a.id = 'v1')-[x IS e]->(b IS v)
   COLUMNS(x.edgeid AS edge, b.id AS destination, b.name AS name)) gt$q$;
 p jsonb;
BEGIN
 PERFORM set_config('work_mem', '64kB', true);
 PERFORM pg_temp.reuse_compare(q);
 p := pg_temp.reuse_plan(q);
 IF NOT EXISTS (SELECT FROM pg_temp.reuse_nodes(p) n
     WHERE n->>'Custom Plan Provider' = 'GraphExpandNative') OR
    NOT EXISTS (SELECT FROM pg_temp.reuse_native_nodes(p) n
     WHERE n->>'Node Type' = 'Memoize' AND (n->>'Cache Hits')::bigint > 0) THEN
   RAISE EXCEPTION 'expected an actual native Memoize child with cache hits: %', p;
 END IF;
 -- The graph hint is not a request to enable ordinary Nested Loop joins.
 PERFORM set_config('enable_nestloop', 'off', true);
 PERFORM pg_temp.reuse_compare(q);
 p := pg_temp.reuse_plan(q);
 IF NOT EXISTS (SELECT FROM pg_temp.reuse_nodes(p) n WHERE n->>'Node Type' = 'Memoize') THEN
   RAISE EXCEPTION 'disabling ordinary joins disabled graph access reuse: %', p;
 END IF;
 PERFORM set_config('enable_nestloop', 'on', true);
 PERFORM set_config('enable_memoize', 'off', true);
 p := pg_temp.reuse_plan(q);
 IF EXISTS (SELECT FROM pg_temp.reuse_nodes(p) n WHERE n->>'Node Type' = 'Memoize') THEN
   RAISE EXCEPTION 'graph access ignored enable_memoize: %', p;
 END IF;
 PERFORM pg_temp.reuse_compare(q);

 -- With parameterized index access disabled, the same native policy should
 -- consider the unparameterized cheapest scan plus real Materialize node.
 PERFORM set_config('enable_indexscan', 'off', true);
 PERFORM set_config('enable_indexonlyscan', 'off', true);
 PERFORM set_config('enable_bitmapscan', 'off', true);
 p := pg_temp.reuse_plan(q);
 IF NOT EXISTS (SELECT FROM pg_temp.reuse_nodes(p) n WHERE n->>'Node Type' = 'Materialize') THEN
   RAISE EXCEPTION 'expected a cost-selected native Materialize child: %', p;
 END IF;
 PERFORM pg_temp.reuse_compare(q);
 PERFORM set_config('enable_material', 'off', true);
 p := pg_temp.reuse_plan(q);
 IF EXISTS (SELECT FROM pg_temp.reuse_nodes(p) n WHERE n->>'Node Type' = 'Materialize') THEN
   RAISE EXCEPTION 'graph access ignored enable_material: %', p;
 END IF;
 PERFORM pg_temp.reuse_compare(q);
END $$;

-- A non-key outer parameter must invalidate the child cache when the whole
-- graph is rescanned.  Include changed values, a repeat, NULL and an empty
-- binding.  The retained edge ids expose accidental DISTINCT or stale reuse.
DO $$
DECLARE q text := $q$SELECT r.src, r.maxedge, r.tenant, t.*
 FROM (VALUES ('v1'::text, 1000, 0), ('v1', 1000, 1), ('v2', 4004, 1), ('v1', 2000, 1),
              ('v1', 1000, 0), (NULL, 10, 1), ('absent', 20, 0)) r(src, maxedge, tenant)
 LEFT JOIN LATERAL (
   SELECT * FROM GRAPH_TABLE(g MATCH
     (a IS v WHERE a.id = r.src)-[x IS e WHERE x.edgeid <= r.maxedge]->
     (b IS v WHERE b.tenant = r.tenant)
     COLUMNS(x.edgeid AS edge, b.id AS destination)) gt OFFSET 0
 ) t ON true$q$;
 p jsonb;
BEGIN
 PERFORM pg_temp.reuse_compare(q);
 p := pg_temp.reuse_plan(q);
 IF NOT EXISTS (SELECT FROM pg_temp.reuse_native_nodes(p) n
     WHERE n->>'Node Type' = 'Memoize' AND (n->>'Cache Hits')::bigint > 0) OR
    NOT EXISTS (SELECT FROM pg_temp.reuse_nodes(p) n
     WHERE n->>'Custom Plan Provider' = 'GraphExpandNative'
       AND (n->>'Graph Rescans')::bigint > 0) THEN
   RAISE EXCEPTION 'correlated native cache invalidation was not exercised: %', p;
 END IF;
END $$;

-- Composite cache keys: the same local text id belongs to two tenants.
CREATE TABLE vc(tenant int, id text COLLATE "C", name text, PRIMARY KEY(tenant,id));
CREATE TABLE ec(id int PRIMARY KEY, st int, src text COLLATE "C",
                dt int, dst text COLLATE "C");
INSERT INTO vc SELECT t, 'v' || i, t || ':' || i
FROM generate_series(1, 2) t CROSS JOIN generate_series(1, 200) i;
INSERT INTO ec SELECT i, 1, 'v1', 1 + i % 2, 'v' || (2 + i % 5)
FROM generate_series(1, 4000) i;
INSERT INTO ec VALUES (4001, 1, 'v1', NULL, 'v2'), (4002, 1, 'v1', 2, NULL),
                      (4003, 1, 'v1', NULL, 'v2'), (4004, 1, 'v1', 2, NULL);
CREATE INDEX ec_src ON ec(st, src);
CREATE PROPERTY GRAPH gc VERTEX TABLES (vc KEY(tenant,id)) EDGE TABLES (ec KEY(id)
 SOURCE KEY(st,src) REFERENCES vc(tenant,id)
 DESTINATION KEY(dt,dst) REFERENCES vc(tenant,id) PROPERTIES(id AS edgeid));
ANALYZE vc;
ANALYZE ec;
DO $$
DECLARE q text := $q$SELECT * FROM GRAPH_TABLE(gc MATCH
 (a IS vc WHERE a.id = 'v1' AND a.tenant = 1)-[x IS ec]->(b IS vc)
 COLUMNS(x.edgeid AS edge, b.tenant AS tenant, b.id AS destination, b.name AS name)) gt$q$;
 p jsonb;
BEGIN
 PERFORM pg_temp.reuse_compare(q);
 p := pg_temp.reuse_plan(q);
 IF NOT EXISTS (SELECT FROM pg_temp.reuse_nodes(p) n
     WHERE n->>'Node Type' = 'Memoize' AND (n->>'Cache Hits')::bigint > 0) THEN
   RAISE EXCEPTION 'composite native cache path was not exercised: %', p;
 END IF;
END $$;

-- Volatile predicates must not be hidden by parameter-result memoization.
CREATE SEQUENCE visits;
CREATE FUNCTION visit(text) RETURNS boolean LANGUAGE plpgsql VOLATILE AS $$
BEGIN
 PERFORM nextval('ggc_native_reuse.visits');
 RETURN $1 IS NOT NULL;
END $$;
DO $$
DECLARE q text := $q$SELECT * FROM GRAPH_TABLE(g MATCH
 (a IS v WHERE a.id = 'v1')-[x IS e]->(b IS v WHERE visit(b.id))
 COLUMNS(x.edgeid AS edge, b.id AS destination)) gt$q$;
 p jsonb;
BEGIN
 PERFORM set_config('enable_material', 'off', true);
 PERFORM set_config('enable_seqscan', 'off', true);
 PERFORM pg_temp.reuse_compare(q);
 p := pg_temp.reuse_plan(q);
 IF EXISTS (SELECT FROM pg_temp.reuse_nodes(p) n WHERE n->>'Node Type' = 'Memoize') THEN
   RAISE EXCEPTION 'volatile access was memoized: %', p;
 END IF;
END $$;

-- Pausing in the middle of a cache entry must not mark a partial bag as
-- complete.  Compare every row fetched through a cursor with a complete run.
DO $$
DECLARE
 q text := $q$SELECT * FROM GRAPH_TABLE(g MATCH
   (a IS v)-[x IS e]->(b IS v)-[y IS e]->(c IS v)
   COLUMNS(x.edgeid AS x, y.edgeid AS y, c.id AS destination)) gt$q$;
 c refcursor;
 r record;
 rows jsonb := '[]';
 expected jsonb;
BEGIN
 expected := pg_temp.reuse_bag(q, 'relational');
 OPEN c FOR EXECUTE '/*+ GraphRoute(btree) */ ' || q;
 LOOP
   FETCH c INTO r;
   EXIT WHEN NOT FOUND;
   rows := rows || jsonb_build_array(to_jsonb(r));
 END LOOP;
 CLOSE c;
 SELECT coalesce(jsonb_agg(value ORDER BY value), '[]'::jsonb) INTO rows
 FROM jsonb_array_elements(rows);
 IF rows IS DISTINCT FROM expected THEN
   RAISE EXCEPTION 'cursor pause/resume lost native cached bindings';
 END IF;
 -- A separate early close exercises cleanup of an incomplete Memoize entry.
 OPEN c FOR EXECUTE '/*+ GraphRoute(btree) */ ' || q;
 FETCH c INTO r;
 CLOSE c;
END $$;

-- RLS remains PG's own qualification, and caches cannot leak between
-- roles or bypass the policy.  The existing graph_native regression also
-- covers policy-protected views and LATERAL NULL bindings.
CREATE ROLE regress_ggc_native_reuse;
GRANT USAGE ON SCHEMA ggc_native_reuse TO regress_ggc_native_reuse;
GRANT SELECT ON v, e, g TO regress_ggc_native_reuse;
ALTER TABLE v ENABLE ROW LEVEL SECURITY;
CREATE POLICY native_reuse_policy ON v USING (tenant = 1);
SET ROLE regress_ggc_native_reuse;
DO $$
DECLARE q text := $q$SELECT * FROM GRAPH_TABLE(g MATCH
   (a IS v WHERE a.id = 'v1')-[x IS e]->(b IS v)
   COLUMNS(x.edgeid AS edge, b.id AS destination)) gt$q$;
BEGIN
 PERFORM pg_temp.reuse_compare(q);
END $$;
RESET ROLE;
-- B-tree mark/restore must retain its own storage across per-tuple resets.
-- Cover both scan directions, heap and index-only scans, rescans, and abort.
CREATE TABLE mark_test(id bigint, payload text);
INSERT INTO mark_test SELECT i, repeat('payload-' || i, 20)
FROM generate_series(1, 5000) i;
CREATE INDEX mark_test_idx ON mark_test(id) INCLUDE(payload);
DO $$
DECLARE ios boolean; backwards boolean;
BEGIN
 FOREACH ios IN ARRAY ARRAY[false, true] LOOP
  FOREACH backwards IN ARRAY ARRAY[false, true] LOOP
   PERFORM test_graph_btree_mark_restore('mark_test', 'mark_test_idx', ios, backwards);
   BEGIN
    PERFORM test_graph_btree_mark_restore('mark_test', 'mark_test_idx', ios, backwards, true);
    RAISE EXCEPTION 'expected mark diagnostic abort';
   EXCEPTION WHEN internal_error THEN
    IF SQLERRM <> 'B-tree mark diagnostic requested abort' THEN RAISE; END IF;
   END;
   PERFORM test_graph_btree_mark_restore('mark_test', 'mark_test_idx', ios, backwards);
  END LOOP;
 END LOOP;
END $$;
RESET search_path;
RESET jit;
RESET max_parallel_workers_per_gather;
DROP SCHEMA ggc_native_reuse CASCADE;
DROP ROLE regress_ggc_native_reuse;
