\pset format unaligned
\pset tuples_only on
-- Both sides of the 64-parent boundary; every handle equality field, invalid
-- provider sizes, NONE garbage and name bytes after NUL; ordered and unordered
-- parents; every invocation repeats the mapping through the public Rescan API.
SELECT count(*) = 112, bool_and(test_graph_batch_mapping(n, mode, ordered))
FROM (VALUES (1), (64), (65), (1024)) sizes(n)
CROSS JOIN generate_series(0, 13) modes(mode)
CROSS JOIN (VALUES (true), (false)) orders(ordered);

CREATE TABLE ggc_batch_resources_edge(id bigint PRIMARY KEY, src bigint NOT NULL, dst bigint NOT NULL);
INSERT INTO ggc_batch_resources_edge SELECT i, i, i + 1 FROM generate_series(1, 4096) i;
CREATE INDEX ggc_batch_resources_out ON ggc_batch_resources_edge(src);
-- Large real B-tree batches preserve the first-seen request IDs and complete
-- parent x edge bag, including reverse/duplicate parent ordinals and Rescan.
WITH cases AS MATERIALIZED (
 SELECT ordered, mode, test_graph_batch('ggc_batch_resources_edge',
 'ggc_batch_resources_out', 1,
 ARRAY(SELECT ((i * 7) % 137 + 1)::bigint FROM generate_series(0, 1023) i),
 CASE WHEN ordered THEN ARRAY(SELECT i::bigint FROM generate_series(0, 1023) i)
 ELSE ARRAY(SELECT (i % 13)::bigint FROM generate_series(0, 1023) i) END,
 64, 32, 4, 65536, 2097152, false, 0, 1, false, mode, 0)::jsonb d
 FROM (VALUES (true), (false)) orders(ordered)
 CROSS JOIN (VALUES (3), (14), (20)) modes(mode)
)
SELECT count(*) = 6,
 bool_and((d->>'row_count')::int = 2048 AND
 (d#>>'{instrumentation,endpoint_requests}')::int = 274 AND
 (d#>>'{instrumentation,shared_parent_requests}')::int = 1774 AND
 (d#>>'{instrumentation,rescan_count}')::int = 1),
 bool_and((SELECT count(*) = 2048 AND
  bool_and((r->>'edge_id')::int = (((r->>'parent_id')::int - 1000) * 7) % 137 + 1
   AND (r->>'opposite')::int = (r->>'edge_id')::int + 1
   AND (r->>'request')::int = ((r->>'parent_id')::int - 1000) % 137 + 1)
  FROM jsonb_array_elements(d->'rows') r))
FROM cases;
-- The sorting workspace must also fit: reject before provider startup.
SELECT test_graph_batch('ggc_batch_resources_edge','ggc_batch_resources_out',1,
 ARRAY(SELECT i::bigint FROM generate_series(1,4096) i),
 ARRAY(SELECT i::bigint FROM generate_series(1,4096) i),
 64,32,4,65536,1024,false,0,1,false,0,0);
SELECT (test_graph_batch_cleanup_state()::jsonb->>'active')::int=0;
DROP TABLE ggc_batch_resources_edge;
