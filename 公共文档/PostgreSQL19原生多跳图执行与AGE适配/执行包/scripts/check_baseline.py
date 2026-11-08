#!/usr/bin/env python3
"""Differential PGQ/SQL and AGE/SQL checks against exhaustive path histories."""
from __future__ import annotations
import argparse
import json
import random
from common import PACKAGE, now, read, run_dir, sha, write
from lab import cluster, paths, sql
from oracle import bag, solve, selfcheck

def install_graph(variant, name, graph):
    schema = 'glab_' + name
    age_schema = 'age_glab_' + name
    rows = ','.join(f'({v},NULL)' for v in graph['vertices'])
    edges = ','.join(f'({eid},{src},{dst})' for eid, src, dst in graph['edges'])
    setup = f"""
    DROP SCHEMA IF EXISTS {schema} CASCADE;
    CREATE SCHEMA {schema}; SET search_path = {schema},public;
    CREATE TABLE vertex (id bigint PRIMARY KEY, payload text);
    CREATE TABLE edge (id bigint PRIMARY KEY, src bigint NOT NULL REFERENCES vertex(id), dst bigint NOT NULL REFERENCES vertex(id));
    CREATE INDEX edge_out ON edge(src); CREATE INDEX edge_in ON edge(dst);
    INSERT INTO vertex VALUES {rows};
    """ + (f'INSERT INTO edge VALUES {edges};' if edges else '') + f"""
    CREATE PROPERTY GRAPH pg_graph VERTEX TABLES(vertex KEY(id))
      EDGE TABLES(edge KEY(id) SOURCE KEY(src) REFERENCES vertex(id) DESTINATION KEY(dst) REFERENCES vertex(id));
    CREATE EXTENSION IF NOT EXISTS age;
    LOAD 'age'; SET search_path = ag_catalog,public;
    """
    # All identifiers originate from the shipped fixture or our deterministic generator.
    sql(variant, setup)
    exists = sql(variant, f"SELECT count(*) FROM ag_catalog.ag_graph WHERE name = '{age_schema}';").strip()
    if exists != '0':
        sql(variant, f"LOAD 'age'; SET search_path=ag_catalog,public; SELECT drop_graph('{age_schema}',true);")
    vertices = ','.join(f'(v{v}:vertex {{key:{v}}})' for v in graph['vertices'])
    relations = ','.join(f'(v{src})-[:edge {{key:{eid}}}]->(v{dst})' for eid, src, dst in graph['edges'])
    creation = vertices + (',' + relations if relations else '')
    sql(variant, f"LOAD 'age'; SET search_path=ag_catalog,public; SELECT create_graph('{age_schema}'); SELECT * FROM cypher('{age_schema}', $$ CREATE {creation} $$) AS (v agtype);")

def fixed_sql(name, case, frontend):
    n = case['upper']
    assert n == case['lower'] and 1 <= n <= 4
    d = case.get('direction', 'OUT')
    assert d in ('OUT', 'IN')
    source = case['source']
    schema = 'glab_' + name
    edgecols = [f'e{i}' for i in range(1, n + 1)]
    columns = ['s', *edgecols, 't']
    if frontend == 'pgq':
        pattern = '(v0 IS vertex WHERE v0.id = ' + str(source) + ')'
        for i in range(1, n + 1):
            pattern += (f'-[r{i} IS edge]->' if d == 'OUT' else f'<-[r{i} IS edge]-') + f'(v{i} IS vertex)'
        outputs = ['v0.id AS s', *[f'r{i}.id AS e{i}' for i in range(1, n + 1)], f'v{n}.id AS t']
        inner = f'GRAPH_TABLE ({schema}.pg_graph MATCH {pattern} COLUMNS ({", ".join(outputs)}))'
        prefix = f'SET search_path={schema},public;'
        cast = lambda c: f'g.{c}'
    elif frontend == 'age':
        pattern = f'(v0:vertex {{key:{source}}})'
        for i in range(1, n + 1):
            pattern += (f'-[r{i}:edge]->' if d == 'OUT' else f'<-[r{i}:edge]-') + f'(v{i}:vertex)'
        outputs = ['v0.key', *[f'r{i}.key' for i in range(1, n + 1)], f'v{n}.key']
        inner = f"cypher('age_{schema}', $$ MATCH {pattern} RETURN {','.join(outputs)} $$)"
        prefix = "LOAD 'age'; SET search_path=ag_catalog,public;"
        cast = lambda c: f'(g.{c}::text)::bigint'
    else:
        tables = [f'{schema}.vertex v0']
        for i in range(1, n + 1):
            src, dst = ('src','dst') if d == 'OUT' else ('dst','src')
            tables.append(f'JOIN {schema}.edge r{i} ON r{i}.{src}=v{i-1}.id JOIN {schema}.vertex v{i} ON v{i}.id=r{i}.{dst}')
        cond = [f'v0.id={source}']
        if case['mode'] == 'TRAIL':
            cond += [f'r{i}.id<>r{j}.id' for i in range(1,n+1) for j in range(i+1,n+1)]
        outputs = ['v0.id AS s', *[f'r{i}.id AS e{i}' for i in range(1,n+1)], f'v{n}.id AS t']
        inner = f'(SELECT {",".join(outputs)} FROM {" ".join(tables)} WHERE {" AND ".join(cond)})'
        prefix = ''
        cast = lambda c: f'g.{c}'
    alias = 'g' + ('(' + ','.join(c + ' agtype' for c in columns) + ')' if frontend == 'age' else '')
    arr = 'ARRAY[' + ','.join(cast(c) for c in edgecols) + ']'
    target = f" WHERE {cast('t')}={case['target']}" if case.get('target') is not None else ''
    return prefix + f"SELECT coalesce(json_agg(json_build_array({cast('s')},{arr},{cast('t')})), '[]'::json) FROM {inner} {alias}{target};"

