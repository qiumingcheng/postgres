#!/usr/bin/env python3
"""Single-active-task ledger with command evidence and stale-evidence rejection."""
from __future__ import annotations
import argparse
import fcntl
import fnmatch
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from common import PACKAGE, WORKSPACE, now, read, run_dir, sha, write

DEFINITIONS = PACKAGE/'specs/tasks.json'
STATE = PACKAGE/'state/task_state.json'

def digest(value):
    return hashlib.sha256(json.dumps(value,sort_keys=True,ensure_ascii=False).encode()).hexdigest()

def definitions():
    return {t['id']:t for t in read(DEFINITIONS)['tasks']}

def implementation_digest(task):
    entries={}
    repositories={}
    for pattern in task['allowed_paths']:
        parts=Path(pattern).parts
        # PGXS builds inside AGE source. Hash tracked and non-ignored new source,
        # not ignored .o/.d/results created by a verification build.
        if len(parts)>2 and parts[:2]==('.worktrees','pg19-graph-core'):
            prefix='/'.join(parts[:3])
            repo=WORKSPACE/prefix
            if not repo.exists():continue
            if prefix not in repositories:
                listing=subprocess.check_output(['git','ls-files','--cached','--others','--exclude-standard','-z'],cwd=repo,text=True)
                repositories[prefix]=[prefix+'/'+x for x in listing.split('\0') if x]
            for relative in repositories[prefix]:
                if fnmatch.fnmatchcase(relative,pattern):
                    f=WORKSPACE/relative
                    entries[relative]=sha(f) if f.is_file() else 'DELETED'
            continue
        for path in WORKSPACE.glob(pattern):
            files=path.rglob('*') if path.is_dir() else [path]
            for f in files:
                if f.is_file() and '.git' not in f.parts and '__pycache__' not in f.parts:
                    entries[str(f.relative_to(WORKSPACE))]=sha(f)
    return digest(entries)

def initialize():
    if STATE.exists():
        raise RuntimeError('state already exists; never reset progress')
    defs=definitions()
    write(STATE,{'version':1,'active_task':None,'updated_at':now(),
                 'tasks':{key:{'status':'deferred' if t.get('deferred') else 'todo','evidence':None} for key,t in defs.items()}})

def ready(state,defs):
    return [key for key,t in defs.items() if state['tasks'][key]['status']=='todo'
            and all(state['tasks'][d]['status']=='completed' for d in t['depends_on'])]

def sync_definitions(state, defs, reason):
    """Append new TODO/deferred entries without rewriting historical receipts."""
    if not reason or not reason.strip():
        raise RuntimeError('definition sync requires a reason')
    if state['active_task'] is not None or any(s['status']=='in_progress' for s in state['tasks'].values()):
        raise RuntimeError('finish/block the active task before definition sync')
    removed=set(state['tasks'])-set(defs)
    if removed:
        raise RuntimeError('definition sync cannot remove historical tasks: '+','.join(sorted(removed)))
    for key, entry in state['tasks'].items():
        if entry['status']=='completed':
            if not entry.get('evidence'):
                raise RuntimeError('completed task without evidence')
            evidence_valid(defs[key],read(PACKAGE/entry['evidence']),current=False)
            if not all(state['tasks'].get(dep,{}).get('status')=='completed' for dep in defs[key]['depends_on']):
                raise RuntimeError('completed task cannot acquire unfinished dependencies')
    # Validate the entire new graph before touching the ledger.
    visited=set(); stack=set()
    def visit(key):
        if key not in defs: raise RuntimeError('unknown dependency '+key)
        if key in stack: raise RuntimeError('definition cycle at '+key)
        if key in visited:return
        stack.add(key)
        for dep in defs[key]['depends_on']:visit(dep)
        stack.remove(key);visited.add(key)
    for key in defs:visit(key)
    added=[key for key in defs if key not in state['tasks']]
    if not added:
        return []
    for key in added:
        state['tasks'][key]={'status':'deferred' if defs[key].get('deferred') else 'todo','evidence':None}
    state.setdefault('definition_syncs',[]).append({'at':now(),'reason':reason,
        'added_tasks':added,'definitions_sha256':digest(defs),'completed_entries_preserved':True})
    state['updated_at']=now()
    write(STATE,state)
    return added

def evidence_valid(task, evidence, current=True):
    if evidence['definition_sha256']!=digest(task): raise RuntimeError('task definition changed since verification')
    if current and evidence['implementation_sha256']!=implementation_digest(task): raise RuntimeError('implementation changed since verification')
    if not evidence['commands'] or len(evidence['commands'])!=len(task['verify']): raise RuntimeError('missing mandatory commands')
    for command,definition in zip(evidence['commands'],task['verify']):
        log=(PACKAGE/command['log']).resolve()
        expected=command_for(definition['argv'])
        # Python executable paths can vary across hosts; compare the recorded
        # interpreter independently, and require the actual command arguments.
        actual=command.get('argv',[])
        argv_ok=(actual==expected or (definition['argv'][0]=='{python}' and actual[1:]==expected[1:] and bool(actual)))
        if not argv_ok or command['exit_code']!=0 or not log.is_relative_to(PACKAGE.resolve()) or not log.is_file() or sha(log)!=command['sha256']:
            raise RuntimeError('failed, missing or modified command evidence')

