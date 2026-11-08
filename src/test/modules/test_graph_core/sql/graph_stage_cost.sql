\set ECHO none
SET client_min_messages=warning;
LOAD 'test_graph_core';
SET jit=off;
SET graph_btree_max_candidates=1;
-- Keep calibrated profiles from other fixtures out of this ordinary-PG cost test.
CREATE ROLE ggc_stage_cost_role;
CREATE SCHEMA ggc_stage_cost AUTHORIZATION ggc_stage_cost_role;
SET ROLE ggc_stage_cost_role;
SET search_path=ggc_stage_cost,public;
CREATE TABLE v(id bigint PRIMARY KEY,color text,needle text);
CREATE TABLE e(id bigint PRIMARY KEY,src bigint NOT NULL,dst bigint NOT NULL,payload text);
INSERT INTO v SELECT i,CASE WHEN i<=100 THEN 'red' ELSE 'blue' END,'red'
 FROM generate_series(1,1000) i;
-- Every vertex has one edge to its partner. All walks stay in the same color.
INSERT INTO e SELECT i,i,CASE WHEN i%2=1 THEN i+1 ELSE i-1 END,
 CASE WHEN i<=100 THEN 'red' ELSE 'blue' END FROM generate_series(1,1000) i;
CREATE INDEX e_out ON e(src) INCLUDE(dst,id);
CREATE PROPERTY GRAPH g VERTEX TABLES(v KEY(id)) EDGE TABLES(e KEY(id)
 SOURCE KEY(src) REFERENCES v(id) DESTINATION KEY(dst) REFERENCES v(id));
VACUUM(FREEZE,ANALYZE) v;
VACUUM(FREEZE,ANALYZE) e;

CREATE FUNCTION pg_temp.stage_bag(q text) RETURNS jsonb LANGUAGE plpgsql AS $$
DECLARE r record; bag jsonb='[]';
BEGIN
 FOR r IN EXECUTE q LOOP bag:=bag||jsonb_build_array(to_jsonb(r)); END LOOP;
 RETURN (SELECT coalesce(jsonb_agg(value ORDER BY value),'[]'::jsonb)
         FROM jsonb_array_elements(bag));
END $$;
CREATE FUNCTION pg_temp.stage_plan(q text) RETURNS jsonb LANGUAGE plpgsql AS $$
DECLARE p jsonb; g jsonb;
BEGIN
 EXECUTE 'EXPLAIN(ANALYZE,TIMING OFF,FORMAT JSON) /*+ GraphRoute(btree) */ '||q INTO p;
 WITH RECURSIVE plans(n) AS (SELECT p->0->'Plan' UNION ALL
  SELECT c FROM plans CROSS JOIN LATERAL jsonb_array_elements(n->'Plans') c)
 SELECT n INTO STRICT g FROM plans WHERE n->>'Custom Plan Provider'='GraphExpandFixed';
 IF pg_temp.stage_bag('/*+ GraphRoute(btree) */ '||q) IS DISTINCT FROM
    pg_temp.stage_bag('/*+ GraphRoute(relational) */ '||q) THEN
  RAISE EXCEPTION 'stage filter changed complete result bag: %',q;
 END IF;
 RETURN g;
END $$;

-- The filtered anchor has already passed its local predicate. Revisiting that
-- exact variable must keep its 100 bindings, not multiply 0.1 on each revisit.
DO $$
DECLARE q text; p jsonb;
BEGIN
 FOREACH q IN ARRAY ARRAY[
  $q$SELECT * FROM GRAPH_TABLE(g MATCH (a IS v WHERE a.color='red')
   -[x IS e]->(b IS v)-[y IS e]->(a)
   COLUMNS(a.id AS aid,b.id AS bid,x.id AS x,y.id AS y)) gt$q$,
  $q$SELECT * FROM GRAPH_TABLE(g MATCH (a IS v WHERE a.color='red')
   -[w IS e]->(b IS v)-[x IS e]->(a)-[y IS e]->(c IS v)-[z IS e]->(a)
   COLUMNS(a.id AS aid,b.id AS bid,c.id AS cid,w.id AS w,x.id AS x,y.id AS y,z.id AS z)) gt$q$
 ] LOOP
  p:=pg_temp.stage_plan(q);
  IF (p->>'Plan Rows')::numeric::int<>100 OR (p->>'Actual Rows')::numeric::int<>100 THEN
   RAISE EXCEPTION 'repeated bound vertex restriction changed estimate: %',p;
  END IF;
 END LOOP;
