\pset format unaligned
\pset tuples_only on

CREATE TABLE ggc_fixed_edge(
 id bigint PRIMARY KEY,
 src bigint NOT NULL,
 dst bigint NOT NULL,
 payload text
) WITH (fillfactor=40);
CREATE INDEX ggc_fixed_out ON ggc_fixed_edge(src);
INSERT INTO ggc_fixed_edge VALUES
 (1,1,2,repeat('p1-',1700)),
 (2,1,2,repeat('p2-',1700)),
 (3,1,1,repeat('p3-',1700)),
 (4,2,3,repeat('p4-',1700)),
 (5,2,1,repeat('p5-',1700)),
 (6,3,4,repeat('p6-',1700)),
 (7,3,2,repeat('p7-',1700)),
 (8,4,5,repeat('p8-',1700)),
 (9,2,4,repeat('p9-',1700));
INSERT INTO ggc_fixed_edge
SELECT 100+i,99+i,100+i,repeat('chain-'||i||'-',700)
FROM generate_series(1,9) AS i;

CREATE TABLE ggc_fixed_request(
 anchor bigint NOT NULL,
 vertices bigint[],
 edges bigint[],
 payloads text[]
);
INSERT INTO ggc_fixed_request(anchor) VALUES (1);

SELECT test_graph_fixed_config(
 'ggc_fixed_request',
 array_fill('ggc_fixed_edge'::regclass,ARRAY[2]),
 array_fill('ggc_fixed_out'::regclass,ARRAY[2]),
 ARRAY[1,1],0,4,64,100000);
SELECT count(*),sum(anchor),
 array_agg(vertices::text ORDER BY edges::text),
 array_agg(edges::text ORDER BY edges::text),
 min(length(payloads[1])),min(length(payloads[2]))
FROM ggc_fixed_request;
EXPLAIN (COSTS OFF)
SELECT anchor,vertices,edges,payloads FROM ggc_fixed_request;

CREATE FUNCTION pg_temp.ggc_startup_metadata() RETURNS jsonb
LANGUAGE plpgsql AS $$
DECLARE p jsonb;
BEGIN
 EXECUTE 'EXPLAIN (ANALYZE, FORMAT JSON, TIMING OFF) SELECT anchor,vertices,edges,payloads FROM ggc_fixed_request' INTO p;
 RETURN p->0->'Plan';
END $$;
-- Native B-tree opts in; an identical second stage reuses locked metadata.
SELECT j->>'Graph Startup Revalidations',j->>'Graph Startup Metadata Reuses'
FROM (SELECT pg_temp.ggc_startup_metadata() j) s;

-- Two compatible stages retain just one provider instance; LIMIT still closes it.
SELECT test_graph_lifecycle_config('counted',ARRAY[]::regclass[],0,0,0,0);
SELECT count(*),sum(anchor),sum(cardinality(vertices)),sum(cardinality(edges)),
 sum(cardinality(payloads)) FROM ggc_fixed_request;
SELECT j->>'wrapper_begin',j->>'wrapper_end',j->>'wrapper_active'
FROM (SELECT test_graph_lifecycle_diagnostic()::jsonb j) s;
SELECT anchor,cardinality(vertices),cardinality(edges),cardinality(payloads)
FROM ggc_fixed_request LIMIT 1;
SELECT j->>'wrapper_begin',j->>'wrapper_end',j->>'wrapper_active'
FROM (SELECT test_graph_lifecycle_diagnostic()::jsonb j) s;
-- A registered wrapper has not opted in, so both stages validate independently.
SELECT j->>'Graph Startup Revalidations',
 NOT (j ? 'Graph Startup Metadata Reuses')
FROM (SELECT pg_temp.ggc_startup_metadata() j) s;
TRUNCATE ggc_fixed_request;
INSERT INTO ggc_fixed_request(anchor) VALUES (100);
SELECT test_graph_fixed_config(
 'ggc_fixed_request',
 array_fill('ggc_fixed_edge'::regclass,ARRAY[9]),
 array_fill('ggc_fixed_out'::regclass,ARRAY[9]),
 array_fill(1,ARRAY[9]),0,4,64,100000);
SELECT anchor,cardinality(vertices),cardinality(edges),vertices[10],edges[9],
 cardinality(payloads),length(payloads[9])
FROM ggc_fixed_request;
EXPLAIN (COSTS OFF)
SELECT anchor,vertices,edges,payloads FROM ggc_fixed_request;

