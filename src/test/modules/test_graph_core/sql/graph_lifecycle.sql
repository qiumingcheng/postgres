\pset format unaligned
\pset tuples_only on
SET client_min_messages=warning;
SET plan_cache_mode=force_generic_plan;

CREATE TABLE ggc_life_edge(
 id bigint PRIMARY KEY,
 src bigint NOT NULL,
 dst bigint NOT NULL,
 payload text
) WITH(fillfactor=40);
INSERT INTO ggc_life_edge VALUES
 (101,1,2,'a'),(102,1,3,'b'),(103,2,4,'c'),(104,3,4,'d'),
 (105,4,1,'e'),(106,4,5,'f'),(107,5,6,'g'),(108,6,7,'h');
CREATE TABLE ggc_life_vertex(id bigint PRIMARY KEY);
INSERT INTO ggc_life_vertex SELECT i FROM generate_series(1,7) i;
CREATE INDEX ggc_life_out ON ggc_life_edge(src);
CREATE INDEX ggc_life_adj1 ON ggc_life_edge USING graph_adj(src,dst);
CREATE INDEX ggc_life_adj2 ON ggc_life_edge USING graph_adj(src,dst);
CREATE PROPERTY GRAPH ggc_life_graph
 VERTEX TABLES(ggc_life_vertex KEY(id))
 EDGE TABLES(ggc_life_edge KEY(id)
  SOURCE KEY(src) REFERENCES ggc_life_vertex(id)
  DESTINATION KEY(dst) REFERENCES ggc_life_vertex(id));
CREATE TABLE ggc_life_request(
 anchor bigint NOT NULL,
 vertices bigint[],
 edges bigint[],
 payloads text[]
);
INSERT INTO ggc_life_request(anchor) VALUES (1),(2);

-- A cached generic plan contains only serialized provider identity/generation.
SELECT test_graph_fixed_config(
 'ggc_life_request',array_fill('ggc_life_edge'::regclass,ARRAY[2]),
 array_fill('ggc_life_out'::regclass,ARRAY[2]),ARRAY[1,1],0,4,64,100000);
SELECT test_graph_lifecycle_config('btree',ARRAY[]::regclass[],0,7,11,0);
PREPARE ggc_life_generic(bigint) AS
 SELECT anchor,vertices,edges,payloads
 FROM ggc_life_request WHERE anchor=$1;
EXECUTE ggc_life_generic(1);
EXECUTE ggc_life_generic(2);
SELECT generic_plans>=2 AND custom_plans=0
FROM pg_prepared_statements WHERE name='ggc_life_generic';
DEALLOCATE ggc_life_generic;

-- ABI rejection is handled before the volatile anchor qual can run.
TRUNCATE ggc_life_request;
INSERT INTO ggc_life_request(anchor) VALUES (1);
CREATE SEQUENCE ggc_life_side_effect;
SELECT test_graph_fixed_config(
 'ggc_life_request',array_fill('ggc_life_edge'::regclass,ARRAY[2]),
 array_fill('ggc_life_out'::regclass,ARRAY[2]),ARRAY[1,1],0,4,64,100000);
SELECT test_graph_lifecycle_config('abi_reject',ARRAY[]::regclass[],0,7,11,0);
SELECT anchor,vertices,edges,payloads
FROM ggc_life_request WHERE nextval('ggc_life_side_effect')>0;
SELECT NOT is_called FROM ggc_life_side_effect;

-- The same-semantics B-tree fallback is prevalidated and selected at startup.
SELECT test_graph_lifecycle_config(
 'abi_reject',array_fill('ggc_life_out'::regclass,ARRAY[2]),0,7,11,0);
EXPLAIN (ANALYZE,COSTS OFF,TIMING OFF,SUMMARY OFF,BUFFERS OFF)
SELECT anchor,vertices,edges,payloads
FROM ggc_life_request WHERE nextval('ggc_life_side_effect')>0;
SELECT last_value=1 AND is_called FROM ggc_life_side_effect;

-- Once execution starts, a provider ERROR is reported; fallback is not tried.
SELECT test_graph_lifecycle_config(
 'runtime_error',array_fill('ggc_life_out'::regclass,ARRAY[2]),0,7,11,0);
SELECT anchor,vertices,edges,payloads FROM ggc_life_request;
WITH d AS (SELECT test_graph_lifecycle_diagnostic()::jsonb j)
SELECT (j->>'wrapper_begin')::int=1
   AND (j->>'wrapper_getnext')::int=1
   AND (j->>'wrapper_end')::int=1
   AND (j->>'wrapper_active')::int=0
FROM d;

-- Handles are direct only for the same provider/index/build/domain scope.
SELECT test_graph_fixed_config(
 'ggc_life_request',array_fill('ggc_life_edge'::regclass,ARRAY[4]),
 ARRAY['ggc_life_adj1'::regclass,'ggc_life_adj1'::regclass,
       'ggc_life_adj2'::regclass,'ggc_life_adj2'::regclass],
 ARRAY[1,1,1,1],0,4,64,100000);