END $$;

-- Equivalent predicates at the first or last stage produce the same bag.
-- Earlier rejection must reduce later access work, not only final Path.rows.
-- The last pair requires properties from two variables: its predicate must
-- run and be charged as soon as both inputs exist, including property lookup.
-- PG can use its default cross-column selectivity for that pair; do not fit
-- the cost model to this fixture's correlated data or require an exact count.
DO $$
DECLARE predicates text[]; early_q text; late_q text; early jsonb; late jsonb;
        early_rows numeric; late_rows numeric;
        pattern text=$q$SELECT * FROM GRAPH_TABLE(g MATCH (a IS v)
         -[x IS e%s]->(b IS v%s)-[y IS e]->(c IS v)-[z IS e%s]->(d IS v%s)
         %s COLUMNS(a.id AS aid,x.id AS x,y.id AS y,z.id AS z,d.id AS did)) gt$q$;
BEGIN
 FOREACH predicates SLICE 1 IN ARRAY ARRAY[
  ARRAY[' WHERE x.payload=''red''','','','','', '', '', ' WHERE z.payload=''red''','',''],
  ARRAY['',' WHERE b.color=''red''','','','', '', '', '', ' WHERE d.color=''red''',''],
  ARRAY['','','','','WHERE x.payload=a.needle', '', '', '', '', 'WHERE z.payload=a.needle']
 ] LOOP
  early_q:=format(pattern,predicates[1],predicates[2],predicates[3],predicates[4],predicates[5]);
  late_q:=format(pattern,predicates[6],predicates[7],predicates[8],predicates[9],predicates[10]);
  early:=pg_temp.stage_plan(early_q);
  late:=pg_temp.stage_plan(late_q);
  early_rows:=(early->>'Plan Rows')::numeric;
  late_rows:=(late->>'Plan Rows')::numeric;
  IF early_rows<=0 OR early_rows IS DISTINCT FROM late_rows OR
     (predicates[5]='' AND early_rows<>100) OR
     (early->>'Actual Rows')::numeric::int<>100 OR (late->>'Actual Rows')::numeric::int<>100 OR
     pg_temp.stage_bag('/*+ GraphRoute(btree) */ '||early_q) IS DISTINCT FROM
     pg_temp.stage_bag('/*+ GraphRoute(btree) */ '||late_q) OR
     (early->>'Total Cost')::numeric >= (late->>'Total Cost')::numeric THEN
   RAISE EXCEPTION 'early filter did not reduce subsequent work: %, %',early,late;
  END IF;
 END LOOP;
END $$;

-- With one anchor and fanout one, the first batch comprises the entire walk.
-- Its startup includes each prefix expansion/vertex read, not only key probes.
DO $$
DECLARE p jsonb;
BEGIN
 p:=pg_temp.stage_plan($q$SELECT * FROM GRAPH_TABLE(g MATCH
  (a IS v WHERE a.id=1::bigint)-[x IS e]->(b IS v)-[y IS e]->(c IS v)
  -[z IS e]->(d IS v) COLUMNS(x.id AS x,y.id AS y,z.id AS z,d.id AS did)) gt$q$);
 IF (p->>'Actual Rows')::numeric::int<>1 OR
    abs((p->>'Startup Cost')::numeric-(p->>'Total Cost')::numeric)>0.01 THEN
  RAISE EXCEPTION 'first batch startup omitted prefix work: %',p;
 END IF;
END $$;

SET search_path=public;
DROP FUNCTION pg_temp.stage_plan(text);
DROP FUNCTION pg_temp.stage_bag(text);
RESET ROLE;

