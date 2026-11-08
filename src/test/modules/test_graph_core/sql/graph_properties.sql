\set ECHO none
SET client_min_messages=warning;
LOAD 'test_graph_core';
SET jit=off;
CREATE SCHEMA ggc_properties;
SET search_path=ggc_properties,public;
CREATE TABLE v(id bigint PRIMARY KEY,name text,age integer,details jsonb,tags text[],born date);
CREATE TABLE e(id bigint PRIMARY KEY,src bigint NOT NULL,dst bigint NOT NULL,
 payload text,amount numeric,properties jsonb);
INSERT INTO v VALUES(1,'A',40,'{"city":"Rome"}',ARRAY['a','b'],'1980-01-01'),
 (2,'B',20,'{}',ARRAY['x',NULL],NULL),(3,'C',NULL,'{"city":"Paris"}',NULL,'2000-02-03'),
 (4,'D',17,NULL,'{}','2007-05-06');
INSERT INTO e VALUES(11,1,2,'a',10,'{"type":"pay"}'),(12,1,2,NULL,1,'{"type":"link"}'),
 (13,2,3,'b',100,'{"type":"pay"}'),(14,3,1,'c',-5,'{}'),
 (15,1,99,'missing',99,'{}'),(16,99,2,'missing',99,'{}'),
 (17,2,2,'self',NULL,NULL),(18,2,4,'d',2,'{}');
CREATE INDEX e_btree ON e(src) INCLUDE(dst,id);
CREATE PROPERTY GRAPH g VERTEX TABLES(v KEY(id)) EDGE TABLES(e KEY(id)
 SOURCE KEY(src) REFERENCES v(id) DESTINATION KEY(dst) REFERENCES v(id));
CREATE TABLE business(id bigint,label text);
INSERT INTO business VALUES(2,'two'),(3,'three-a'),(3,'three-b'),(NULL,'null');
ANALYZE v;
ANALYZE e;

CREATE FUNCTION pg_temp.property_bag(q text) RETURNS jsonb LANGUAGE plpgsql AS $$
DECLARE r record; bag jsonb='[]';
BEGIN
 FOR r IN EXECUTE q LOOP bag:=bag||jsonb_build_array(to_jsonb(r)); END LOOP;
 RETURN (SELECT coalesce(jsonb_agg(value ORDER BY value),'[]'::jsonb)
         FROM jsonb_array_elements(bag));
END $$;
CREATE FUNCTION pg_temp.property_check(q text,oracle text,props boolean=true,filters boolean=false)
 RETURNS void LANGUAGE plpgsql AS $$
DECLARE mode text; p jsonb; graphs int; scans int; quals int; sets int; actual jsonb; wanted jsonb;
BEGIN
 wanted:=pg_temp.property_bag(oracle);
 FOREACH mode IN ARRAY ARRAY['relational','btree'] LOOP
   EXECUTE 'EXPLAIN(ANALYZE,TIMING OFF,FORMAT JSON) /*+ GraphRoute('||mode||') */ '||q INTO p;
   WITH RECURSIVE plans(n) AS (
     SELECT p->0->'Plan' UNION ALL
     SELECT x FROM plans CROSS JOIN LATERAL jsonb_array_elements(n->'Plans') x
   ) SELECT count(*) FILTER(WHERE n->>'Custom Plan Provider'='GraphExpandFixed'),
            coalesce(sum((n->>'Graph Property Scans')::int),0),
            coalesce(sum((n->>'Graph Binding Filters')::int),0),
            count(*) FILTER(WHERE n->>'Graph Prefix Representation' LIKE '%endpoint sets%')
     INTO graphs,scans,quals,sets FROM plans;
   IF graphs<>(CASE WHEN mode='btree' THEN 1 ELSE 0 END) OR sets<>0 OR
      (mode='btree' AND props AND scans=0) OR
      (mode='btree' AND filters AND quals=0) THEN
     RAISE EXCEPTION 'property plan mismatch: %, %',q,p;
   END IF;
   actual:=pg_temp.property_bag('/*+ GraphRoute('||mode||') */ '||q);
   IF actual IS DISTINCT FROM wanted THEN
     RAISE EXCEPTION 'property bag mismatch: %, got %, wanted %',q,actual,wanted;
   END IF;
 END LOOP;
