\set ECHO none
\set ON_ERROR_STOP 1
SET client_min_messages=warning;
SET stats_fetch_consistency=none;
CREATE ROLE ggc_stats_owner;
CREATE ROLE ggc_stats_maintainer;
CREATE ROLE ggc_stats_reader;
CREATE SCHEMA ggc_stats_access AUTHORIZATION ggc_stats_owner;
GRANT USAGE ON SCHEMA ggc_stats_access TO ggc_stats_maintainer,ggc_stats_reader;
SET search_path=ggc_stats_access,public;

CREATE FUNCTION pg_temp.stats_assert(ok boolean, message text) RETURNS void
LANGUAGE plpgsql AS $$BEGIN
 IF ok IS DISTINCT FROM true THEN RAISE EXCEPTION 'graph stats: %',message; END IF;
END$$;
CREATE FUNCTION pg_temp.stats_denied(command text, expected text DEFAULT '42501')
RETURNS void LANGUAGE plpgsql AS $$BEGIN
 BEGIN
  EXECUTE command;
 EXCEPTION WHEN OTHERS THEN
  IF SQLSTATE=expected THEN RETURN; END IF;
  RAISE;
 END;
 RAISE EXCEPTION 'command unexpectedly succeeded: %',command;
END$$;
CREATE FUNCTION pg_temp.stats_counter_matches(bytea,jsonb,integer) RETURNS boolean
AS '$libdir/test_graph_core','test_graph_counter_matches' LANGUAGE C STRICT;

SET ROLE ggc_stats_owner;
CREATE TABLE v(id bigint PRIMARY KEY);
CREATE TABLE e(id bigint PRIMARY KEY,src bigint NOT NULL,dst bigint NOT NULL,weight bigint);
INSERT INTO v SELECT generate_series(1,4);
INSERT INTO e VALUES(1,1,2,1),(2,1,2,1),(3,2,3,1),(4,3,4,1);
CREATE INDEX e_out ON e(src);
CREATE PROPERTY GRAPH g VERTEX TABLES(v KEY(id))
 EDGE TABLES(e KEY(id) SOURCE KEY(src) REFERENCES v(id)
                     DESTINATION KEY(dst) REFERENCES v(id));
ANALYZE v;
ANALYZE e;
CREATE TABLE config AS
SELECT 'g'::regclass::oid graph_oid,p.oid edge_oid,'e_out'::regclass::oid index_oid,
 jsonb_build_object('seed',7,'seed_target',4,'hops',2,
  'predicate_attnum',4,'predicate_min',0,'predicate_max',10,'predicate_clause_id',1,
  'max_scan_pages',100,'max_examined_entries',10000,'max_bindings',1000,
  'max_bindings_per_seed',1000,'max_entries_per_seed',1000,
  'max_memory_bytes',67108864,'max_payload_bytes',1048576,'max_profiles',64,
  'max_parent_batch',4,'deadline_ms',10000,'lock_timeout_ms',1000) options,
 jsonb_build_object('mode','full','seed',7,'vertex_sample_target',4,'strata',1,
  'heavy_hitter_limit',1,'max_scan_pages',100,'max_examined_entries',10000,
  'max_bindings',1000,'max_entries_per_seed',1000,'max_memory_bytes',67108864,
  'max_payload_bytes',1048576,'max_profiles',64,'deadline_ms',10000,
  'lock_timeout_ms',1000,'throttle_delay_ms',0) single_options
FROM pg_propgraph_element p WHERE p.pgepgid='g'::regclass AND p.pgekind='e';
CREATE TABLE scopes(role_name name PRIMARY KEY,l jsonb,p jsonb,lo oid,po oid);
GRANT SELECT ON g,v,e,config,scopes TO ggc_stats_maintainer,ggc_stats_reader;
GRANT MAINTAIN ON v,e TO ggc_stats_maintainer;
GRANT INSERT,UPDATE,DELETE ON scopes TO ggc_stats_maintainer;
RESET ROLE;

CREATE FUNCTION pg_temp.stats_scope(physical boolean DEFAULT false) RETURNS jsonb
LANGUAGE sql AS $$
 SELECT jsonb_build_object('catalog_provider_oid',0,'graph_oid',graph_oid::bigint,
  'edge_element_oid',edge_oid::bigint,'index_relid',CASE WHEN physical THEN index_oid::bigint ELSE 0 END,
  'effective_role',(SELECT oid::bigint FROM pg_roles WHERE rolname=current_user),
  'endpoint_domain',77,'scope_kind',CASE WHEN physical THEN 2 ELSE 1 END,
  'direction',1,'path_mode',1,'result_mode',1,'catalog_generation',7,
  'security_generation',11,'build_generation',CASE WHEN physical THEN 1 ELSE 0 END,
  'semantic_fingerprint',66001) FROM config