-- Real conditional samples distinguish path multiplicity from shared endpoint
-- work: A --100 parallel edges--> B --one edge--> C. Keeping ten histories
-- retains B and its edge; it does not make either one tenth of an endpoint.
SET search_path=ggc_stage_cost,public;
CREATE TABLE sample_v(id bigint PRIMARY KEY);
CREATE TABLE sample_e(id bigint PRIMARY KEY,src bigint NOT NULL,dst bigint NOT NULL,weight bigint);
INSERT INTO sample_v VALUES(1),(2),(3);
INSERT INTO sample_e SELECT i,1,2,i FROM generate_series(1,100) i;
INSERT INTO sample_e VALUES(101,2,3,1);
CREATE INDEX sample_out ON sample_e(src);
CREATE PROPERTY GRAPH sample_g VERTEX TABLES(sample_v KEY(id)) EDGE TABLES(sample_e KEY(id)
 SOURCE KEY(src) REFERENCES sample_v(id) DESTINATION KEY(dst) REFERENCES sample_v(id));
VACUUM(FREEZE,ANALYZE) sample_v;
VACUUM(FREEZE,ANALYZE) sample_e;
CREATE FUNCTION pg_temp.stage_sample_cost(oid,oid,oid,integer,integer,
 bigint,bigint,bigint,bigint,integer,integer,float8,float8[],integer,bigint,
 boolean,bigint,float8[],integer[]) RETURNS text
AS '$libdir/test_graph_core','test_graph_path_cost' LANGUAGE C STRICT;
DO $$
DECLARE l jsonb; p jsonb; lo oid; po oid; edge_oid oid; collected jsonb;
        raw jsonb; filtered jsonb; exact_sample jsonb; extra_filter jsonb;
        metric text; selects float8[];
        options jsonb='{"seed":1,"seed_target":1,"hops":2,"predicate_attnum":0,
         "predicate_min":0,"predicate_max":0,"predicate_clause_id":0,
         "max_scan_pages":1000,"max_examined_entries":10000,"max_bindings":1000,
         "max_bindings_per_seed":1000,"max_entries_per_seed":1000,
         "max_memory_bytes":16777216,"max_payload_bytes":1048576,"max_profiles":64,
         "max_parent_batch":64,"deadline_ms":10000,"lock_timeout_ms":1000}';
