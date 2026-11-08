#!/usr/bin/env python3
"""Fail-closed checks. Missing feature tests cannot turn into a passing gate."""
from __future__ import annotations
import argparse
import json
import subprocess
import sys
from common import PACKAGE, now, read, sha
from lab import paths, verify_sources

def receipt(label):
    matches=sorted((PACKAGE/'evidence/runs').glob('*-'+label))
    if not matches: raise RuntimeError('NOT_RUN: no receipt for '+label)
    path=matches[-1]/'summary.json'
    if not path.is_file():raise RuntimeError('latest run incomplete or failed: '+str(matches[-1]))
    result=read(path)
    if result.get('status')!='PASS': raise RuntimeError('receipt not PASS: '+str(path))
    for command in result.get('commands',[]) + ([result['command']] if 'command' in result else []):
        if command['exit_code']!=0 or sha(PACKAGE/command['log'])!=command['sha256']:
            raise RuntimeError('receipt command invalid')
    if 'results_sha256' in result and sha(path.parent/'results.json')!=result['results_sha256']:
        raise RuntimeError('result digest mismatch')
    for run in result.get('rounds',[]):
        command=run['command']
        if command['exit_code']!=0 or sha(PACKAGE/command['log'])!=command['sha256']:
            raise RuntimeError('benchmark command evidence invalid')
        for item in run['raw_logs']:
            if sha(PACKAGE/item['path'])!=item['sha256']:raise RuntimeError('benchmark raw data digest mismatch')
    print(path)
    return result

def check(name):
    if name=='baseline-build':
        verify_sources();r=receipt('build-baseline');p=paths('baseline')
        if sha(p['install']/'bin/postgres')!=r['postgres_sha256'] or sha(p['install']/'lib/postgresql/age.so')!=r['age_sha256']:
            raise RuntimeError('binary changed since baseline build')
    elif name=='baseline-regression-audit':
        receipt('regress-baseline-pg')
        audit=read(PACKAGE/'baseline/known_issues.json')
        for f in audit['evidence_files']:
            if sha(PACKAGE/f['path'])!=f['sha256']: raise RuntimeError('AGE regression evidence changed')
        if not audit['failed_tests'] or audit['status']!='OPEN_BASELINE_DIFFERENCES': raise RuntimeError('baseline difference ledger missing')
        print('PASS: characterization complete; AGE regression remains FAIL, not waived')
    elif name=='baseline-oracle': receipt('baseline-differential-baseline')
    elif name=='baseline-benchmark': receipt('development-benchmark-baseline')
    elif name=='business-input':
        all_spec=read(PACKAGE/'specs/acceptance.json')
        spec=all_spec['product']
        missing=[k for k,v in spec.items() if v is None or (isinstance(v,(str,list,dict)) and not v)]
        if missing or spec['status']!='FROZEN': raise RuntimeError('NEEDS_INPUT: '+','.join(missing))
        if all_spec.get('version',1)>=2:
            stats=all_spec.get('statistics_preregistration',{})
            pending=[k for k,v in stats.items() if v is None or (isinstance(v,(str,list,dict)) and not v)]
            if pending or stats.get('status')!='FROZEN':
                raise RuntimeError('NEEDS_STATISTICS_PREREGISTRATION: '+','.join(pending))
        print('Product inputs present; formal measurements still required.')
    else: raise RuntimeError('unknown check')

def task_check(task_id):
    defs={t['id']:t for t in read(PACKAGE/'specs/tasks.json')['tasks']}
    if task_id not in defs: raise RuntimeError('unknown task')
    script=PACKAGE/'checks'/f'{task_id}.py'
    if not script.is_file(): raise RuntimeError('NOT_IMPLEMENTED: '+str(script)+'; implement the task-specific contract test first')
    result=subprocess.run([sys.executable,script],capture_output=True,text=True,timeout=1800)
    print(result.stdout,end='');print(result.stderr,end='',file=sys.stderr)
    if result.returncode: raise RuntimeError('task-specific verification failed')
    try: record=json.loads(result.stdout.strip().splitlines()[-1])
    except (ValueError,IndexError): raise RuntimeError('last line must be a structured verification record')
    if record.get('task_id')!=task_id or record.get('status')!='PASS': raise RuntimeError('wrong task/status')
    required=set(defs[task_id]['assertions'])
    if set(record.get('assertions_passed',[]))!=required: raise RuntimeError('missing/extra contract assertions')
    if not record.get('evidence_files'): raise RuntimeError('actual evidence files required')
    for e in record['evidence_files']:
        path=(PACKAGE/e['path']).resolve()
        if not path.is_relative_to(PACKAGE) or sha(path)!=e['sha256']: raise RuntimeError('evidence outside package or digest mismatch')
    for flag, field in [('requires_maintenance_execution','maintenance_execution_evidence'),
                        ('requires_statistics_evidence','statistics_evidence')]:
        if defs[task_id].get(flag):
            item=record.get(field)
            if not isinstance(item,dict) or 'path' not in item or 'sha256' not in item:
                raise RuntimeError('missing actual '+field+'; reference fields/oracle-only checks do not qualify')
            path=(PACKAGE/item['path']).resolve()
            if not path.is_relative_to((PACKAGE/'evidence').resolve()) or not path.is_file() or sha(path)!=item['sha256']:
                raise RuntimeError('missing or changed '+field)
    if defs[task_id].get('requires_feature_execution'):
        plan=record.get('selected_feature_plan_evidence')
        if not isinstance(plan,dict) or 'path' not in plan or 'sha256' not in plan:
            raise RuntimeError('fallback-only execution cannot pass this feature task')
        planpath=(PACKAGE/plan['path']).resolve()
        if not planpath.is_relative_to(PACKAGE/'evidence') or not planpath.is_file() or sha(planpath)!=plan['sha256']:
            raise RuntimeError('missing or changed actual feature plan evidence')

if __name__=='__main__':
    a=argparse.ArgumentParser(description=__doc__);a.add_argument('command',choices=['check','task']);a.add_argument('name');x=a.parse_args()
    try:
        if x.command=='check': check(x.name)
        else: task_check(x.name)
    except (RuntimeError,OSError,KeyError,ValueError) as e:
        print(str(e),file=sys.stderr);sys.exit(3)
