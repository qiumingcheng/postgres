#!/usr/bin/env python3
"""Rebuild and verify PGQ semantics against native SQL and an independent oracle."""
import json
from pathlib import Path
import shutil
import sys

SCRIPTS = Path(__file__).resolve().parents[1] / 'scripts'
sys.path.insert(0, str(SCRIPTS))
from common import PACKAGE, WORKSPACE, now, run_dir, run_logged, sha, write
from lab import db_env

SOURCE = WORKSPACE / '.worktrees/pg19-graph-core/postgres19'
INSTALL = WORKSPACE / '.install/pg19-graph-core/candidate'
MODULE = SOURCE / 'src/test/modules/test_graph_core'
PROBE = PACKAGE / 'golden/G1-070/verify_pgq.py'

def evidence(path):
    return dict(path=str(path.relative_to(PACKAGE)),sha256=sha(path))

def main():
    out = run_dir('check-G1-070')
    lab = PACKAGE / 'scripts/lab.py'
    commands = []
    started = False
    try:
        commands.append(run_logged([sys.executable,lab,'stop','--variant=candidate'],out,'pre-stop',timeout=120))
        commands.append(run_logged([sys.executable,lab,'build','--variant=candidate','--jobs=4'],out,'build',timeout=1800))
        make = ['make','-C',MODULE,'USE_PGXS=1',f'PG_CONFIG={INSTALL / "bin/pg_config"}']
        for name,args in [('clean',['clean']),('build',['-j4']),('install',['install'])]:
            commands.append(run_logged(make+args,out,'module-'+name,timeout=300))
        commands.append(run_logged([sys.executable,lab,'start','--variant=candidate'],out,'start',timeout=180))
        started = True
        commands.append(run_logged(make+['installcheck'],out,'native-regression',env=db_env('candidate'),timeout=300))
        native = out / 'graph_pgq.out'
        shutil.copyfile(MODULE / 'results/graph_pgq.out',native)
        commands.append(run_logged([sys.executable,PROBE],out,'pgq-oracle',timeout=300))
        # run_logged retains stdout in a JSON command record and in its log;
        # parse the log from the first result object to avoid relying on banners.
        log = (out / 'pgq-oracle.log').read_text()
        start = log.find('{\n')
        if start < 0:
            raise RuntimeError('missing live PGQ probe evidence')
        result, _ = json.JSONDecoder().raw_decode(log[start:])
        required = {'bag_null_orphan_2','bag_null_orphan_3','bag_null_orphan_4',
                    'vertex_snapshot_delete','column_only_fallback','column_permission_denied',
                    'v_rls_fallback','e_rls_fallback','rls_statistics_loader',
                    'projected_bag_duplicates','bigint_payload_2','bigint_payload_3','bigint_payload_4'}
        if result['status'] != 'PASS' or {r['case'] for r in result['results']} != required:
            raise RuntimeError('incomplete PGQ semantic matrix')
        results = out / 'results.json'
        write(results,dict(probe=result,postgres_sha256=sha(INSTALL/'bin/postgres'),
                           module_sha256=sha(INSTALL/'lib/postgresql/test_graph_core.so'),
                           source_sha256={str(p.relative_to(SOURCE)):sha(p) for p in (
                               SOURCE/'src/backend/executor/graph_fixed.c',
                               SOURCE/'src/include/executor/graph_fixed.h',
                               SOURCE/'src/backend/optimizer/path/graphstats.c',
                               MODULE/'test_graph_fixed.c',MODULE/'test_graph_cost.c')},
                           finished=now(),limitations=[
                               'PGQ planner entry is a test-module shim; selected execution uses the core runtime.',
                               'Join-method switches prove feature execution, not natural CBO superiority.',
                               'RLS and column-only edge permissions retain the relational plan.',
                               'Per-binding vertex index checks require later batching/cost calibration.']))
        commands.append(run_logged([sys.executable,lab,'stop','--variant=candidate'],out,'post-stop',timeout=120))
        started = False
        write(out/'summary.json',dict(status='PASS',task_id='G1-070',commands=commands,finished=now()))
        print(json.dumps(dict(task_id='G1-070',status='PASS',assertions_passed=[
            'edge_vertex_rls','column_permissions','bag_types_null','independent_oracle_2_4',
            'stats_global_payload_not_rls_population'],selected_feature_plan_evidence=evidence(results),
            evidence_files=[evidence(p) for p in (results,native,PROBE,out/'native-regression.log',out/'pgq-oracle.log')]),ensure_ascii=False))
    finally:
        if started:
            run_logged([sys.executable,lab,'stop','--variant=candidate'],out,'post-stop',timeout=120)

if __name__ == '__main__':
    main()
