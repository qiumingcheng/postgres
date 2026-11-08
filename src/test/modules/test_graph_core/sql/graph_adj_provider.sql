\pset format unaligned
\pset tuples_only on
SET client_min_messages=warning;

CREATE TABLE ggc_adj_vertex(id bigint PRIMARY KEY);
CREATE TABLE ggc_adj_edge(
 id bigint PRIMARY KEY, src bigint NOT NULL, dst bigint NOT NULL,
 payload bigint) WITH (fillfactor=40);
INSERT INTO ggc_adj_vertex SELECT i FROM generate_series(1,9) i;
INSERT INTO ggc_adj_edge VALUES
 (101,1,2,1),(102,1,3,1),(103,2,4,1),(104,3,4,1),
 (105,4,5,1),(106,4,6,1),(107,5,7,1),(108,6,7,1);
CREATE INDEX ggc_adj_edge_idx ON ggc_adj_edge USING graph_adj(src,dst);
CREATE PROPERTY GRAPH ggc_adj_graph
 VERTEX TABLES(ggc_adj_vertex KEY(id))
 EDGE TABLES(ggc_adj_edge KEY(id)
  SOURCE KEY(src) REFERENCES ggc_adj_vertex(id)
  DESTINATION KEY(dst) REFERENCES ggc_adj_vertex(id));

WITH d AS
(
 SELECT test_graph_adj_walk('ggc_adj_edge','ggc_adj_edge_idx',
                            ARRAY[1::bigint,1],4,1,0)::jsonb j
)
SELECT j->>'provider'='graph_adj'
   AND (j->>'final_binding_count')::integer=8
   AND (j->>'directory_lookups')::integer=1
   AND (j->>'ref_direct_lookups')::integer=5
   AND (j->>'ref_fallbacks')::integer=0
   AND (j->>'shared_parent_requests')::integer=12
   AND (j->>'physical_candidates')::integer=
       (j->>'provider_physical_candidates')::integer
FROM d;

WITH direct AS
(
 SELECT test_graph_adj_walk('ggc_adj_edge','ggc_adj_edge_idx',
                            ARRAY[1::bigint,1],4,1,0)::jsonb j
), foreign_ref AS
(
 SELECT test_graph_adj_walk('ggc_adj_edge','ggc_adj_edge_idx',
                            ARRAY[1::bigint,1],4,1,1)::jsonb j
)
SELECT direct.j->'final_keys'=foreign_ref.j->'final_keys'
   AND (foreign_ref.j->>'ref_fallbacks')::integer=5
   AND (foreign_ref.j->>'directory_lookups')::integer=6
   AND (foreign_ref.j->>'ref_direct_lookups')::integer=0
FROM direct,foreign_ref;

-- Same-scope persisted refs are authoritative: no key retry is permitted.
SELECT test_graph_adj_walk('ggc_adj_edge','ggc_adj_edge_idx',
                           ARRAY[1::bigint],2,1,2);

UPDATE ggc_adj_edge SET payload=payload+10 WHERE id=107;
DELETE FROM ggc_adj_edge WHERE id=108;
INSERT INTO ggc_adj_edge VALUES (109,5,8,1),(110,8,9,1);
WITH first_run AS
(
 SELECT test_graph_adj_walk('ggc_adj_edge','ggc_adj_edge_idx',
                            ARRAY[1::bigint,1],4,1,0)::jsonb j
), second_run AS
(
 SELECT test_graph_adj_walk('ggc_adj_edge','ggc_adj_edge_idx',
                            ARRAY[1::bigint,1],4,1,0)::jsonb j
), oracle AS
(
 WITH RECURSIVE walk(instance,depth,key) AS
 (
  SELECT instance,0,1::bigint FROM generate_series(1,2) instance
  UNION ALL
  SELECT w.instance,w.depth+1,e.dst
  FROM walk w JOIN ggc_adj_edge e ON e.src=w.key WHERE w.depth<4
 )
 SELECT to_jsonb(array_agg(key ORDER BY key)) keys
 FROM walk WHERE depth=4
), provider_bag AS
(
 SELECT to_jsonb(array_agg(value::bigint ORDER BY value::bigint)) keys
 FROM first_run,jsonb_array_elements_text(first_run.j->'final_keys') value
)
SELECT first_run.j->'final_keys'=second_run.j->'final_keys'
   AND provider_bag.keys=oracle.keys
   AND (first_run.j->>'visibility_rejects')::integer=1
   AND (first_run.j->>'hot_redirects')::integer=1
   AND (first_run.j->>'final_binding_count')::integer=8
FROM first_run,second_run,oracle,provider_bag;

-- These low-degree directions are entirely inline; no external merge work.
SELECT NOT (graph_adj_debug_compact('ggc_adj_edge_idx',1,true,0)::jsonb
        ->>'compacted')::boolean;
SELECT NOT (graph_adj_debug_compact('ggc_adj_edge_idx',4,true,0)::jsonb
        ->>'compacted')::boolean;
