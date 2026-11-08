\pset format unaligned
\pset tuples_only on
SET client_min_messages=warning;
CREATE ROLE ggc_reassign_old SUPERUSER;
CREATE ROLE ggc_reassign_new SUPERUSER;
CREATE TEMP TABLE ggc_reassign_state(label text PRIMARY KEY, stat_oid oid, scope jsonb);
CREATE FUNCTION pg_temp.ggc_reassign_cost_scope(fingerprint bigint) RETURNS jsonb
LANGUAGE SQL AS $$
 SELECT jsonb_build_object(
  'catalog_provider_oid',0,'graph_oid',0,'edge_element_oid',0,
  'index_relid',0,'effective_role',current_user::regrole::oid::bigint,
  'endpoint_domain',0,'scope_kind',3,'direction',0,'path_mode',0,
  'result_mode',0,'catalog_generation',0,'security_generation',0,
  'build_generation',0,'semantic_fingerprint',fingerprint)
$$;
CREATE TEMP TABLE ggc_reassign_payload AS
SELECT jsonb_build_array(jsonb_build_object(
 'metric_kind',300,'profile_key',jsonb_build_object('component','batch_startup'),
 'metadata',jsonb_build_object(
  'started_at','2026-11-08 00:00:00+00','finished_at','2026-11-08 00:00:01+00',
  'snapshot_source','offline_microbenchmark','sample_method','fixed',
  'truncation_reason','none','seed',1,'population',1,'sample_count',1,
  'effective_sample_size',1,'complete_depth',0,'layout_version',1,
  'model_version',1,'change_baseline',jsonb_build_object('source','fixture'),
  'resources',jsonb_build_object('scan_pages',0,'examined_entries',1,'peak_bytes',1)),
 'estimate',jsonb_build_object('kind',1,'state',1,'sample_count',1,
  'p95_known',false,'uncertainty_known',false,'censored',false,
  'expected',1,'workload_p95',NULL,'uncertainty_low',NULL,'uncertainty_high',NULL))) profiles;
SET ROLE ggc_reassign_old;
INSERT INTO ggc_reassign_state
SELECT 'old',pg_graph_statistic_create(s,'{}'),s
FROM (SELECT pg_temp.ggc_reassign_cost_scope(1) s) q;
INSERT INTO ggc_reassign_state
SELECT 'recreate',pg_graph_statistic_create(s,'{}'),s
FROM (SELECT pg_temp.ggc_reassign_cost_scope(2) s) q;
SELECT pg_graph_statistic_publish(stat_oid,profiles)=1
FROM ggc_reassign_state,ggc_reassign_payload WHERE label='old';
CREATE TABLE ggc_reassign_vertex(id bigint PRIMARY KEY);
CREATE TABLE ggc_reassign_edge(id bigint PRIMARY KEY,src bigint,dst bigint);
INSERT INTO ggc_reassign_vertex VALUES(1),(2);
INSERT INTO ggc_reassign_edge VALUES(1,1,2);
CREATE INDEX ggc_reassign_edge_src ON ggc_reassign_edge(src);
CREATE PROPERTY GRAPH ggc_reassign_graph
 VERTEX TABLES(ggc_reassign_vertex KEY(id))
 EDGE TABLES(ggc_reassign_edge KEY(id)
  SOURCE KEY(src) REFERENCES ggc_reassign_vertex(id)
  DESTINATION KEY(dst) REFERENCES ggc_reassign_vertex(id));
INSERT INTO ggc_reassign_state
SELECT 'logical',pg_graph_statistic_create(s,'{}'),s FROM
(SELECT jsonb_build_object(
 'catalog_provider_oid',0,'graph_oid','ggc_reassign_graph'::regclass::oid::bigint,
 'edge_element_oid',(SELECT oid::bigint FROM pg_propgraph_element
  WHERE pgepgid='ggc_reassign_graph'::regclass AND pgekind='e'),
 'index_relid',0,'effective_role',current_user::regrole::oid::bigint,
 'endpoint_domain',1,'scope_kind',1,'direction',1,'path_mode',1,
 'result_mode',1,'catalog_generation',1,'security_generation',1,
 'build_generation',0,'semantic_fingerprint',3) s) q;