BEGIN
 SELECT oid INTO STRICT edge_oid FROM pg_propgraph_element
  WHERE pgepgid='sample_g'::regclass AND pgekind='e';
 l:=jsonb_build_object('catalog_provider_oid',0,'graph_oid','sample_g'::regclass::oid::bigint,
  'edge_element_oid',edge_oid::bigint,'index_relid',0,
  'effective_role',(SELECT oid::bigint FROM pg_roles WHERE rolname=current_user),
  'endpoint_domain',77,'scope_kind',1,'direction',1,'path_mode',1,'result_mode',1,
  'catalog_generation',7,'security_generation',11,'build_generation',0,'semantic_fingerprint',90117);
 p:=l||jsonb_build_object('scope_kind',2,'index_relid','sample_out'::regclass::oid::bigint,'build_generation',1);
 lo:=pg_graph_statistic_create(l,'{"anchor_condition":{"version":1,"key":1}}');
 po:=pg_graph_statistic_create(p,'{"anchor_condition":{"version":1,"key":1}}');
 collected:=pg_graph_statistic_collect_path(lo,po,options);
 IF collected->>'status'<>'READY' THEN RAISE EXCEPTION 'parallel sample failed: %',collected; END IF;
 raw:=pg_temp.stage_sample_cost('sample_g'::regclass,edge_oid,'sample_out'::regclass,
  77,1,7,11,1,90117,2,0,1.0,ARRAY[0,0]::float8[],64,1048576,false,1,
  ARRAY[1,1,1,1,1,1]::float8[],ARRAY[0,0])::jsonb;
 IF (raw->>'logical_state')::int<>1 OR (raw->>'physical_state')::int<>1 OR
    (raw->'stages'->1->>'B')::numeric<>100 OR
    (raw->'stages'->1->>'F')::numeric<>1 OR
    (raw->'stages'->1->>'N')::numeric<>2 OR
    (raw->'stages'->1->>'E')::numeric<>1 THEN
  RAISE EXCEPTION 'parallel sample lost metric populations: %',raw;
 END IF;
 -- Both an edge predicate and a history predicate can retain ten parallel
 -- paths. Unknown packing conservatively keeps two sampled requests (the
 -- actual filtered execution needs one); it must not invent 0.2 requests.
 FOREACH selects SLICE 1 IN ARRAY ARRAY[
  ARRAY[0.1,1,1,1,1,1]::float8[],ARRAY[1,1,0.1,1,1,1]::float8[]] LOOP
  filtered:=pg_temp.stage_sample_cost('sample_g'::regclass,edge_oid,'sample_out'::regclass,
   77,1,7,11,1,90117,2,0,1.0,ARRAY[0,0]::float8[],64,1048576,false,1,
   selects,ARRAY[0,0])::jsonb;
  IF (filtered->'stages'->1->>'B')::numeric<>10 OR
     (filtered->'stages'->1->>'P')::numeric<>10 THEN
   RAISE EXCEPTION 'parallel path predicate did not reduce bindings: %',filtered;
  END IF;
  FOREACH metric IN ARRAY ARRAY['F','N','E','A','X','H','page_groups','peak_restore_bytes'] LOOP
   IF filtered->'stages'->1->metric IS DISTINCT FROM raw->'stages'->1->metric THEN
    RAISE EXCEPTION 'path survival incorrectly discounted %: %, %',metric,raw,filtered;
   END IF;
  END LOOP;
 END LOOP;
 filtered:=pg_temp.stage_sample_cost('sample_g'::regclass,edge_oid,'sample_out'::regclass,
  77,1,7,11,1,90117,2,0,1.0,ARRAY[0,0]::float8[],64,1048576,false,1,
  ARRAY[1,1,0,1,1,1]::float8[],ARRAY[0,0])::jsonb;
 FOREACH metric IN ARRAY ARRAY['F','B','N','E','A','P','X','H','page_groups'] LOOP
  IF (filtered->'stages'->1->>metric)::numeric<>0 THEN
   RAISE EXCEPTION 'empty surviving frontier retained %: %',metric,filtered;
  END IF;
 END LOOP;
 -- The collector itself now applies weight BETWEEN 1 AND 10. Marking that
 -- exact template must not apply its selectivity again; an extra template
 -- still reduces the sampled population.
 collected:=pg_graph_statistic_collect_path(lo,po,options||
  '{"predicate_attnum":4,"predicate_min":1,"predicate_max":10,"predicate_clause_id":1}');
 IF collected->>'status'<>'READY' THEN RAISE EXCEPTION 'filtered sample failed: %',collected; END IF;
 exact_sample:=pg_temp.stage_sample_cost('sample_g'::regclass,edge_oid,'sample_out'::regclass,
  77,1,7,11,1,90117,2,1,1.0,ARRAY[0,0]::float8[],64,1048576,false,1,
  ARRAY[0.1,1,1,1,1,1]::float8[],ARRAY[1,1])::jsonb;
 extra_filter:=pg_temp.stage_sample_cost('sample_g'::regclass,edge_oid,'sample_out'::regclass,
  77,1,7,11,1,90117,2,1,1.0,ARRAY[0,0]::float8[],64,1048576,false,1,
  ARRAY[0.1,1,1,1,1,1]::float8[],ARRAY[0,0])::jsonb;
 IF (exact_sample->>'logical_state')::int<>1 OR
    (exact_sample->'stages'->1->>'P')::numeric<>10 OR
    (exact_sample->'stages'->1->>'N')::numeric<>1 OR
    (extra_filter->'stages'->1->>'P')::numeric<>1 THEN
  RAISE EXCEPTION 'sampled predicate applied twice or extra predicate lost: %, %',exact_sample,extra_filter;
 END IF;
END $$;
DROP FUNCTION pg_temp.stage_sample_cost(oid,oid,oid,integer,integer,
 bigint,bigint,bigint,bigint,integer,integer,float8,float8[],integer,bigint,
 boolean,bigint,float8[],integer[]);
SET search_path=public;
DROP SCHEMA ggc_stage_cost CASCADE;
DROP ROLE ggc_stage_cost_role;
RESET graph_btree_max_candidates;
\echo graph stage filtering, cycle estimates, and startup: ok