def command_for(argv):
    return [s.replace('{python}',sys.executable).replace('{package}',str(PACKAGE)).replace('{workspace}',str(WORKSPACE)) for s in argv]

def execute(task,state):
    out=run_dir('task-'+task['id'])
    commands=[]
    before=implementation_digest(task)
    for i,definition in enumerate(task['verify']):
        argv=command_for(definition['argv'])
        log=out/f'verify-{i}.log'
        with log.open('w') as stream:
            try:
                result=subprocess.run(argv,cwd=WORKSPACE,stdout=stream,stderr=subprocess.STDOUT,
                                      timeout=definition.get('timeout_seconds',1800))
                code=result.returncode
            except subprocess.TimeoutExpired:
                code=124
                stream.write('\nTIMEOUT\n')
        commands.append({'argv':argv,'exit_code':code,'log':str(log.relative_to(PACKAGE)),'sha256':sha(log)})
        print(f'{task["id"]} verify {i}: {code}; {log}',flush=True)
        if code:
            print(log.read_text(errors='replace')[-3000:],flush=True)
            break
    after=implementation_digest(task)
    ev={'task_id':task['id'],'finished':now(),'definition_sha256':digest(task),
        'implementation_sha256':after,'commands':commands,
        'verification_modified_implementation':before!=after}
    write(out/'evidence.json',ev)
    state['tasks'][task['id']]['evidence']=str((out/'evidence.json').relative_to(PACKAGE))
    write(STATE,state)
    evidence_valid(task,ev)
    if before!=after: raise RuntimeError('verification modified tracked deliverables; inspect and rerun checks')

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('command',choices=['init','status','list-ready','show','claim','run','complete','block','reopen','sync-definitions'])
    p.add_argument('task',nargs='?');p.add_argument('--reason');p.add_argument('--actor',default='coding-agent')
    x=p.parse_args()
    STATE.parent.mkdir(parents=True,exist_ok=True)
    with (STATE.parent/'.taskctl.lock').open('a') as lock:
        fcntl.flock(lock,fcntl.LOCK_EX)
        if x.command=='init': initialize();return
        defs=definitions();state=read(STATE)
        if x.command=='sync-definitions':
            print(json.dumps({'added_tasks':sync_definitions(state,defs,x.reason),'active_task':state['active_task'],
                'completed_entries_preserved':True},ensure_ascii=False,indent=2));return
        if x.command=='status': print(json.dumps(state,ensure_ascii=False,indent=2));return
        if x.command=='list-ready':
            print(json.dumps({'active_task':state['active_task'],'ready':[{'id':i,'title':defs[i]['title']} for i in ready(state,defs)]},ensure_ascii=False,indent=2));return
        if x.task not in defs: raise RuntimeError('unknown task')
        task=defs[x.task];entry=state['tasks'][x.task]
        if x.command=='show': print(json.dumps({'definition':task,'state':entry},ensure_ascii=False,indent=2));return
        if x.command=='claim':
            if state['active_task'] is not None or x.task not in ready(state,defs): raise RuntimeError('task not ready or another task is active')
            state['active_task']=x.task;entry.update({'status':'in_progress','actor':x.actor,'started':now(),'evidence':None})
        elif x.command=='reopen':
            if not x.reason or state['active_task'] is not None: raise RuntimeError('reason required; finish/block active task first')
            if task.get('deferred'):raise RuntimeError('deferred scope must be explicitly changed in the task definition first')
            # Completed downstream work cannot remain falsely valid after a prerequisite is reopened.
            affected={x.task}
            while True:
                new=affected|{k for k,t in defs.items() if any(d in affected for d in t['depends_on']) and state['tasks'][k]['status']=='completed'}
                if new==affected: break
                affected=new
            for key in affected:
                old=state['tasks'][key].copy()
                state['tasks'][key]={'status':'todo','evidence':None,'previous':old,'reopened':now(),'reason':x.reason}
        else:
            if state['active_task']!=x.task or entry['status']!='in_progress': raise RuntimeError('must own the active task')
            if x.command=='run': execute(task,state);return
            if x.command=='block':
                if not x.reason: raise RuntimeError('reason required')
                entry.update({'status':'blocked','reason':x.reason});state['active_task']=None
            elif x.command=='complete':
                if not entry['evidence']: raise RuntimeError('run mandatory checks first')
                ev=read(PACKAGE/entry['evidence']);evidence_valid(task,ev)
                if ev.get('verification_modified_implementation'): raise RuntimeError('verification changed deliverables')
                entry.update({'status':'completed','finished':now()});state['active_task']=None
        state['updated_at']=now();write(STATE,state)
        print(x.command+' '+x.task)

if __name__=='__main__':
    try: main()
    except (RuntimeError,KeyError,OSError,ValueError) as error:
        print(str(error),file=sys.stderr);sys.exit(1)
