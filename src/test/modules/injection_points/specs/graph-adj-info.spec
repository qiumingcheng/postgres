# Snapshot a diagnostic direction before INSERT seals its full active segment.
# First permutation covers inline + active; the second also has an existing
# packed base, covering the former unlock between collect_segment calls.
setup
{
  CREATE EXTENSION injection_points;
  CREATE TABLE graph_adj_info_edge (src bigint NOT NULL, dst bigint NOT NULL);
  CREATE INDEX graph_adj_info_idx ON graph_adj_info_edge USING graph_adj (src, dst);
  INSERT INTO graph_adj_info_edge SELECT 1, 2 FROM generate_series(1, 16);
}
teardown
{
  DROP TABLE graph_adj_info_edge;
  DROP EXTENSION injection_points;
}

session reader
setup
{
  DO $$BEGIN PERFORM injection_points_set_local(); PERFORM injection_points_attach('graph-adj-info-direction-copied', 'wait'); END$$;
}
step diagnose { DO $$BEGIN IF (graph_adj_index_info('graph_adj_info_idx')::jsonb->>'opposite_refs_valid')::boolean IS DISTINCT FROM true THEN RAISE EXCEPTION 'invalid opposite references'; END IF; END$$; }
step check17 { DO $$DECLARE j jsonb := graph_adj_index_info('graph_adj_info_idx')::jsonb; BEGIN IF (j->>'out_physical_entries')::int IS DISTINCT FROM 17 OR (j->>'in_physical_entries')::int IS DISTINCT FROM 17 OR (j->>'base_entries')::int IS DISTINCT FROM 16 THEN RAISE EXCEPTION 'missing insertion or compaction'; END IF; END$$; }
step check25 { DO $$DECLARE j jsonb := graph_adj_index_info('graph_adj_info_idx')::jsonb; BEGIN IF (j->>'out_physical_entries')::int IS DISTINCT FROM 25 OR (j->>'in_physical_entries')::int IS DISTINCT FROM 25 OR (j->>'base_entries')::int IS DISTINCT FROM 32 THEN RAISE EXCEPTION 'missing insertion or second compaction'; END IF; END$$; }

session writer
step prime { INSERT INTO graph_adj_info_edge SELECT 1, 2 FROM generate_series(1, 8); }
step insert { INSERT INTO graph_adj_info_edge VALUES (1, 2); }
step detach { DO $$BEGIN PERFORM injection_points_detach('graph-adj-info-direction-copied'); END$$; }
step wake { DO $$BEGIN PERFORM injection_points_wakeup('graph-adj-info-direction-copied'); END$$; }

permutation diagnose insert detach wake check17
permutation prime diagnose insert detach wake check25