RESET ROLE;
SET ROLE ggc_reassign_new;
-- The destination already owns the otherwise identical scope.
INSERT INTO ggc_reassign_state
SELECT 'existing',pg_graph_statistic_create(s,'{}'),s
FROM (SELECT pg_temp.ggc_reassign_cost_scope(1) s) q;
SELECT pg_graph_statistic_publish(stat_oid,profiles)=1
FROM ggc_reassign_state,ggc_reassign_payload WHERE label='existing';
RESET ROLE;
REASSIGN OWNED BY ggc_reassign_old TO ggc_reassign_old;
SELECT count(*)=3 AND bool_and(h.gstowner='ggc_reassign_old'::regrole
 AND h.gstrole='ggc_reassign_old'::regrole)
FROM pg_graph_statistic h JOIN ggc_reassign_state s ON h.oid=s.stat_oid
WHERE s.label IN ('old','recreate','logical');
SELECT count(*)=1 FROM pg_graph_statistic_data d JOIN ggc_reassign_state s
 ON d.gstdstatid=s.stat_oid WHERE s.label='old';
REASSIGN OWNED BY ggc_reassign_old TO ggc_reassign_new;
SELECT count(*)=0 FROM pg_graph_statistic h JOIN ggc_reassign_state s
 ON h.oid=s.stat_oid WHERE s.label IN ('old','recreate','logical');
SELECT count(*)=0 FROM pg_graph_statistic_data d JOIN ggc_reassign_state s
 ON d.gstdstatid=s.stat_oid WHERE s.label IN ('old','recreate','logical');
SELECT count(*)=0 FROM pg_shdepend d JOIN ggc_reassign_state s
 ON d.objid=s.stat_oid WHERE d.classid='pg_graph_statistic'::regclass
 AND s.label IN ('old','recreate','logical');
SELECT count(*)=0 FROM pg_depend d JOIN ggc_reassign_state s
 ON d.objid=s.stat_oid WHERE d.classid='pg_graph_statistic'::regclass
 AND s.label IN ('old','recreate','logical');
SELECT count(*)=1 AND bool_and(h.gstowner='ggc_reassign_new'::regrole
 AND h.gstrole='ggc_reassign_new'::regrole AND h.gstgeneration=1)
FROM pg_graph_statistic h JOIN ggc_reassign_state s ON h.oid=s.stat_oid
WHERE s.label='existing';
SELECT (SELECT count(*) FROM ggc_reassign_edge)=1
 AND (SELECT count(*) FROM ggc_reassign_vertex)=2
 AND (SELECT relowner='ggc_reassign_new'::regrole FROM pg_class
      WHERE oid='ggc_reassign_graph'::regclass)
 AND to_regclass('ggc_reassign_edge_src') IS NOT NULL;
SET ROLE ggc_reassign_old;
SELECT pg_graph_statistic_read(stat_oid,scope) IS NULL
FROM ggc_reassign_state WHERE label='old';
RESET ROLE;
DROP ROLE ggc_reassign_old;
SET ROLE ggc_reassign_new;
INSERT INTO ggc_reassign_state
SELECT 'fresh',pg_graph_statistic_create(s,'{}'),s
FROM (SELECT pg_temp.ggc_reassign_cost_scope(2) s) q;
SELECT pg_graph_statistic_publish(stat_oid,profiles)=1
FROM ggc_reassign_state,ggc_reassign_payload WHERE label='fresh';
SELECT (pg_graph_statistic_read(stat_oid,scope)->>'generation')::int=1
FROM ggc_reassign_state WHERE label='fresh';
RESET ROLE;
DROP OWNED BY ggc_reassign_new;
DROP ROLE ggc_reassign_new;
DROP FUNCTION pg_temp.ggc_reassign_cost_scope(bigint);
DROP TABLE ggc_reassign_state,ggc_reassign_payload;
