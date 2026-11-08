\set ECHO none
SET client_min_messages=warning;
LOAD 'test_graph_core';
SET jit=off;
CREATE SCHEMA ggc_orders;
SET search_path=ggc_orders,public;
CREATE TABLE v(id bigint PRIMARY KEY,name text);
CREATE TABLE e(id bigint PRIMARY KEY,src bigint NOT NULL,dst bigint NOT NULL,payload text,amount numeric);
INSERT INTO v SELECT i,'v-'||i FROM generate_series(1,6) i;
INSERT INTO e VALUES(11,1,2,'a',1),(12,1,2,NULL,2),(13,2,3,'b',3),
 (14,3,1,'c',4),(15,2,2,'self',5),(16,2,2,'parallel-self',6),(17,4,2,'reverse',7),
 (18,2,99,'missing',8),(19,99,2,'missing-reverse',9);
CREATE INDEX e_out ON e(src);
CREATE INDEX e_in ON e(dst);
CREATE PROPERTY GRAPH g VERTEX TABLES(v KEY(id)) EDGE TABLES(e KEY(id)
 SOURCE KEY(src) REFERENCES v(id) DESTINATION KEY(dst) REFERENCES v(id));
ANALYZE v;
ANALYZE e;
CREATE FUNCTION pg_temp.order_bag(q text) RETURNS jsonb LANGUAGE plpgsql AS $$
DECLARE r record; result jsonb='[]';
BEGIN
 FOR r IN EXECUTE q LOOP result:=result||jsonb_build_array(to_jsonb(r)); END LOOP;
 RETURN (SELECT coalesce(jsonb_agg(value ORDER BY value),'[]'::jsonb) FROM jsonb_array_elements(result));
END $$;
CREATE FUNCTION pg_temp.order_check(q text,oracle text) RETURNS void LANGUAGE plpgsql AS $$
DECLARE p jsonb; g jsonb; expected jsonb; actual jsonb; budget int; sje text;
BEGIN
 expected:=pg_temp.order_bag(oracle);
 sje:=current_setting('enable_self_join_elimination');
 IF pg_temp.order_bag('/*+ GraphRoute(relational) */ '||q) IS DISTINCT FROM expected THEN
   RAISE EXCEPTION 'relational oracle mismatch: %',q;
 END IF;
 FOREACH budget IN ARRAY ARRAY[1,8] LOOP
   PERFORM set_config('graph_btree_max_candidates',budget::text,true);
   actual:=pg_temp.order_bag('/*+ GraphRoute(btree) */ '||q);
   IF actual IS DISTINCT FROM expected THEN RAISE EXCEPTION 'order bag mismatch: %, %, %, %',budget,q,actual,expected; END IF;
   EXECUTE 'EXPLAIN(ANALYZE,TIMING OFF,FORMAT JSON) /*+ GraphRoute(btree) */ '||q INTO p;
   WITH RECURSIVE plans(n) AS (SELECT p->0->'Plan' UNION ALL
     SELECT c FROM plans CROSS JOIN LATERAL jsonb_array_elements(n->'Plans') c)
   SELECT n INTO g FROM plans WHERE n->>'Custom Plan Provider'='GraphExpandFixed';
   IF g IS NULL OR (g->>'Graph Plan Candidates')::int NOT BETWEEN 1 AND budget OR
      (budget=1 AND (g->>'Graph Anchor Position')::int<>0) THEN RAISE EXCEPTION 'order budget/plan: %',p; END IF;
   IF current_setting('enable_self_join_elimination')<>sje THEN RAISE EXCEPTION 'self-join GUC leak'; END IF;
 END LOOP;
END $$;

DO $$
DECLARE d1 text; d2 text; anchor text; condition text; tail text; q text; oracle text;
        x_join text; y_join text;
 columns text='a.id AS a,b.id AS b,z.id AS z,x.id AS x,y.id AS y,x.src AS xs,x.dst AS xd,y.src AS ys,y.dst AS yd,a.name AS aname,b.name AS bname,z.name AS zname,x.payload AS xp,y.payload AS yp,x.amount AS xa,y.amount AS ya';