WITH d AS
(
 SELECT graph_adj_index_info('ggc_adj_edge_idx')::jsonb j
), walk AS
(
 SELECT test_graph_adj_walk('ggc_adj_edge','ggc_adj_edge_idx',
                            ARRAY[1::bigint,1],4,1,0)::jsonb j
)
SELECT (d.j->>'base_entries')::integer=0
   AND (d.j->>'inline_entries')::integer=20
   AND (d.j->>'data_pages')::integer=0
   AND (d.j->>'packed_small_segments')::boolean
   AND (walk.j->>'physical_candidates')::integer=9
   AND (walk.j->>'heap_page_groups')::integer>0
   AND (walk.j->>'tableam_fetches')::integer=9
FROM d,walk;

CREATE TEMP TABLE ggc_adj_stats_state AS
WITH ids AS
(
 SELECT 'ggc_adj_graph'::regclass::oid::bigint graph_oid,
        'ggc_adj_edge'::regclass::oid::bigint edge_relid,
        'ggc_adj_edge_idx'::regclass::oid::bigint index_oid,
        (SELECT oid::bigint FROM pg_roles WHERE rolname=current_user) role_oid
), mapped AS
(
 SELECT ids.*,p.oid::bigint edge_oid
 FROM ids JOIN pg_propgraph_element p
   ON p.pgepgid=ids.graph_oid AND p.pgekind='e'
), scopes AS
(
 SELECT jsonb_build_object(
   'catalog_provider_oid',0,'graph_oid',graph_oid,'edge_element_oid',edge_oid,
   'index_relid',0,'effective_role',role_oid,'endpoint_domain',edge_relid,
   'scope_kind',1,'direction',1,'path_mode',1,'result_mode',1,
   'catalog_generation',7,'security_generation',11,'build_generation',0,
   'semantic_fingerprint',98766) logical_scope,
  jsonb_build_object(
   'catalog_provider_oid',0,'graph_oid',graph_oid,'edge_element_oid',edge_oid,
   'index_relid',index_oid,'effective_role',role_oid,'endpoint_domain',edge_relid,
   'scope_kind',2,'direction',1,'path_mode',1,'result_mode',1,
   'catalog_generation',7,'security_generation',11,'build_generation',1,
   'semantic_fingerprint',98766) physical_scope
 FROM mapped
), configured AS
(
 SELECT *,jsonb_build_object(
   'seed',20260921,'seed_target',9,'hops',4,
   'predicate_attnum',4,'predicate_min',-100,'predicate_max',100,
   'predicate_clause_id',1,'max_scan_pages',1000,
   'max_examined_entries',100000,'max_bindings',10000,
   'max_bindings_per_seed',5000,'max_entries_per_seed',5000,
   'max_memory_bytes',67108864,'max_payload_bytes',1048576,
   'max_profiles',64,'max_parent_batch',8,'deadline_ms',10000,
   'lock_timeout_ms',1000) options
 FROM scopes
)
SELECT logical_scope,physical_scope,options,
       pg_graph_statistic_create(logical_scope,'{}'::jsonb) logical_oid,
       pg_graph_statistic_create(physical_scope,'{}'::jsonb) physical_oid
FROM configured;
WITH collected AS
(
 SELECT pg_graph_statistic_collect_path(
          logical_oid,physical_oid,options) j
 FROM ggc_adj_stats_state
)
SELECT j->>'status'='READY'
   AND (j->>'published')::boolean
   AND (j->>'complete_depth')::integer=4
   AND (j->'stages'->0->>'directory_lookups')::integer>0
   AND (j->'stages'->0->>'ref_direct_lookups')::integer=0
   AND (j->'stages'->1->>'directory_lookups')::integer=0
   AND (j->'stages'->1->>'ref_direct_lookups')::integer>0
   AND (j->'stages'->2->>'ref_direct_lookups')::integer>0
   AND (j->'stages'->3->>'ref_direct_lookups')::integer>0
FROM collected;
WITH document AS
(
 SELECT pg_graph_statistic_read(physical_oid,physical_scope) d
 FROM ggc_adj_stats_state
), profiles AS
(
 SELECT p FROM document,jsonb_array_elements(d->'profiles') p
)
SELECT count(*)=24
   AND count(*) FILTER (WHERE (p->>'metric_kind')::integer=220)=4
   AND bool_and((p->'estimate'->>'expected')::numeric>=0)
FROM profiles;

-- IN statistics must select the destination key's opfamily, not column 2.
CREATE TEMP TABLE ggc_adj_in_stats AS
WITH scopes AS
(
 SELECT logical_scope||'{"direction":2}'::jsonb l,
        physical_scope||jsonb_build_object('direction',2,
          'index_relid','ggc_adj_edge_idx'::regclass::oid::bigint) p,options
 FROM ggc_adj_stats_state
)
SELECT pg_graph_statistic_create(l,'{}'::jsonb) logical_oid,
       pg_graph_statistic_create(p,'{}'::jsonb) physical_oid,options
FROM scopes;
WITH collected AS
(
 SELECT pg_graph_statistic_collect_path(logical_oid,physical_oid,options) j
 FROM ggc_adj_in_stats
)
SELECT j->>'status'='READY' AND (j->>'published')::boolean
   AND (j->>'complete_depth')::int=4
FROM collected;

DROP PROPERTY GRAPH ggc_adj_graph;
DROP TABLE ggc_adj_edge;
DROP TABLE ggc_adj_vertex;
