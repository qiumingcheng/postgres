\pset format unaligned
\pset tuples_only on
CREATE TABLE ggc_btree_edge(id bigint PRIMARY KEY,src bigint NOT NULL,dst bigint NOT NULL,payload text) WITH(fillfactor=40);
CREATE INDEX ggc_btree_out ON ggc_btree_edge(src);
CREATE INDEX ggc_btree_in ON ggc_btree_edge(dst);
INSERT INTO ggc_btree_edge VALUES (101,10,20,'a'),(102,10,20,'b'),(103,10,30,'c'),(104,20,30,'d'),(105,30,10,'e'),(106,10,10,'f'),(107,40,20,'g');
SELECT array_agg((r->>'id')::bigint ORDER BY (r->>'id')::bigint)
FROM jsonb_array_elements(test_graph_btree_provider('ggc_btree_edge','ggc_btree_out',1,ARRAY[10]::bigint[],2)::jsonb->'rows') r;
SELECT array_agg((r->>'id')::bigint ORDER BY (r->>'id')::bigint)
FROM jsonb_array_elements(test_graph_btree_provider('ggc_btree_edge','ggc_btree_in',2,ARRAY[20]::bigint[],1)::jsonb->'rows') r;
SELECT count(*)=8 AND bool_and(NOT (r->>'opposite_available')::boolean)
FROM jsonb_array_elements(test_graph_btree_provider('ggc_btree_edge','ggc_btree_out',1,ARRAY[10,10]::bigint[],3)::jsonb->'rows') r;
SELECT test_graph_btree_provider('ggc_btree_edge','ggc_btree_out',1,ARRAY[999]::bigint[],2)::jsonb->'rows';
UPDATE ggc_btree_edge SET payload='hot' WHERE id=101;
SELECT r->>'root_tid' <> r->>'visible_tid' AND (r->>'opposite')::bigint=20
FROM jsonb_array_elements(test_graph_btree_provider('ggc_btree_edge','ggc_btree_out',1,ARRAY[10]::bigint[],1)::jsonb->'rows') r WHERE (r->>'id')::bigint=101;
SELECT test_graph_btree_provider('ggc_btree_edge','ggc_btree_out',1,ARRAY[10]::bigint[],2,13)::jsonb;
SELECT test_graph_btree_provider('ggc_btree_edge','ggc_btree_out',1,ARRAY[10]::bigint[],2,3);
SELECT test_graph_btree_provider('ggc_btree_edge','ggc_btree_out',1,ARRAY[10]::bigint[],2)::jsonb->'snapshot_borrowed';
-- begin_batch is a complete validation boundary even without validate().
SELECT array_agg((r->>'id')::bigint ORDER BY (r->>'id')::bigint)
FROM jsonb_array_elements(test_graph_btree_provider('ggc_btree_edge','ggc_btree_out',1,ARRAY[10]::bigint[],2,100)::jsonb->'rows') r;
DO $$
DECLARE m integer; rejected boolean;
BEGIN
  FOR m IN 110..120 LOOP
    rejected := false;
    BEGIN
      PERFORM test_graph_btree_provider('ggc_btree_edge','ggc_btree_out',1,ARRAY[10,20]::bigint[],2,m);
    EXCEPTION WHEN feature_not_supported THEN rejected := true;
    END;
    IF NOT rejected THEN RAISE EXCEPTION 'direct begin accepted invalid mode %',m; END IF;
  END LOOP;
END $$;
-- A valid request with a mismatched index still fails the held-lock check.
SELECT test_graph_btree_provider('ggc_btree_edge','ggc_btree_in',1,ARRAY[10]::bigint[],2,100);
CREATE ROLE ggc_btree_begin_denied;
SET ROLE ggc_btree_begin_denied;
SELECT test_graph_btree_provider('ggc_btree_edge','ggc_btree_out',1,ARRAY[10]::bigint[],2,100);
RESET ROLE;
DROP ROLE ggc_btree_begin_denied;
ALTER TABLE ggc_btree_edge ENABLE ROW LEVEL SECURITY;
SELECT test_graph_btree_provider('ggc_btree_edge','ggc_btree_out',1,ARRAY[10]::bigint[],2,100);
ALTER TABLE ggc_btree_edge DISABLE ROW LEVEL SECURITY;
SELECT test_graph_btree_provider('ggc_btree_edge','ggc_btree_out',1,ARRAY[10]::bigint[],2,100)::jsonb->'snapshot_borrowed';
-- Ordered distinct requests share one array scan, including missing keys and
-- paused one-candidate delivery. Request IDs and global ordinals are retained.
WITH cases(direction,idx,keys,expected) AS (VALUES
 (1,'ggc_btree_out'::regclass,ARRAY[0,10,15,20,30,40,999]::bigint[],
  '2:101,2:102,2:103,2:106,4:104,5:105,6:107'),
 (2,'ggc_btree_in'::regclass,ARRAY[10,20,30]::bigint[],
  '1:105,1:106,2:101,2:102,2:107,3:103,3:104')
), results AS MATERIALIZED (
 SELECT direction,capacity,expected,test_graph_btree_provider('ggc_btree_edge',idx,
  direction,keys,capacity,0,0,2,2)::jsonb d
 FROM cases CROSS JOIN (VALUES(1),(3),(16)) caps(capacity)
)
SELECT count(*)=6,bool_and((d->>'array_scans')::int=1),
 bool_and((SELECT string_agg(format('%s:%s',r->>'request',r->>'id'),',' ORDER BY n)
  FROM jsonb_array_elements(d->'rows') WITH ORDINALITY x(r,n))=expected),
 bool_and((SELECT bool_and((r->>'ordinal')::bigint=n-1)
  FROM jsonb_array_elements(d->'rows') WITH ORDINALITY x(r,n))),
 bool_and((SELECT max(v::int)<=least(capacity,2) FROM jsonb_array_elements_text(d->'batches') v))