-- Prefix factorization is opt-in and never changes an ordered request.
CREATE FUNCTION pg_temp.ggc_factorize(bigint) RETURNS bigint
AS '$libdir/test_graph_core','test_graph_fixed_factorize' LANGUAGE C STRICT;
SELECT pg_temp.ggc_factorize(1048576);
SELECT NOT (j ? 'Graph Prefix Representation')
FROM (SELECT pg_temp.ggc_startup_metadata() j) s;
SELECT test_graph_fixed_tuning(8,16,4,1048576,true,false);
SELECT anchor,cardinality(vertices),cardinality(edges),vertices[10],edges[9],
 cardinality(payloads),length(payloads[9])
FROM ggc_fixed_request;
SELECT j->>'Graph Prefix Representation',j->>'Factorized Arcs'
FROM (SELECT pg_temp.ggc_startup_metadata() j) s;

-- Duplicate anchors, parallel edges, a self-loop, cycles and TOAST payloads:
-- compare the entire multiset, not merely its cardinality.
TRUNCATE ggc_fixed_request;
INSERT INTO ggc_fixed_request(anchor) VALUES (1),(1);
SELECT test_graph_fixed_config(
 'ggc_fixed_request',
 array_fill('ggc_fixed_edge'::regclass,ARRAY[2]),
 array_fill('ggc_fixed_out'::regclass,ARRAY[2]),
 ARRAY[1,1],0,4,64,100000);
SELECT test_graph_fixed_tuning(8,16,4,1048576,true,false);
SELECT pg_temp.ggc_factorize(1048576);
WITH ggc_factor_actual AS MATERIALIZED (
 SELECT anchor,vertices,edges,payloads FROM ggc_fixed_request),
ggc_factor_expected AS MATERIALIZED (
 SELECT r.anchor,ARRAY[r.anchor,e1.dst,e2.dst] vertices,
 ARRAY[e1.id,e2.id] edges,ARRAY[e1.payload,e2.payload] payloads
 FROM (VALUES (1::bigint),(1::bigint)) r(anchor)
 JOIN ggc_fixed_edge e1 ON e1.src=r.anchor
 JOIN ggc_fixed_edge e2 ON e2.src=e1.dst)
SELECT (SELECT count(*) FROM ggc_factor_actual), (SELECT count(*) FROM (
 (TABLE ggc_factor_actual EXCEPT ALL TABLE ggc_factor_expected)
 UNION ALL
 (TABLE ggc_factor_expected EXCEPT ALL TABLE ggc_factor_actual)) differences);
SELECT j->>'Factorized Arcs',(j->>'Actual Rows')::numeric::bigint
FROM (SELECT pg_temp.ggc_startup_metadata() j) s;
-- Wrappers do not inherit the path-independence capability.
-- Both distinct second-stage endpoints share one fixed-runtime batch.
SELECT j->>'Stage 1 Batch Summary' LIKE 'executions=1 %',
 j->>'Stage 2 Batch Summary' LIKE 'executions=1 %'
FROM (SELECT pg_temp.ggc_startup_metadata() j) s;
SELECT test_graph_lifecycle_config('counted',ARRAY[]::regclass[],0,0,0,0);
SELECT NOT (j ? 'Graph Prefix Representation')
FROM (SELECT pg_temp.ggc_startup_metadata() j) s;
-- EOF is sticky across anchor batches, but not across an explicit Rescan.
-- A real anchor IndexScan used to restart after a short final batch.
TRUNCATE ggc_fixed_request;
ALTER TABLE ggc_fixed_request ADD PRIMARY KEY (anchor);
INSERT INTO ggc_fixed_request(anchor) SELECT 1000+i FROM generate_series(1,32) i;
INSERT INTO ggc_fixed_edge
 SELECT 200+i,1000+i,1000+i,'eof-'||i FROM generate_series(1,32) i;
ANALYZE ggc_fixed_request;
ANALYZE ggc_fixed_edge;
SET enable_seqscan = off;
SET enable_bitmapscan = off;
SET enable_material = off;
SET enable_memoize = off;
DO $$
DECLARE
 h integer; n integer; factor boolean; q text; p jsonb; diff bigint;
 actual_count bigint; rescans bigint;