BEGIN
 FOREACH d1 IN ARRAY ARRAY['out','in','both'] LOOP
 FOREACH d2 IN ARRAY ARRAY['out','in','both'] LOOP
   x_join:=CASE d1 WHEN 'out' THEN 'x.src=a.id AND x.dst=b.id'
     WHEN 'in' THEN 'x.dst=a.id AND x.src=b.id'
     ELSE '((x.src=a.id AND x.dst=b.id) OR (x.dst=a.id AND x.src=b.id))' END;
   y_join:=CASE d2 WHEN 'out' THEN 'y.src=b.id AND y.dst=z.id'
     WHEN 'in' THEN 'y.dst=b.id AND y.src=z.id'
     ELSE '((y.src=b.id AND y.dst=z.id) OR (y.dst=b.id AND y.src=z.id))' END;
   FOREACH anchor IN ARRAY ARRAY['a.id=2::bigint','b.id=2::bigint','z.id=2::bigint','true'] LOOP
   FOREACH tail IN ARRAY ARRAY['',' AND x.amount<y.amount',' AND a.id=z.id',' AND a.id=b.id'] LOOP
     condition:=anchor||tail;
     q:='SELECT * FROM GRAPH_TABLE(g MATCH (a IS v)'||
       CASE d1 WHEN 'out' THEN '-[x IS e]->' WHEN 'in' THEN '<-[x IS e]-' ELSE '-[x IS e]-' END||
       '(b IS v)'||CASE d2 WHEN 'out' THEN '-[y IS e]->' WHEN 'in' THEN '<-[y IS e]-' ELSE '-[y IS e]-' END||
       '(z IS v) WHERE '||condition||' COLUMNS('||columns||')) gt';
     oracle:='SELECT '||columns||' FROM v a JOIN e x ON true JOIN v b ON '||x_join||
       ' JOIN e y ON true JOIN v z ON '||y_join||' WHERE '||condition;
     PERFORM pg_temp.order_check(q,oracle);
   END LOOP;
   END LOOP;
 END LOOP;
 END LOOP;
END $$;

-- Saved custom/generic plans retain physical positions and logical outputs.
PREPARE graph_order(bigint) AS /*+ GraphRoute(btree) */ SELECT * FROM GRAPH_TABLE(g MATCH
 (a IS v)-[x IS e]->(b IS v)-[y IS e]-(z IS v) WHERE b.id=$1
 COLUMNS(a.id AS a,b.id AS b,z.id AS z,x.src AS xs,x.dst AS xd,y.src AS ys,y.dst AS yd,x.amount AS xa,y.amount AS ya)) gt;
DO $$
DECLARE mode text; anchor int; expected jsonb;
BEGIN
 FOREACH mode IN ARRAY ARRAY['force_generic_plan','force_custom_plan'] LOOP
   PERFORM set_config('plan_cache_mode',mode,true);
   FOREACH anchor IN ARRAY ARRAY[1,2,4,2] LOOP
     expected:=pg_temp.order_bag(format('SELECT a.id AS a,b.id AS b,z.id AS z,x.src AS xs,x.dst AS xd,y.src AS ys,y.dst AS yd,x.amount AS xa,y.amount AS ya FROM v a JOIN e x ON x.src=a.id JOIN v b ON b.id=x.dst JOIN e y ON true JOIN v z ON ((y.src=b.id AND y.dst=z.id) OR (y.dst=b.id AND y.src=z.id)) WHERE b.id=%s',anchor));
     IF pg_temp.order_bag(format('EXECUTE graph_order(%s)',anchor)) IS DISTINCT FROM expected THEN
       RAISE EXCEPTION 'saved order parameter mapping';
     END IF;
   END LOOP;
 END LOOP;