FROM results;
-- The owned request array survives mutation of caller storage before getnext.
SELECT (d->>'array_scans')::int=1,(d->>'raw_count')::int=7
FROM (SELECT test_graph_btree_provider('ggc_btree_edge','ggc_btree_out',1,
 ARRAY[0,10,20,30,40]::bigint[],1,2)::jsonb d) s;
-- A partial array -> scalar -> array rescan must neither keep stale keys nor
-- lose the next scan's first candidate; same immutable execution scope.
SELECT (d->>'array_scans')::int=2,
 (SELECT string_agg(format('%s:%s',r->>'phase',r->>'id'),',' ORDER BY n)
 FROM jsonb_array_elements(d->'rows') WITH ORDINALITY x(r,n))=
 '0:101,1:104,2:101,2:102,2:103,2:106,2:104,2:105'
FROM (SELECT test_graph_btree_provider('ggc_btree_edge','ggc_btree_out',1,
 ARRAY[10,20,30]::bigint[],1,7)::jsonb d) s;
CREATE INDEX ggc_btree_desc ON ggc_btree_edge(src DESC);
CREATE INDEX ggc_btree_include ON ggc_btree_edge(src) INCLUDE(dst);
CREATE INDEX ggc_btree_compound ON ggc_btree_edge(src,dst);
-- Non-ascending/duplicate requests and compound keys retain scalar scans;
-- INCLUDE columns preserve single-key order and now allow array probes.
WITH results AS MATERIALIZED (
 SELECT test_graph_btree_provider('ggc_btree_edge',idx,1,keys,1)::jsonb d,expected,arrays
 FROM (VALUES
  ('ggc_btree_out'::regclass,ARRAY[20,10,10]::bigint[],9,0),
  ('ggc_btree_desc'::regclass,ARRAY[10,20]::bigint[],5,0),
  ('ggc_btree_include'::regclass,ARRAY[10,20]::bigint[],5,1),
  ('ggc_btree_compound'::regclass,ARRAY[10,20]::bigint[],5,0)
 ) v(idx,keys,expected,arrays)
)
SELECT count(*)=4,bool_and((d->>'array_scans')::int=arrays),
 bool_and((d->>'raw_count')::int=expected) FROM results;
-- Empty arrays of matches still complete, and signed int8 extremes compare
-- without subtraction overflow.
SELECT (d->>'array_scans')::int=1,(d->>'raw_count')::int=0
FROM (SELECT test_graph_btree_provider('ggc_btree_edge','ggc_btree_out',1,
 ARRAY[-99,99]::bigint[],1)::jsonb d) s;
INSERT INTO ggc_btree_edge VALUES
 (108,'-9223372036854775808',20,'min'),(109,'9223372036854775807',20,'max');
SELECT (d->>'array_scans')::int=1,
 (SELECT array_agg((r->>'id')::int ORDER BY n)
 FROM jsonb_array_elements(d->'rows') WITH ORDINALITY x(r,n))=ARRAY[108,109]
FROM (SELECT test_graph_btree_provider('ggc_btree_edge','ggc_btree_out',1,
 ARRAY['-9223372036854775808','-1','9223372036854775807']::bigint[],1)::jsonb d) s;
-- Real multi-page/posting-list delivery agrees exactly with separate probes.
CREATE TABLE ggc_btree_array_edge(id bigint,src bigint NOT NULL,dst bigint NOT NULL,payload text);
INSERT INTO ggc_btree_array_edge SELECT i,i/50,i+10000,'x' FROM generate_series(1,4000) i;
CREATE INDEX ggc_btree_array_out ON ggc_btree_array_edge(src);
VACUUM ggc_btree_array_edge;
WITH keys AS (SELECT ARRAY[0,1,2,20,40,60,80,90]::bigint[] k),
 full_batch AS MATERIALIZED (
 SELECT test_graph_btree_provider('ggc_btree_array_edge','ggc_btree_array_out',1,k,7)::jsonb d FROM keys
), singles AS MATERIALIZED (
 SELECT key_order,d FROM keys,unnest(k) WITH ORDINALITY x(k,key_order),
 LATERAL (SELECT test_graph_btree_provider('ggc_btree_array_edge','ggc_btree_array_out',1,ARRAY[x.k],7)::jsonb d) q
)
SELECT (d->>'array_scans')::int=1,
 (SELECT jsonb_agg(r-'ordinal'-'request' ORDER BY n)
  FROM jsonb_array_elements(d->'rows') WITH ORDINALITY x(r,n))=
 (SELECT jsonb_agg(r-'ordinal'-'request' ORDER BY key_order,n)
  FROM singles,jsonb_array_elements(singles.d->'rows') WITH ORDINALITY x(r,n))
FROM full_batch;
DROP TABLE ggc_btree_array_edge;
DROP TABLE ggc_btree_edge;
