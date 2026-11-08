#!/usr/bin/env python3
"""A runnable development baseline. Short runs never qualify for a product gate."""
from __future__ import annotations
import argparse
import csv
import json
import math
import random
from common import PACKAGE, now, read, run_dir, run_logged, sha, write
from lab import cluster, db_env, paths, sql

def quantile(values, q):
    s = sorted(values)
    pos = (len(s) - 1) * q
    a = int(pos)
    return s[a] + (s[min(a+1,len(s)-1)] - s[a]) * (pos-a)

def setup(variant, directory, cfg):
    rng = random.Random(cfg['seed'])
    path = directory / 'edges.csv'
    with path.open('w') as out:
        writer = csv.writer(out)
        for i in range(1, cfg['edges'] + 1):
            writer.writerow((i, rng.randint(1,cfg['vertices']),rng.randint(1,cfg['vertices'])))
    sql(variant, f"""
    DROP SCHEMA IF EXISTS glab_bench CASCADE; CREATE SCHEMA glab_bench;
    CREATE TABLE glab_bench.vertex(id bigint PRIMARY KEY);
    INSERT INTO glab_bench.vertex SELECT generate_series(1,{cfg['vertices']});
    CREATE TABLE glab_bench.edge(id bigint PRIMARY KEY,src bigint NOT NULL REFERENCES glab_bench.vertex(id),dst bigint NOT NULL REFERENCES glab_bench.vertex(id));
    CREATE INDEX edge_out ON glab_bench.edge(src); CREATE INDEX edge_in ON glab_bench.edge(dst);
    COPY glab_bench.edge FROM '{path}' WITH (FORMAT csv);
    CREATE SEQUENCE glab_bench.edge_seq START WITH {cfg['edges']+1};
    ANALYZE glab_bench.vertex; ANALYZE glab_bench.edge;
    """)
    return sha(path)

def benchmark(variant, seconds, rounds):
    cluster(variant, 'start')
    cfg = read(PACKAGE / 'specs/acceptance.json')['development'].copy()
    if seconds is not None: cfg['timed_seconds'] = seconds
    if rounds is not None: cfg['repetitions'] = rounds
    out = run_dir('development-benchmark-' + variant)
    p = paths(variant)
    run_records = []
    seed_hash = None
    for round_id in range(cfg['repetitions']):
        for workload in ('read','insert'):
            for clients in cfg['clients']:
                unit = out / f'{round_id}-{workload}-{clients}'
                unit.mkdir()
                seed_hash = setup(variant,unit,cfg)
                # Full tuple consumption for the read case, not COUNT(*) pushdown.
                script = unit / 'workload.sql'
                script.write_text('\\set s random(1, ' + str(cfg['vertices']) + ')\n' + (
                    'SELECT e1.id,e2.id,e2.dst FROM glab_bench.edge e1 JOIN glab_bench.edge e2 ON e2.src=e1.dst WHERE e1.src=:s;\n'
                    if workload == 'read' else
                    '\\set d random(1, ' + str(cfg['vertices']) + ")\nINSERT INTO glab_bench.edge VALUES(nextval('glab_bench.edge_seq'),:s,:d);\n"))
                query_plan = sql(variant, "EXPLAIN (FORMAT JSON) SELECT e1.id,e2.id,e2.dst FROM glab_bench.edge e1 JOIN glab_bench.edge e2 ON e2.src=e1.dst WHERE e1.src=1;")
                (unit / 'plan.json').write_text(query_plan)
                settings = sql(variant, "SELECT json_object_agg(name,setting) FROM pg_settings WHERE name IN ('fsync','synchronous_commit','full_page_writes','shared_buffers','work_mem','checkpoint_timeout','autovacuum');")
                indexes = sql(variant, "SELECT json_agg(row_to_json(x)) FROM (SELECT indexrelid::regclass::text AS name,indisvalid,indisready,pg_get_indexdef(indexrelid) AS ddl FROM pg_index WHERE indrelid='glab_bench.edge'::regclass) x;")
                rec = run_logged([p['install']/ 'bin/pgbench', '-n', '-M', 'prepared', '-c', str(clients), '-j', str(min(clients,4)), '-T', str(cfg['timed_seconds']), '--random-seed='+str(cfg['seed']+round_id), '-l', '--log-prefix='+str(unit/'transactions'), '-f', script, 'graph_lab'], unit, 'pgbench', env=db_env(variant))
                latencies=[]
                for log in sorted(unit.glob('transactions.*')):
                    for line in log.read_text().splitlines():
                        fields=line.split()
                        if len(fields)<6 or not fields[2].isdigit():
                            raise RuntimeError('unexpected/failed transaction log; inspect raw records')
                        latencies.append(int(fields[2])/1000.0)
                if not latencies:
                    raise RuntimeError('no transaction samples')
                textlog=(unit/'pgbench.log').read_text()
                if 'number of failed transactions: 0 ' not in textlog:
                    raise RuntimeError('nonzero or unknown failed transaction count')
                run_records.append({'round':round_id,'workload':workload,'clients':clients,
                    'successes':len(latencies),'p95_ms':quantile(latencies,.95),'p99_ms':quantile(latencies,.99),
                    'transactions_per_nominal_second':len(latencies)/cfg['timed_seconds'],
                    'measured_mode':'closed_loop_capacity; not fixed offered load',
                    'settings':json.loads(settings),'indexes':json.loads(indexes),
                    'dataset_sha256':seed_hash,'command':rec,
                    'raw_logs':[{'path':str(l.relative_to(PACKAGE)),'sha256':sha(l)} for l in sorted(unit.glob('transactions.*'))]})
    summary={'status':'PASS','scope':'development harness baseline only','variant':variant,
             'config':cfg,'finished':now(),'rounds':run_records,
             'build_manifest':read(p['build']/'build_manifest.json'),
             'performance_acceptance':'NOT_RUN','product_gate':'NOT_RUN',
             'limitations':['short windows; no five-checkpoint or maintenance-cycle coverage',
                            'ordinary B-tree layout only; no graph_adj candidate',
                            'only existing-endpoint autocommit writes and fixed two-hop relational reads',
                            'closed-loop P99 is not the fixed-offered-load DML acceptance metric']}
    write(out/'summary.json',summary)
    print(f'PASS: development harness; product gate NOT_RUN; {out}')

if __name__=='__main__':
    a=argparse.ArgumentParser(description=__doc__)
    a.add_argument('--variant',choices=['baseline','candidate'],default='baseline')
    a.add_argument('--seconds',type=int)
    a.add_argument('--rounds',type=int)
    x=a.parse_args()
    if (x.seconds is not None and x.seconds<1) or (x.rounds is not None and x.rounds<1): a.error('positive values required')
    benchmark(x.variant,x.seconds,x.rounds)
