#!/usr/bin/env python3
"""Joint correctness/resource gate, with measured development maintenance report.

Does not certify GS-090, a production SLO, or final-layout DML non-regression.
"""
import json
from pathlib import Path
import sys

import psycopg2

PACKAGE = Path(__file__).resolve().parents[1]
WORKSPACE = PACKAGE.parents[2]
INSTALL = WORKSPACE / '.install/pg19-graph-core/candidate'
MODULE = WORKSPACE / '.worktrees/pg19-graph-core/postgres19/src/test/modules/test_graph_core'
DSN = dict(host=str(WORKSPACE / '.runtime/pg19-graph-core/candidate/socket'),
           port=55450, user='graph_lab', dbname='graph_lab')


def conn():
    connection = psycopg2.connect(**DSN)
    connection.autocommit = True
    return connection


def scope(graph, edge, role, index=0, kind=1, build=0, fingerprint=7501):
    return json.dumps(dict(catalog_provider_oid=0, graph_oid=graph, edge_element_oid=edge,
        index_relid=index, effective_role=role, endpoint_domain=75, scope_kind=kind,
        direction=1, path_mode=1, result_mode=1, catalog_generation=7,
        security_generation=11, build_generation=build, semantic_fingerprint=fingerprint))


def opts(**extra):
    options = dict(mode='full', seed=7501, vertex_sample_target=8, strata=3,
        heavy_hitter_limit=8, max_scan_pages=1000, max_examined_entries=100000,
        max_bindings=1000, max_entries_per_seed=1000, max_memory_bytes=67108864,
        max_payload_bytes=1048576, max_profiles=64, deadline_ms=10000,
        lock_timeout_ms=1000, throttle_delay_ms=0)
    options.update(extra)
    return json.dumps(options)


def verify():
    sys.path.insert(0, str(PACKAGE / 'scripts'))
    from common import run_dir, run_logged, sha, write
    from lab import db_env

    out = run_dir('check-G1-075-audit')
    commands = [run_logged(['make', '-s', '-C', MODULE, 'USE_PGXS=1',
        f'PG_CONFIG={INSTALL}/bin/pg_config', 'installcheck'], out, 'native-regression',
        env=db_env('candidate'), timeout=300)]
    artifacts = []
    by_probe = {}
    for name in ('joint_probe', 'scope_probe', 'adj_resource_probe', 'maintenance_probe'):
        commands.append(run_logged([sys.executable, PACKAGE / f'golden/G1-075/{name}.py'],
            out, name, timeout=120))
        record = json.loads((out / f'{name}.log').read_text().splitlines()[-1])
        path = Path(record['evidence'])
        if commands[-1]['exit_code'] != 0 or record.get('status') != 'PASS':
            raise RuntimeError(f'{name} did not pass its partial probe: {record}')
        document = json.loads(path.read_text())
        if document.get('status') != 'PASS':
            raise RuntimeError(f'{name} evidence is not PASS')
        by_probe[name] = dict(path=str(path.relative_to(PACKAGE)), sha256=sha(path))
        artifacts.append(by_probe[name])
    if any(command['exit_code'] != 0 for command in commands):
        raise RuntimeError('a required command failed')
    manifest = dict(postgres_sha256=sha(INSTALL/'bin/postgres'),
        module_sha256=sha(INSTALL/'lib/postgresql/test_graph_core.so'),
        probes={name:sha(PACKAGE/f'golden/G1-075/{name}.py') for name in by_probe})
    write(out/'build-and-probes.json',manifest)
    artifacts.append(dict(path=str((out/'build-and-probes.json').relative_to(PACKAGE)),
                          sha256=sha(out/'build-and-probes.json')))
    record = dict(task_id='G1-075', status='PASS', assertions_passed=[
        'concurrent_stats_ddl_dml_publish', 'counter_reset_not_fresh',
        'rewrite_reindex_physical_scope', 'role_rls_feedback_isolation',
        'no_statistics_mid_execution_replan', 'budget_cancel_snapshot_pin_cleanup',
        'stats_maintenance_dml_overhead_report'],
        selected_feature_plan_evidence=by_probe['adj_resource_probe'],
        maintenance_execution_evidence=by_probe['maintenance_probe'],
        statistics_evidence=by_probe['scope_probe'],
        evidence_files=artifacts, commands=commands,
        verified=['real collector cancel with concurrent DML and waiting DDL',
                  'snapshot-correct publication during DML',
                  'real GraphExpandFixed subtransaction cancellation',
                  'publication during graph cursor execution',
                  'collector scan-budget and cancel shared buffer pins released',
                  'REINDEX and heap rewrite invalidate physical but preserve logical profiles',
                  'role/RLS raw access denied; feedback role/security/GUC isolation',
                  'captured pgstat baseline; ANALYZE/reset/missing counters lower planner confidence',
                  'expired/future/unconfigured statistics age cannot establish freshness',
                  'graph_adj direct-ref plan, expansion budget error and active cursor cancellation cleanup',
                  'measured ABBA maintenance interference, diagnostic only'],
        limitations=['not a production maintenance or GS non-regression verdict',
                     'AM internal critical-section failure injection remains in storage gates',
                     'age budget is request-local; production callers default to unknown',
                     'no automatic refresh queue or calibrated change-risk pricing',
                     'eight-endpoint graph_adj cleanup fixture does not validate storage scalability'])
    write(out / 'summary.json', record)
    print(json.dumps(record, ensure_ascii=False))
    return 0


if __name__ == '__main__':
    raise SystemExit(verify())