END $$;
REINDEX INDEX e_in;
REINDEX INDEX e_out;
DO $$ BEGIN
 IF jsonb_array_length(pg_temp.order_bag('EXECUTE graph_order(2)'))=0 THEN RAISE EXCEPTION 'reindex lost saved order'; END IF;
END $$;
DEALLOCATE graph_order;

-- A fixed middle key drives BOTH branches. Its propagated edge equality
-- must not be multiplied into cardinality a second time, even with payload
-- predicates on the edge. ANALYZE sees this entire <30k-row fixture.
CREATE TABLE cost_v(id bigint PRIMARY KEY);
CREATE TABLE cost_e(id bigint PRIMARY KEY,src bigint NOT NULL,dst bigint NOT NULL,payload integer);
INSERT INTO cost_v SELECT i FROM generate_series(1,15000) i;
INSERT INTO cost_e SELECT i,i,1000,1 FROM generate_series(1,200) i;
INSERT INTO cost_e VALUES(1000,1000,1001,1);
INSERT INTO cost_e SELECT i,i,i+1,1 FROM generate_series(2000,11999) i;
CREATE INDEX cost_e_out ON cost_e(src);
CREATE INDEX cost_e_in ON cost_e(dst);
CREATE PROPERTY GRAPH cost_g VERTEX TABLES(cost_v KEY(id)) EDGE TABLES(cost_e KEY(id)
 SOURCE KEY(src) REFERENCES cost_v(id) DESTINATION KEY(dst) REFERENCES cost_v(id));
ANALYZE cost_v;
ANALYZE cost_e;
DO $$
DECLARE q text; p jsonb; g jsonb; predicate text; pagecost text;
        cheap numeric; expensive numeric; oracle text;
BEGIN
 PERFORM set_config('graph_btree_max_candidates','8',true);
 FOREACH predicate IN ARRAY ARRAY['b.id=1000::bigint','b.id=1000::bigint AND y.payload=1'] LOOP
   q:='SELECT * FROM GRAPH_TABLE(cost_g MATCH (a IS cost_v)-[x IS cost_e]->(b IS cost_v)-[y IS cost_e]->(z IS cost_v) WHERE '||predicate||
      ' COLUMNS(a.id AS a,b.id AS b,z.id AS z,x.id AS x,y.id AS y)) gt';
   oracle:='SELECT a.id AS a,b.id AS b,z.id AS z,x.id AS x,y.id AS y FROM cost_v a JOIN cost_e x ON a.id=x.src JOIN cost_v b ON x.dst=b.id JOIN cost_e y ON b.id=y.src JOIN cost_v z ON y.dst=z.id WHERE '||predicate;
   PERFORM pg_temp.order_check(q,oracle);
   FOREACH pagecost IN ARRAY ARRAY['1','4'] LOOP
     PERFORM set_config('random_page_cost',pagecost,true);
     EXECUTE 'EXPLAIN(FORMAT JSON) /*+ GraphRoute(btree) */ '||q INTO p;
     WITH RECURSIVE plans(n) AS (SELECT p->0->'Plan' UNION ALL
       SELECT c FROM plans CROSS JOIN LATERAL jsonb_array_elements(n->'Plans') c)
     SELECT n INTO g FROM plans WHERE n->>'Custom Plan Provider'='GraphExpandFixed';
     IF g IS NULL OR (g->>'Graph Anchor Position')::int<>1 OR
        (g->>'Plan Rows')::int NOT BETWEEN 190 AND 210 THEN
       RAISE EXCEPTION 'middle key estimated more than once: %',p;
     END IF;
     IF pagecost='1' THEN cheap:=(g->>'Total Cost')::numeric;
     ELSE expensive:=(g->>'Total Cost')::numeric; END IF;
   END LOOP;
   IF expensive<cheap THEN RAISE EXCEPTION 'graph access ignores PG page costs'; END IF;
 END LOOP;
END $$;
RESET graph_btree_max_candidates;
RESET jit;
RESET search_path;
DROP SCHEMA ggc_orders CASCADE;
\echo graph start orders and bound keys: ok