def check(variant):
    selfcheck()
    cluster(variant, 'start')
    out = run_dir('baseline-differential-' + variant)
    data = read(PACKAGE / 'golden/small_graphs.json')
    rng = random.Random(20260906)
    data['graphs']['random'] = {'vertices': list(range(1,7)), 'edges': [[100+i,rng.randint(1,6),rng.randint(1,6)] for i in range(12)]}
    tests = []
    for name, graph in data['graphs'].items():
        install_graph(variant, name, graph)
        for source in (graph['vertices'][0],graph['vertices'][-1]):
            for direction in ('OUT','IN'):
                for hops in (1,2,3,4):
                    for frontend, mode in (('pgq','WALK'),('age','TRAIL')):
                        case = {'source': source, 'lower': hops, 'upper': hops, 'direction': direction, 'mode': mode, 'result': 'ALL_BINDINGS'}
                        query = fixed_sql(name, case, frontend)
                        reference_query = fixed_sql(name, case, 'sql')
                        expected = solve(graph, case)
                        actual = json.loads(sql(variant, query))
                        reference = json.loads(sql(variant, reference_query))
                        ok = bag(actual) == bag(expected) == bag(reference)
                        record = {'graph': name, 'frontend': frontend, 'case': case, 'rows':len(actual), 'status':'PASS' if ok else 'FAIL', 'query':query, 'reference_sql':reference_query, 'actual':actual, 'oracle':expected}
                        tests.append(record)
                        if not ok:
                            write(out / 'failure.json', record)
                            raise AssertionError(f'differential mismatch: {name}/{frontend}/{case}; {out}')
    # Nullable projected properties: value and SQL type are part of the smoke contract.
    nullable = sql(variant, "SET search_path=glab_diamond,public; SELECT json_build_array(payload,pg_typeof(payload)::text) FROM GRAPH_TABLE(pg_graph MATCH (v IS vertex WHERE v.id=1) COLUMNS(v.payload));").strip()
    if json.loads(nullable) != [None,'text']:
        raise AssertionError('NULL/type projection mismatch')
    manifest = read(paths(variant)['build'] / 'build_manifest.json')
    write(out / 'results.json', tests)
    result = {'status':'PASS','scope':'fixed 1..4 hop baseline differential; OUT/IN; PGQ WALK and AGE TRAIL separately',
              'variant':variant, 'count':len(tests), 'oracle_hand_cases':len(data['cases']), 'nullable_projection':'PASS',
              'finished':now(), 'build_manifest':manifest, 'results_sha256':sha(out/'results.json'),
              'product_gate':'NOT_RUN', 'new_graph_node_executed':False}
    write(out / 'summary.json', result)
    print(f'PASS: {len(tests)} SQL/PGQ/AGE differential cases; {out}')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--variant', choices=['baseline','candidate'], default='baseline')
    check(parser.parse_args().variant)