$$;
CREATE FUNCTION pg_temp.stats_estimate() RETURNS jsonb LANGUAGE sql AS $$
 SELECT public.test_graph_path_cost(graph_oid,edge_oid,index_oid,77,1,7,11,1,66001,
  2,1,1.0,ARRAY[0,0]::float8[],64,1048576)::jsonb FROM config
$$;
CREATE FUNCTION pg_temp.stats_collect() RETURNS jsonb LANGUAGE sql AS $$
 SELECT pg_graph_statistic_collect_path(lo,po,options)
 FROM scopes,config WHERE role_name=current_user
$$;
CREATE FUNCTION pg_temp.stats_ready() RETURNS boolean LANGUAGE sql AS $$
 SELECT j->>'logical_state'='1' AND j->>'physical_state'='1'
  AND NOT (j->'stages'->0->>'logical_fallback')::boolean
  AND NOT (j->'stages'->0->>'physical_fallback')::boolean
 FROM (SELECT pg_temp.stats_estimate() j) s
$$;

-- Ordinary table/graph owner: both collectors and physical index scope work.
SET ROLE ggc_stats_owner;
INSERT INTO scopes SELECT current_user,l,p,
 pg_graph_statistic_create(l,'{}'),pg_graph_statistic_create(p,'{}')
FROM (SELECT pg_temp.stats_scope() l,pg_temp.stats_scope(true) p) s;
SELECT pg_temp.stats_assert(
 (pg_graph_statistic_collect(lo,single_options)->>'status')='READY','owner single-hop')
FROM scopes,config WHERE role_name=current_user;
SELECT pg_temp.stats_assert(pg_temp.stats_collect()->>'status'='READY','owner path');
SELECT pg_temp.stats_assert(pg_temp.stats_ready(),'owner consumes its own profiles');
SELECT pg_temp.stats_assert(pg_temp.stats_estimate()->>'uncertainty_unknown'='true',
 'unchanged counters do not invent a statistical confidence interval');
SELECT pg_temp.stats_denied('SELECT pg_graph_statistic_publish(lo,''[]'') FROM scopes WHERE role_name=current_user');
SELECT pg_temp.stats_denied('SELECT pg_graph_statistic_read(lo,l) FROM scopes WHERE role_name=current_user');
SELECT pg_temp.stats_denied('SELECT * FROM pg_graph_statistic_data');
RESET ROLE;

-- Arbitrarily old observation timestamps are not evidence of data changes.
SELECT pg_temp.stats_assert(pg_temp.stats_counter_matches(h.gstscope,
 jsonb_set(d.gstdmetadata->'change_baseline'->'counters','{sampled_at_us}','0'),0),
 'freshness has no wall-clock TTL')
FROM pg_graph_statistic h JOIN pg_graph_statistic_data d ON d.gstdstatid=h.oid
WHERE h.oid=(SELECT lo FROM scopes WHERE role_name='ggc_stats_owner') LIMIT 1;

-- MAINTAIN + complete SELECT is sufficient, while role scope stays exact.
SET ROLE ggc_stats_maintainer;
SELECT pg_temp.stats_assert(pg_temp.stats_estimate()->>'logical_state'='0',
 'another role never consumes the owner profile');
SELECT pg_temp.stats_denied('SELECT pg_graph_statistic_create(l,''{}'') FROM scopes WHERE role_name=''ggc_stats_owner''');
SELECT pg_temp.stats_denied('SELECT pg_graph_statistic_collect_path(lo,po,options) FROM scopes,config WHERE role_name=''ggc_stats_owner''');
SELECT pg_temp.stats_denied('SELECT pg_graph_statistic_drop(lo) FROM scopes WHERE role_name=''ggc_stats_owner''');
INSERT INTO scopes SELECT current_user,l,p,
 pg_graph_statistic_create(l,'{}'),pg_graph_statistic_create(p,'{}')
FROM (SELECT pg_temp.stats_scope() l,pg_temp.stats_scope(true) p) s;
SELECT pg_temp.stats_assert(pg_temp.stats_collect()->>'status'='READY','maintainer path');
SELECT pg_temp.stats_assert(pg_temp.stats_ready(),'maintainer consumes its own profiles');
RESET ROLE;

SET ROLE ggc_stats_reader;
SELECT pg_temp.stats_denied('SELECT pg_graph_statistic_create(pg_temp.stats_scope(),''{}'')');
SELECT pg_temp.stats_assert(pg_temp.stats_estimate()->>'logical_state'='0','reader has no profile');
RESET ROLE;
REVOKE SELECT ON e FROM ggc_stats_reader;
GRANT SELECT(id,src,dst),MAINTAIN ON e TO ggc_stats_reader;
GRANT MAINTAIN ON v TO ggc_stats_reader;
SET ROLE ggc_stats_reader;
SELECT pg_temp.stats_denied('SELECT pg_graph_statistic_create(pg_temp.stats_scope(),''{}'')');
RESET ROLE;