BEGIN
 FOREACH factor IN ARRAY ARRAY[false,true] LOOP
  FOREACH h IN ARRAY ARRAY[1,2,4,9] LOOP
   PERFORM test_graph_fixed_config('ggc_fixed_request',
    array_fill('ggc_fixed_edge'::regclass,ARRAY[h]),
    array_fill('ggc_fixed_out'::regclass,ARRAY[h]),
    array_fill(1,ARRAY[h]),0,16,64,100000);
   PERFORM test_graph_fixed_tuning(8,16,4,1048576,true,false);
   PERFORM pg_temp.ggc_factorize(CASE WHEN factor THEN 1048576 ELSE 0 END);
   FOREACH n IN ARRAY ARRAY[0,1,15,16,17,32] LOOP
    q := format('SELECT anchor,vertices,edges,payloads FROM ggc_fixed_request WHERE anchor<=%s',1000+n);
    EXECUTE 'WITH a AS MATERIALIZED ('||q||'), e AS MATERIALIZED ('||
     format('SELECT 1000+i::bigint, array_fill(1000+i::bigint,ARRAY[%s]),
      array_fill(200+i::bigint,ARRAY[%s]),array_fill(''eof-''||i,ARRAY[%s])
      FROM generate_series(1,%s) i)',h+1,h,h,n)||
     ' SELECT (SELECT count(*) FROM a),(SELECT count(*) FROM (
      (TABLE a EXCEPT ALL TABLE e) UNION ALL
      (TABLE e EXCEPT ALL TABLE a)) d)' INTO actual_count,diff;
    IF actual_count<>n OR diff<>0 THEN
     RAISE EXCEPTION 'EOF bag mismatch factor=% hops=% anchors=%',factor,h,n;
    END IF;
    EXECUTE 'EXPLAIN (ANALYZE,FORMAT JSON,TIMING OFF) '||q INTO p;
    p := p->0->'Plan';
    IF p->>'Node Type'<>'Custom Scan' OR
       p#>>'{Plans,0,Node Type}'<>'Index Scan' OR
       (p->>'Anchor Bindings')::bigint<>n OR
       (p->>'Anchor Batches')::bigint<>(n+15)/16 OR
       (p ? 'Graph Prefix Representation')<>factor THEN
     RAISE EXCEPTION 'EOF plan mismatch: %',p;
    END IF;
   END LOOP;
   -- The same child is rescanned after EOF, with changed outer parameters.
   q := 'SELECT r.* FROM (VALUES (1000::bigint),(1001),(1017),(1000),(1032)) s(a)
    CROSS JOIN LATERAL (SELECT anchor,vertices,edges,payloads
     FROM ggc_fixed_request WHERE anchor=s.a OFFSET 0) r';
   EXECUTE 'WITH a AS MATERIALIZED ('||q||'), e AS MATERIALIZED ('||
    format('SELECT 1000+i::bigint,array_fill(1000+i::bigint,ARRAY[%s]),
     array_fill(200+i::bigint,ARRAY[%s]),array_fill(''eof-''||i,ARRAY[%s])
     FROM (VALUES (1),(17),(32)) t(i))',h+1,h,h)||
    ' SELECT count(*) FROM ((TABLE a EXCEPT ALL TABLE e) UNION ALL
     (TABLE e EXCEPT ALL TABLE a)) d' INTO diff;
   IF diff<>0 THEN RAISE EXCEPTION 'Rescan bag mismatch'; END IF;
   EXECUTE 'EXPLAIN (ANALYZE,FORMAT JSON,TIMING OFF) '||q INTO p;
   WITH RECURSIVE plans(j) AS (
    SELECT p->0->'Plan' UNION ALL
    SELECT c FROM plans CROSS JOIN LATERAL jsonb_array_elements(j->'Plans') c)
   SELECT max((j->>'Graph Rescans')::bigint) INTO rescans FROM plans;
   IF rescans IS NULL OR rescans<4 THEN
    RAISE EXCEPTION 'Missing real Graph Rescan: %',p;
   END IF;
   EXECUTE 'SELECT count(*) FROM ('||
    'SELECT anchor,vertices,edges,payloads FROM ggc_fixed_request WHERE anchor=1001 LIMIT 1) q'
    INTO actual_count;
   IF actual_count<>1 THEN RAISE EXCEPTION 'LIMIT lost final partial batch'; END IF;
  END LOOP;
 END LOOP;
END $$;
RESET enable_memoize;
RESET enable_material;
RESET enable_bitmapscan;
RESET enable_seqscan;
DROP TABLE ggc_fixed_request;
DROP TABLE ggc_fixed_edge;
