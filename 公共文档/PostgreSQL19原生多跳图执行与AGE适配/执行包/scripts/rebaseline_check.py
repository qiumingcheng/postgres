#!/usr/bin/env python3
"""Audit the V2 handoff offline; never start a database or complete a task."""
from collections import Counter
import sys
from common import PACKAGE, DESIGN, read, run_dir, run_logged, sha, write, now


def main():
    out=run_dir('rebaseline-v2')
    commands=[]
    for label, argv in [
        ('package-validation', ['validate_package.py']),
        ('offline-selfcheck', ['selfcheck.py']),
        ('source-preflight', ['lab.py','preflight']),
        ('ready-tasks', ['taskctl.py','list-ready']),
    ]:
        commands.append(run_logged([sys.executable,PACKAGE/'scripts'/argv[0],*argv[1:]],out,label,timeout=1800))
    manifest=read(PACKAGE/'specs/design_baseline_v2.json')
    ledger=read(PACKAGE/'state/task_state.json')
    files=manifest['active_reference_headers']+manifest['active_specs']+[
        'specs/design_baseline_v2.json','specs/tasks.json','specs/acceptance.json',
        'state/task_state.json','MODEL_HANDOFF_PROMPT.md','README.md','实施契约.md','任务导航.md',
        'scripts/taskctl.py','scripts/gate.py','scripts/validate_package.py','scripts/selfcheck.py',
        'scripts/rebaseline_check.py']
    write(out/'summary.json',{
        'status':'PASS','scope':'V2 execution package offline checks only','finished':now(),
        'commands':commands,'design_sha256':sha(DESIGN),'files':{f:sha(PACKAGE/f) for f in files},
        'task_counts':dict(Counter(t['status'] for t in ledger['tasks'].values())),
        'active_task':ledger['active_task'],'product_gate':'NOT_RUN',
        'new_database_tests':'NOT_RUN','database_lifecycle_actions':[],
        'historical_inventory':'evidence/final_inventory.json (historical snapshot, not overwritten)'})
    print(out/'summary.json')


if __name__=='__main__':
    main()