-- Revoking maintenance stops collection but not safe SELECT-only consumption.
REVOKE MAINTAIN ON e FROM ggc_stats_maintainer;
SET ROLE ggc_stats_maintainer;
SELECT pg_temp.stats_denied('SELECT pg_temp.stats_collect()');
SELECT pg_temp.stats_assert(pg_temp.stats_ready(),'SELECT-only consumption remains safe');
RESET ROLE;
GRANT MAINTAIN ON e TO ggc_stats_maintainer;
REVOKE SELECT ON e FROM ggc_stats_maintainer;
SET ROLE ggc_stats_maintainer;
SELECT pg_temp.stats_denied('SELECT pg_temp.stats_collect()');
SELECT pg_temp.stats_assert(pg_temp.stats_estimate()->>'logical_state'='3',
 'revoked complete visibility falls back');
RESET ROLE;
GRANT SELECT ON e TO ggc_stats_maintainer;
REVOKE SELECT ON g FROM ggc_stats_maintainer;
SET ROLE ggc_stats_maintainer;
SELECT pg_temp.stats_denied('SELECT pg_temp.stats_collect()');
RESET ROLE;
GRANT SELECT ON g TO ggc_stats_maintainer;

-- Even an owner/BYPASSRLS population is not used for an RLS relation.
ALTER TABLE e ENABLE ROW LEVEL SECURITY;
SET ROLE ggc_stats_owner;
SELECT pg_temp.stats_denied('SELECT pg_temp.stats_collect()');
SELECT pg_temp.stats_assert(pg_temp.stats_estimate()->>'logical_state'='3','RLS fallback');
RESET ROLE;
ALTER TABLE e DISABLE ROW LEVEL SECURITY;

-- Local unflushed DML must be observed even with a cached pgstat snapshot.
SET ROLE ggc_stats_owner;
BEGIN;
SET LOCAL stats_fetch_consistency=cache;
SELECT pg_temp.stats_assert(pg_temp.stats_collect()->>'status'='READY','pre-DML recollect');
SELECT pg_temp.stats_assert(pg_temp.stats_ready(),'collection reads do not stale counters');
INSERT INTO e VALUES(5,1,3,1),(6,1,4,1);
SELECT pg_temp.stats_assert(j->>'logical_state'='3' AND j->>'physical_state'='3'
 AND (j->'stages'->0->>'logical_fallback')::boolean
 AND (j->'stages'->0->>'physical_fallback')::boolean,'DML changes cause actual fallback')
FROM (SELECT pg_temp.stats_estimate() j) s;
ROLLBACK;
SELECT pg_temp.stats_assert(pg_temp.stats_collect()->>'status'='READY','refresh after rollback');
SELECT pg_temp.stats_assert(pg_temp.stats_ready(),'refresh restores usable profiles');
ANALYZE e;
SELECT pg_temp.stats_assert(pg_temp.stats_estimate()->>'logical_state'='3','ANALYZE baseline change');
SELECT pg_temp.stats_assert(pg_temp.stats_collect()->>'status'='READY','refresh after analyze');
SELECT pg_temp.stats_assert(pg_temp.stats_ready(),'ANALYZE then recollect is usable');
RESET ROLE;

-- Unknown counters must actually fall back; returning stale values is wrong.
SET track_counts=off;
SET ROLE ggc_stats_owner;
SELECT pg_temp.stats_assert(j->>'logical_state'='2' AND j->>'physical_state'='2'
 AND (j->'stages'->0->>'logical_fallback')::boolean,'unavailable counters are PARTIAL')
FROM (SELECT pg_temp.stats_estimate() j) s;
RESET ROLE;
SET track_counts=on;
SELECT pg_stat_reset_single_table_counters('e'::regclass);
SET ROLE ggc_stats_owner;
SELECT pg_temp.stats_assert(pg_temp.stats_estimate()->>'logical_state'='3','counter reset invalidates');
ALTER TABLE e ADD COLUMN description text;
SELECT pg_temp.stats_assert(pg_temp.stats_estimate()->>'logical_state'='3','DDL dependency invalidates');
RESET ROLE;

-- Dropping one's own scope remains possible after all base grants are lost.
REVOKE SELECT,MAINTAIN ON e,v FROM ggc_stats_maintainer;
SET ROLE ggc_stats_maintainer;
SELECT pg_graph_statistic_drop(lo),pg_graph_statistic_drop(po)
FROM scopes WHERE role_name=current_user;
RESET ROLE;
DROP SCHEMA ggc_stats_access CASCADE;
DROP ROLE ggc_stats_reader;
DROP ROLE ggc_stats_maintainer;
DROP ROLE ggc_stats_owner;
RESET search_path;
RESET stats_fetch_consistency;
SELECT 'graph_stats_access_ok';