SELECT test_graph_lifecycle_config(
 'graph_adj',array_fill('ggc_life_out'::regclass,ARRAY[4]),1,7,11,0);
EXPLAIN (ANALYZE,COSTS OFF,TIMING OFF,SUMMARY OFF,BUFFERS OFF)
SELECT anchor,vertices,edges,payloads FROM ggc_life_request;

-- A parameterized inner GraphExpandFixed is rebuilt by ExecReScan.
TRUNCATE ggc_life_request;
INSERT INTO ggc_life_request(anchor) VALUES (1),(2);
CREATE TABLE ggc_life_seed(id bigint PRIMARY KEY);
INSERT INTO ggc_life_seed VALUES (1),(2);
SELECT test_graph_fixed_config(
 'ggc_life_request',array_fill('ggc_life_edge'::regclass,ARRAY[2]),
 array_fill('ggc_life_out'::regclass,ARRAY[2]),ARRAY[1,1],0,4,64,100000);
SELECT test_graph_lifecycle_config('btree',ARRAY[]::regclass[],0,7,11,0);
EXPLAIN (ANALYZE,COSTS OFF,TIMING OFF,SUMMARY OFF,BUFFERS OFF)
SELECT s.id,q.anchor,q.vertices,q.edges,q.payloads
FROM ggc_life_seed s
CROSS JOIN LATERAL
 (SELECT anchor,vertices,edges,payloads
  FROM ggc_life_request r WHERE r.anchor=s.id OFFSET 0) q
ORDER BY s.id,q.edges::text;

-- Cost statistics are an independent cached-plan dependency.  Publishing a
-- new generation replans subsequent executions, while DDL dependencies hidden
-- in custom_private invalidate the plan through normal relcache machinery.
CREATE TEMP TABLE ggc_life_stats AS
WITH ids AS
(
 SELECT 'ggc_life_graph'::regclass::oid::bigint graph_oid,
        'ggc_life_out'::regclass::oid::bigint index_oid,
        oid::bigint role_oid
 FROM pg_roles WHERE rolname=current_user
), mapped AS
(
 SELECT ids.*,p.oid::bigint edge_oid
 FROM ids JOIN pg_propgraph_element p
   ON p.pgepgid=ids.graph_oid AND p.pgekind='e'
), scope AS
(
 SELECT *,jsonb_build_object(
   'catalog_provider_oid',0,'graph_oid',graph_oid,'edge_element_oid',edge_oid,
   'index_relid',0,'effective_role',role_oid,'endpoint_domain',77,
   'scope_kind',1,'direction',1,'path_mode',1,'result_mode',1,
   'catalog_generation',7,'security_generation',11,'build_generation',0,
   'semantic_fingerprint',6060) logical_scope,
  jsonb_build_object(
   'catalog_provider_oid',0,'graph_oid',graph_oid,'edge_element_oid',edge_oid,
   'index_relid',index_oid,'effective_role',role_oid,'endpoint_domain',77,
   'scope_kind',2,'direction',1,'path_mode',1,'result_mode',1,
   'catalog_generation',7,'security_generation',11,'build_generation',1,
   'semantic_fingerprint',6060) physical_scope,
  jsonb_build_object(
   'catalog_provider_oid',0,'graph_oid',0,'edge_element_oid',0,
   'index_relid',0,'effective_role',role_oid,'endpoint_domain',0,
   'scope_kind',3,'direction',0,'path_mode',0,'result_mode',0,
   'catalog_generation',0,'security_generation',0,'build_generation',0,
   'semantic_fingerprint',1) cost_scope
 FROM mapped
)
SELECT graph_oid,edge_oid,index_oid,logical_scope,physical_scope,cost_scope,
       pg_graph_statistic_create(logical_scope,'{}'::jsonb) logical_oid,
       pg_graph_statistic_create(physical_scope,'{}'::jsonb) physical_oid,
       coalesce((SELECT oid FROM pg_graph_statistic
                 WHERE gstkind=3 AND gstowner=role_oid
                 ORDER BY oid DESC LIMIT 1),
                pg_graph_statistic_create(cost_scope,'{}'::jsonb)) cost_oid
FROM scope;

