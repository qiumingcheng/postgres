\pset format unaligned
\pset tuples_only on
SET client_min_messages=warning;
CREATE TABLE ggc_prov_vertex(id bigint PRIMARY KEY,name text);
CREATE TABLE ggc_prov_edge(
 id bigint PRIMARY KEY,
 src bigint NOT NULL REFERENCES ggc_prov_vertex(id),
 dst bigint NOT NULL REFERENCES ggc_prov_vertex(id),
 payload text);
INSERT INTO ggc_prov_vertex VALUES (1,'a'),(2,'b'),(3,'c'),(4,'isolate');
INSERT INTO ggc_prov_edge VALUES
 (101,1,2,'x'),(102,1,2,'parallel'),(103,2,3,'y'),(104,3,3,'loop');
CREATE PROPERTY GRAPH ggc_prov_graph
 VERTEX TABLES(ggc_prov_vertex)
 EDGE TABLES(ggc_prov_edge
  SOURCE KEY(src) REFERENCES ggc_prov_vertex(id)
  DESTINATION KEY(dst) REFERENCES ggc_prov_vertex(id));

-- The GraphPattern has already been cleared, but its fixed provenance exists.
WITH diagnostic AS
(
 SELECT test_graph_pgq_provenance($q$
  SELECT pad.x,gt.edge_id,gt.src_id,gt.dst_id
  FROM (VALUES (7)) pad(x),
       GRAPH_TABLE(ggc_prov_graph
        MATCH (a IS ggc_prov_vertex)-[e IS ggc_prov_edge]->(b IS ggc_prov_vertex)
        COLUMNS(e.id AS edge_id,a.id AS src_id,b.id AS dst_id)) gt
 $q$)::jsonb AS d
)
SELECT d->>'before_programs'='1' AND d->>'before_graph_patterns'='0'
FROM diagnostic;

-- Pull-up offsets the three local RTIs past the outer query's entries.
WITH diagnostic AS
(
 SELECT test_graph_pgq_provenance($q$
  SELECT pad.x,gt.edge_id,gt.src_id,gt.dst_id
  FROM (VALUES (7)) pad(x),
       GRAPH_TABLE(ggc_prov_graph
        MATCH (a IS ggc_prov_vertex)-[e IS ggc_prov_edge]->(b IS ggc_prov_vertex)
        COLUMNS(e.id AS edge_id,a.id AS src_id,b.id AS dst_id)) gt
 $q$)::jsonb AS d
), variables AS
(
 SELECT d,jsonb_array_elements(d->'variables') AS v FROM diagnostic
)
SELECT bool_and((d->>'top_program')::boolean
                AND (d->>'mapping_valid')::boolean
                AND (v->>'mapped')::boolean
                AND (v->>'rti')::integer > 2)
       AND count(*)=3
FROM variables;

-- The optimization sidecar must not change the executable relational fallback.
WITH graph_result AS
(
 SELECT pad.x,gt.edge_id,gt.src_id,gt.dst_id
 FROM (VALUES (7)) pad(x),
      GRAPH_TABLE(ggc_prov_graph
       MATCH (a IS ggc_prov_vertex)-[e IS ggc_prov_edge]->(b IS ggc_prov_vertex)
       COLUMNS(e.id AS edge_id,a.id AS src_id,b.id AS dst_id)) gt
), relational_result AS
(
 SELECT pad.x,e.id AS edge_id,a.id AS src_id,b.id AS dst_id
 FROM (VALUES (7)) pad(x)
 CROSS JOIN ggc_prov_edge e
 JOIN ggc_prov_vertex a ON a.id=e.src
 JOIN ggc_prov_vertex b ON b.id=e.dst
), difference AS
(
 (SELECT * FROM graph_result EXCEPT ALL SELECT * FROM relational_result)
 UNION ALL
 (SELECT * FROM relational_result EXCEPT ALL SELECT * FROM graph_result)
)
SELECT NOT EXISTS(SELECT FROM difference);

-- A stale/invalid RTI is rejected before it can reach a graph path.
WITH diagnostic AS
(
 SELECT test_graph_pgq_invalid_mapping($q$
  SELECT pad.x,gt.edge_id,gt.src_id,gt.dst_id
  FROM (VALUES (7)) pad(x),
       GRAPH_TABLE(ggc_prov_graph
        MATCH (a IS ggc_prov_vertex)-[e IS ggc_prov_edge]->(b IS ggc_prov_vertex)
        COLUMNS(e.id AS edge_id,a.id AS src_id,b.id AS dst_id)) gt
 $q$)::jsonb AS d
)
SELECT d->>'before_programs'='1'
       AND d->>'after_programs'='0'
       AND (d->>'top_disabled')::boolean
       AND NOT (d->>'mapping_valid')::boolean
FROM diagnostic;

-- Undirected provenance survives pull-up with valid original-table mappings.
WITH diagnostic AS
(
 SELECT test_graph_pgq_provenance($q$
  SELECT * FROM GRAPH_TABLE(ggc_prov_graph
   MATCH (a IS ggc_prov_vertex)-[e IS ggc_prov_edge]-(b IS ggc_prov_vertex)
   COLUMNS(e.id AS edge_id,a.id AS src_id,b.id AS dst_id))
 $q$)::jsonb AS d
)
SELECT d->>'before_programs'='1'
       AND d->>'after_programs'='1'
       AND (d->>'top_program')::boolean
       AND NOT (d->>'top_disabled')::boolean
       AND (d->>'mapping_valid')::boolean
FROM diagnostic;
SELECT count(*) > 0 FROM GRAPH_TABLE(ggc_prov_graph
 MATCH (a IS ggc_prov_vertex)-[e IS ggc_prov_edge]-(b IS ggc_prov_vertex)
 COLUMNS(e.id AS edge_id,a.id AS src_id,b.id AS dst_id));

DROP PROPERTY GRAPH ggc_prov_graph;
DROP TABLE ggc_prov_edge;
DROP TABLE ggc_prov_vertex;