END $$;

DO $$
DECLARE cols text; pred text; graph text; native text; query text;
BEGIN
 FOREACH cols IN ARRAY ARRAY[
   'a.name AS origin,b.age AS middle_age,c.details AS destination,x.amount AS first_amount,y.properties AS last_properties',
   'a.name AS origin,a.id AS aid,b.tags AS tags,c.born AS born,x.id AS eid,y.amount AS amount',
   'a.age::numeric+x.amount AS total,coalesce(b.name,''?'')||c.name AS names,(y.properties->>''type'')::varchar(12) AS kind',
   'b.age::bigint AS middle_age,c.age::numeric AS end_age,x.amount::double precision AS amount',
   'c.id AS endpoint,x.id AS e1,y.id AS e2,x.amount AS amount,y.amount AS other'
 ] LOOP
   FOREACH pred IN ARRAY ARRAY[
     'true','y.amount>x.amount','a.age>b.age AND y.amount>=x.amount',
     'x.properties->>''type'' IS NOT DISTINCT FROM y.properties->>''type''',
     'a.name<>c.name AND (b.age IS NULL OR x.amount>b.age)',
     'x.amount+y.amount IS NULL OR c.age<a.age'
   ] LOOP
     graph:=format('GRAPH_TABLE(g MATCH (a IS v)-[x IS e]->(b IS v)-[y IS e]->(c IS v)
       WHERE %s COLUMNS(%s))',pred,cols);
     native:=format('(SELECT %s FROM v a JOIN e x ON x.src=a.id JOIN v b ON b.id=x.dst
       JOIN e y ON y.src=b.id JOIN v c ON c.id=y.dst WHERE %s)',cols,pred);
     PERFORM pg_temp.property_check('SELECT * FROM '||graph||' gt','SELECT * FROM '||native||' gt',true,pred<>'true');
   END LOOP;
 END LOOP;
 -- Only outputs keys, but the filter needs complete historical properties.
 graph:='GRAPH_TABLE(g MATCH (a IS v WHERE a.id=1::bigint)-[x IS e]->(b IS v)
   -[y IS e]->(c IS v) WHERE y.amount>x.amount COLUMNS(c.id AS endpoint))';
 native:='(SELECT c.id AS endpoint FROM v a JOIN e x ON x.src=a.id JOIN v b ON b.id=x.dst
   JOIN e y ON y.src=b.id JOIN v c ON c.id=y.dst WHERE a.id=1 AND y.amount>x.amount)';
 FOREACH query IN ARRAY ARRAY['SELECT * FROM @REGION@ gt','SELECT DISTINCT endpoint FROM @REGION@ gt',
   'SELECT count(*) FROM @REGION@ gt','SELECT t.*,gt.* FROM business t LEFT JOIN @REGION@ gt ON t.id=gt.endpoint'] LOOP
   PERFORM pg_temp.property_check(replace(query,'@REGION@',graph),replace(query,'@REGION@',native),true,true);
 END LOOP;
 -- Local filters and typed outputs may share columns without sharing borrowed slots.
 PERFORM pg_temp.property_check($q$SELECT * FROM GRAPH_TABLE(g MATCH (a IS v WHERE a.id=1::bigint)
   -[x IS e WHERE x.amount>0]->(b IS v WHERE b.age>=18)-[y IS e]->(c IS v)
   WHERE y.amount>x.amount COLUMNS(b.name AS name,b.age AS age,x.amount AS amount,y.amount AS other)) gt$q$,
   'SELECT b.name,b.age,x.amount,y.amount AS other FROM v a JOIN e x ON x.src=a.id JOIN v b ON b.id=x.dst
    JOIN e y ON y.src=b.id JOIN v c ON c.id=y.dst WHERE a.id=1 AND x.amount>0 AND b.age>=18 AND y.amount>x.amount',true,true);
END $$;

PREPARE property_params(numeric) AS SELECT /*+ GraphRoute(btree) */ * FROM GRAPH_TABLE(g MATCH
 (a IS v WHERE a.id=1::bigint)-[x IS e]->(b IS v)-[y IS e]->(c IS v)
 WHERE y.amount>x.amount+$1 COLUMNS(a.name AS name,c.id AS endpoint,y.amount AS amount)) gt;
DO $$
DECLARE mode text; threshold int; actual jsonb; wanted jsonb;
BEGIN
 FOREACH mode IN ARRAY ARRAY['force_generic_plan','force_custom_plan'] LOOP
   PERFORM set_config('plan_cache_mode',mode,true);
   FOREACH threshold IN ARRAY ARRAY[0,50,200,0] LOOP
     actual:=pg_temp.property_bag(format('EXECUTE property_params(%s)',threshold));
     wanted:=pg_temp.property_bag(format('SELECT a.name,c.id AS endpoint,y.amount FROM v a
       JOIN e x ON x.src=a.id JOIN v b ON b.id=x.dst JOIN e y ON y.src=b.id JOIN v c ON c.id=y.dst
       WHERE a.id=1 AND y.amount>x.amount+%s',threshold));
     IF actual IS DISTINCT FROM wanted THEN RAISE EXCEPTION 'property parameter mismatch'; END IF;
   END LOOP;
 END LOOP;
END $$;
REINDEX INDEX v_pkey;
REINDEX INDEX e_pkey;
DO $$ BEGIN
 IF pg_temp.property_bag('EXECUTE property_params(0)')<>
 '[{"name":"A","endpoint":3,"amount":100},{"name":"A","endpoint":3,"amount":100},{"name":"A","endpoint":4,"amount":2}]'::jsonb THEN
  -- Compare bags canonically; JSON array ordering is not query ordering.
  IF pg_temp.property_bag('EXECUTE property_params(0)') IS DISTINCT FROM
    pg_temp.property_bag('SELECT ''A''::text AS name,x.endpoint,x.amount FROM (VALUES (3,100),(3,100),(4,2)) x(endpoint,amount)') THEN
    RAISE EXCEPTION 'property reindex invalidation mismatch'; END IF;
 END IF;
END $$;
DEALLOCATE property_params;

-- Toasted attributes survive source-row unpinning, projection and sorting.
UPDATE v SET name=repeat('n',32768),details=jsonb_build_object('wide',repeat('z',32768)),tags=ARRAY[repeat('t',32768)] WHERE id=2;
UPDATE e SET properties=jsonb_build_object('wide',repeat('e',32768)) WHERE id=11;
DO $$ BEGIN
 PERFORM pg_temp.property_check($q$SELECT * FROM GRAPH_TABLE(g MATCH (a IS v WHERE a.id=1::bigint)
  -[x IS e]->(b IS v) COLUMNS(b.name AS name,b.details AS details,b.tags AS tags,x.properties AS props)) gt ORDER BY name$q$,
  'SELECT b.name,b.details,b.tags,x.properties AS props FROM v a JOIN e x ON x.src=a.id JOIN v b ON b.id=x.dst WHERE a.id=1 ORDER BY name');
END $$;

-- An exception after complete binding must unwind property scans and allow reuse.
DO $$
DECLARE failed boolean=false;
BEGIN
 BEGIN
   EXECUTE $q$SELECT /*+ GraphRoute(btree) */ * FROM GRAPH_TABLE(g MATCH (a IS v WHERE a.id=1::bigint)
    -[x IS e]->(b IS v)-[y IS e]->(c IS v)
    WHERE 1/(x.amount+y.amount-110)>0 COLUMNS(c.name AS name)) gt$q$;
 EXCEPTION WHEN division_by_zero THEN failed:=true; END;
 IF NOT failed THEN RAISE EXCEPTION 'binding filter lost division by zero'; END IF;
 PERFORM pg_temp.property_check($q$SELECT * FROM GRAPH_TABLE(g MATCH (a IS v WHERE a.id=1::bigint)
  -[x IS e]->(b IS v) COLUMNS(b.age AS age)) gt$q$,
  'SELECT b.age FROM v a JOIN e x ON x.src=a.id JOIN v b ON b.id=x.dst WHERE a.id=1');
END $$;
SET search_path=public;
DROP SCHEMA ggc_properties CASCADE;
