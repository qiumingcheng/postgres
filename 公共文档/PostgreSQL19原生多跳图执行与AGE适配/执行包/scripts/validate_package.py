#!/usr/bin/env python3
"""Validate task DAG, path references, locks and state/evidence consistency."""
import argparse
import json
from pathlib import Path
import re
import sys
from urllib.parse import unquote
from common import PACKAGE, WORKSPACE, read, sha
from lab import verify_sources
from taskctl import digest, evidence_valid

def validate_design_baseline(defs, manifest, package=PACKAGE):
    design=(package/manifest['main_design']).resolve()
    if sha(design)!=manifest['design_sha256']:
        raise ValueError('main design changed; review/rebaseline contracts and tasks before updating its digest')
    if manifest['version']!=2:
        raise ValueError('active execution baseline must be V2')
    for f in manifest['active_reference_headers']+manifest['active_specs']:
        if not (package/f).is_file():raise ValueError('missing active V2 contract '+f)
    legacy=manifest['legacy_reference_sha256']
    for f, expected in legacy.items():
        if sha(package/f)!=expected:raise ValueError('historical V1 reference changed: '+f)
    preserved=set(manifest['legacy_completed_definitions_preserved'])
    for key,t in defs.items():
        if key not in preserved:
            refs=t['reading_route']['package_refs']+t['allowed_paths']
            if any(any(ref.endswith(old) for old in legacy) for ref in refs):
                raise ValueError('active task uses retired V1 contract: '+key)
    def ancestors(key):
        seen=set(); pending=list(defs[key]['depends_on'])
        while pending:
            dep=pending.pop()
            if dep not in seen:
                seen.add(dep);pending.extend(defs[dep]['depends_on'])
        return seen
    for key,required in manifest['required_task_assertions'].items():
        if not set(required).issubset(defs[key]['assertions']):
            raise ValueError('missing V2 assertions: '+key)
    for key,required in manifest['required_ancestors'].items():
        if not set(required).issubset(ancestors(key)):
            raise ValueError('missing V2 prerequisite: '+key)

def validate(check_state=True):
    verify_sources()
    task_spec=read(PACKAGE/'specs/tasks.json')
    tasks=task_spec['tasks']
    defs={t['id']:t for t in tasks}
    if len(defs)!=len(tasks): raise ValueError('duplicate task ID')
    visited=set();stack=set()
    def visit(key):
        if key in stack: raise ValueError('dependency cycle at '+key)
        if key in visited:return
        stack.add(key)
        for dep in defs[key]['depends_on']:
            if dep not in defs: raise ValueError('unknown dependency '+dep)
            visit(dep)
        stack.remove(key);visited.add(key)
    for key,t in defs.items():
        visit(key)
        for field in ('reading_route','allowed_paths','deliverables','assertions','verify','stop_conditions'):
            if not t.get(field): raise ValueError(key+' missing '+field)
        if len(set(t['assertions']))!=len(t['assertions']):raise ValueError('duplicate assertion')
        for f in t['reading_route']['source_refs']:
            if not (WORKSPACE/f).exists():raise ValueError('missing source anchor '+f)
        for f in t['reading_route']['package_refs']+t['reading_route']['test_refs']:
            if not (PACKAGE/f).is_file():raise ValueError('missing package reference '+f)
        for pattern in t['allowed_paths']:
            if pattern.startswith('/') or '..' in Path(pattern).parts:raise ValueError('unsafe allowed path')
    manifest=read(PACKAGE/'specs/design_baseline_v2.json')
    if task_spec.get('design_revision')!=manifest['revision']:
        raise ValueError('task definitions do not match design baseline revision')
    validate_design_baseline(defs,manifest)
    for md in PACKAGE.rglob('*.md'):
        # These authored documents use ordinary inline links, not arbitrary nested Markdown.
        for target in re.findall(r'\]\(([^)]+)\)',md.read_text()):
            if re.match(r'^(https?://|#)',target):continue
            target=unquote(target.strip('<>').split('#')[0])
            if not (md.parent/target).exists():raise ValueError(f'broken link {md}: {target}')
    if check_state and (PACKAGE/'state/task_state.json').exists():
        state=read(PACKAGE/'state/task_state.json')
        if set(state['tasks'])!=set(defs):raise ValueError('state IDs differ from definitions')
        active=[i for i,s in state['tasks'].items() if s['status']=='in_progress']
        if active != ([state['active_task']] if state['active_task'] else []):raise ValueError('active-task mismatch')
        for key,s in state['tasks'].items():
            if s['status']=='completed':
                if not all(state['tasks'][d]['status']=='completed' for d in defs[key]['depends_on']):raise ValueError('completed task with incomplete dependency')
                if not s.get('evidence'):raise ValueError('completed task without evidence')
                evidence_valid(defs[key],read(PACKAGE/s['evidence']),current=False)
    spec=read(PACKAGE/'specs/acceptance.json')
    if spec['product']['non_regression_margin']!=0:raise ValueError('DML margin changed')
    if spec['development']['product_acceptance_eligible']:raise ValueError('development cannot qualify as product')
    if spec['design_requirements']['revision']!=manifest['revision'] or spec['design_requirements']['final_candidate']!='B_stable_vertex_ref':
        raise ValueError('acceptance is not bound to final stable-ref design')
    devstats=spec['statistics_development']
    if devstats['product_acceptance_eligible'] or devstats['automatic_collection_enabled']:
        raise ValueError('unvalidated statistics example cannot enable product/automatic collection')
    for key in ('vertex_sample_target','seed_target','max_hops','max_profiles','max_scan_pages','max_examined_entries',
                'max_entries_per_seed','max_bindings','max_bindings_per_seed','max_memory_bytes','max_payload_bytes','deadline_ms','lock_timeout_ms'):
        if not isinstance(devstats[key],int) or isinstance(devstats[key],bool) or devstats[key]<=0:
            raise ValueError('statistics budget must be finite and positive: '+key)
    pending=[k for k,v in spec['product'].items() if v is None]
    print(json.dumps({'package':'PASS','design_baseline':'V2','design_sha256':manifest['design_sha256'],
        'tasks':len(tasks),'deferred':sum(t.get('deferred',False) for t in tasks),'pending_business_inputs':pending,
        'pending_statistics_preregistration':[k for k,v in spec['statistics_preregistration'].items() if v is None],
        'product_gate':'NOT_RUN'},ensure_ascii=False,indent=2))
    return len(tasks)

if __name__=='__main__':
    try:validate()
    except (ValueError,RuntimeError,OSError,KeyError) as e:
        print(str(e),file=sys.stderr);sys.exit(1)