CREATE FUNCTION pg_temp.ggc_life_publish_cost(p_value float8)
RETURNS bigint LANGUAGE SQL AS $$
WITH constants AS
(
 SELECT jsonb_build_object(
  'started_at','2026-09-21 00:00:00+00','finished_at','2026-09-21 00:00:01+00',
  'snapshot_source','lifecycle_regression','sample_method','fixed_test_profile',
  'truncation_reason','none','seed',60001,'population',8,'sample_count',100,
  'effective_sample_size',100,'complete_depth',0,'layout_version',1,
  'model_version',1,'change_baseline',jsonb_build_object('source','G1-060'),
  'resources',jsonb_build_object('scan_pages',0,'examined_entries',800,
                                 'peak_bytes',65536)) metadata
), coefficients(metric_kind,component) AS
(
 VALUES (300,'batch_startup'),(301,'key_probe'),(302,'ref_deref'),
        (303,'adj_item'),(304,'heap_fetch'),(305,'heap_page_group'),
        (306,'binding_extend')
), payload AS
(
 SELECT jsonb_agg(jsonb_build_object(
  'metric_kind',metric_kind,
  'profile_key',jsonb_build_object('cost_profile_version',1,'component',component),
  'metadata',metadata,
  'estimate',jsonb_build_object('kind',1,'state',1,'sample_count',100,
    'p95_known',false,'uncertainty_known',false,'censored',false,
    'expected',p_value,'workload_p95',NULL,'uncertainty_low',NULL,
    'uncertainty_high',NULL))) profiles
 FROM coefficients CROSS JOIN constants
)
SELECT pg_graph_statistic_publish(cost_oid,profiles)
FROM ggc_life_stats,payload
$$;

CREATE FUNCTION pg_temp.ggc_life_explain_prepared()
RETURNS jsonb LANGUAGE plpgsql AS $$
DECLARE result jsonb;
BEGIN
 EXECUTE 'EXPLAIN (FORMAT JSON,COSTS OFF) EXECUTE ggc_life_cbo(1)' INTO result;
 RETURN result;
END
$$;

SELECT pg_temp.ggc_life_publish_cost(0.000001)>0;
SELECT test_graph_cbo_config(
 'ggc_life_request','ggc_life_edge','ggc_life_out',graph_oid::oid,edge_oid::oid,
 77,7,11,1,6060,2,1)
FROM ggc_life_stats;
PREPARE ggc_life_cbo(bigint) AS
 SELECT r.anchor,e1.id,e1.dst,e2.id,e2.dst
 FROM ggc_life_request r
 JOIN ggc_life_edge e1 ON e1.src=r.anchor
 JOIN ggc_life_edge e2 ON e2.src=e1.dst
 WHERE r.anchor >= $1;
CREATE TEMP TABLE ggc_life_plan_state AS
SELECT (SELECT gstgeneration FROM pg_graph_statistic s
        JOIN ggc_life_stats x ON s.oid=x.cost_oid) generation,
       (test_graph_cbo_diagnostic()::jsonb->>'plan_builds')::int builds,
       pg_temp.ggc_life_explain_prepared() document;
UPDATE ggc_life_plan_state
SET builds=(test_graph_cbo_diagnostic()::jsonb->>'plan_builds')::int;
WITH p AS
(
 SELECT jsonb_path_query_first(document,
  '$.** ? (@."Custom Plan Provider" == "GraphExpandFixed")') node,
  generation,builds FROM ggc_life_plan_state
)
SELECT node IS NOT NULL
   AND node->>'Graph Cost Profile'=
       'calibrated v1 generation '||generation::text
   AND builds>0
FROM p;

WITH published AS
(
 SELECT pg_temp.ggc_life_publish_cost(0.000001) generation
), planned AS
(
 SELECT generation,pg_temp.ggc_life_explain_prepared() document
 FROM published
), graph_node AS
(
 SELECT generation,jsonb_path_query_first(document,
  '$.** ? (@."Custom Plan Provider" == "GraphExpandFixed")') node
 FROM planned
)
SELECT node IS NOT NULL
   AND node->>'Graph Cost Profile'=
       'calibrated v1 generation '||generation::text
   AND (test_graph_cbo_diagnostic()::jsonb->>'plan_builds')::int>
       (SELECT builds FROM ggc_life_plan_state)
FROM graph_node;
UPDATE ggc_life_plan_state
SET builds=(test_graph_cbo_diagnostic()::jsonb->>'plan_builds')::int;

ALTER TABLE ggc_life_request ADD COLUMN ddl_marker integer;
WITH planned AS
(
 SELECT pg_temp.ggc_life_explain_prepared() document
), graph_node AS
(
 SELECT jsonb_path_query_first(document,
  '$.** ? (@."Custom Plan Provider" == "GraphExpandFixed")') node
 FROM planned
)
SELECT node IS NOT NULL
   AND (test_graph_cbo_diagnostic()::jsonb->>'plan_builds')::int>
       (SELECT builds FROM ggc_life_plan_state)
FROM graph_node;
DEALLOCATE ggc_life_cbo;

SELECT pg_graph_statistic_drop(logical_oid) IS NOT NULL,
       pg_graph_statistic_drop(physical_oid) IS NOT NULL
FROM ggc_life_stats;
DROP PROPERTY GRAPH ggc_life_graph;
DROP TABLE ggc_life_seed;
DROP SEQUENCE ggc_life_side_effect;
DROP TABLE ggc_life_request;
DROP TABLE ggc_life_edge;
DROP TABLE ggc_life_vertex;
