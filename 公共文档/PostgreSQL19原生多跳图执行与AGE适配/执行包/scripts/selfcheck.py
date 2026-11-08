#!/usr/bin/env python3
"""Meaningful checks for evidence rejection, oracle cases and reference ABI build."""
import copy
import contextlib
import io
import itertools
import os
from pathlib import Path
import subprocess
import tempfile
import sys
from types import SimpleNamespace
from unittest.mock import patch
from common import PACKAGE, WORKSPACE, read, run_dir, run_logged, sha, write, now
from lab import paths
from oracle import selfcheck
import taskctl
import lab
import gate
import isolation
from performance import interval
from validate_package import validate, validate_design_baseline

def negative_checks():
    original_package,original_workspace=taskctl.PACKAGE,taskctl.WORKSPACE
    with tempfile.TemporaryDirectory(prefix='graph-package-selfcheck-') as temp:
        root=Path(temp);taskctl.PACKAGE=root;taskctl.WORKSPACE=root
        (root/'code.c').write_text('first implementation\n');(root/'test.log').write_text('real unit fixture\n')
        task={'allowed_paths':['code.c'],'verify':[{'argv':['true']}]}
        evidence={'definition_sha256':taskctl.digest(task),'implementation_sha256':taskctl.implementation_digest(task),
                  'commands':[{'argv':['true'],'exit_code':0,'log':'test.log','sha256':sha(root/'test.log')}]}
        taskctl.evidence_valid(task,evidence)
        def rejects(t,e):
            try:taskctl.evidence_valid(t,e)
            except RuntimeError:return
            raise AssertionError('invalid evidence was accepted')
        bad=copy.deepcopy(evidence);bad['commands'][0]['exit_code']=1;rejects(task,bad)
        bad=copy.deepcopy(evidence);bad['commands']=[];rejects(task,bad)
        bad=copy.deepcopy(evidence);bad['commands'][0]['argv']=['false'];rejects(task,bad)
        (root/'test.log').write_text('tampered\n');rejects(task,evidence)
        (root/'test.log').write_text('real unit fixture\n');(root/'code.c').write_text('changed code\n');rejects(task,evidence)
        (root/'code.c').write_text('first implementation\n');changed=copy.deepcopy(task);changed['verify'].append({'argv':['false']});rejects(changed,evidence)
    taskctl.PACKAGE,taskctl.WORKSPACE=original_package,original_workspace
    lo,hi=interval([1.10,1.12,1.09,1.11]*5,.01,2000,7)
    assert lo>1 and hi>lo
    lo,hi=interval([.89,.9,.91,.92]*5,.01,2000,7)
    assert hi<1
    print('PASS: evidence tamper/staleness/failure rejection and paired-interval direction')

def state_workflow_checks():
    with tempfile.TemporaryDirectory(prefix='graph-task-workflow-') as temp:
        root=Path(temp)
        test=root/'verify.py';test.write_text('print("actual fixture verification")\n')
        def task(key,deps=(),deferred=False):
            return {'id':key,'depends_on':list(deps),'deferred':deferred,'allowed_paths':['verify.py'],
                    'verify':[{'argv':['{python}',str(test)]}]}
        defs=root/'tasks.json';state=root/'state/task_state.json'
        write(defs,{'tasks':[task('A'),task('B',['A']),task('D',deferred=True)]})
        counter=itertools.count()
        def run_directory(label):
            out=root/'evidence'/f'{next(counter)}-{label}';out.mkdir(parents=True);return out
        with patch.multiple(taskctl,PACKAGE=root,WORKSPACE=root,DEFINITIONS=defs,STATE=state,run_dir=run_directory):
            def command(*args,fail=False):
                with patch.object(sys,'argv',['taskctl',*args]),contextlib.redirect_stdout(io.StringIO()):
                    try:taskctl.main()
                    except RuntimeError:
                        if fail:return
                        raise
                    if fail:raise AssertionError('invalid transition accepted: '+str(args))
            command('init')
            command('claim','B',fail=True)
            command('reopen','D','--reason','not an authorization',fail=True)
            command('claim','A')
            command('claim','B',fail=True)
            command('complete','A',fail=True)
            command('run','A')
            original=test.read_text();test.write_text(original+'# changed after validation\n')
            command('complete','A',fail=True)
            command('run','A');command('complete','A')
            command('claim','B');command('run','B');command('complete','B')
            previous=read(state)
            write(defs,{'tasks':[task('A'),task('B',['A']),task('D',deferred=True),task('C',['B']),task('E',deferred=True)]})
            command('sync-definitions',fail=True)
            command('sync-definitions','--reason','test append-only rebaseline')
            synced=read(state)
            assert synced['tasks']['A']==previous['tasks']['A'] and synced['tasks']['B']==previous['tasks']['B']
            assert synced['tasks']['C']=={'status':'todo','evidence':None}
            assert synced['tasks']['E']=={'status':'deferred','evidence':None}
            command('sync-definitions','--reason','idempotence')
            assert read(state)==synced
            valid_defs=read(defs)
            changed=copy.deepcopy(valid_defs);changed['tasks'][0]['title']='changed completed task'
            write(defs,changed);command('sync-definitions','--reason','reject history change',fail=True)
            changed=copy.deepcopy(valid_defs);changed['tasks']=[t for t in changed['tasks'] if t['id']!='D']
            write(defs,changed);command('sync-definitions','--reason','reject deletion',fail=True)
            changed=copy.deepcopy(valid_defs);changed['tasks'][3]['depends_on']=['C']
            write(defs,changed);command('sync-definitions','--reason','reject cycle',fail=True)
            write(defs,valid_defs)
            command('reopen','A','--reason','dependency changed')
            assert read(state)['tasks']['B']['status']=='todo'
            command('claim','A')
            command('sync-definitions','--reason','reject active',fail=True)
            test.write_text('raise SystemExit(1)\n')
            command('run','A',fail=True);command('complete','A',fail=True)
    print('PASS: task dependencies, single active task, append-only definition sync, failed verify and downstream reopen')

def clean_build_checks():
    with tempfile.TemporaryDirectory(prefix='graph-clean-build-') as temp:
        root=Path(temp);p={'build':root/'build/candidate','install':root/'install/candidate','runtime':root/'runtime/candidate'}
        p['build'].mkdir(parents=True);p['install'].mkdir(parents=True)
        write(p['build']/'build_manifest.json',{'status':'fixture'})
        (p['install']/'bin').mkdir();(p['install']/'bin/pg_config').write_text('fixture')
        out=root/'evidence';out.mkdir()
        def built(variant,jobs):
            assert not p['build'].exists() and not p['install'].exists()
            p['build'].mkdir();p['install'].mkdir()
            write(p['build']/'build_manifest.json',{'status':'fixture-rebuilt'})
        with patch.multiple(lab,verify_sources=lambda:None,paths=lambda _:p,source_root=lambda _:root/'source',
                            run_dir=lambda _:out,build=built,guard_isolation=lambda _:None):
            lab.build_clean('candidate',4)
            archive=read(out/'archived.json')
            assert (Path(archive['install'])/'bin/pg_config').read_text()=='fixture'
            assert read(p['build']/'build_manifest.json')['status']=='fixture-rebuilt'
            (p['runtime']/'data').mkdir(parents=True);(p['runtime']/'data/postmaster.pid').write_text('fixture')
            try:lab.build_clean('candidate',4)
            except RuntimeError:pass
            else:raise AssertionError('clean-build accepted running instance')
    print('PASS: clean-build archives owned products and refuses an active instance')

def isolation_checks():
    rejected = []
    def rejects(label, action):
        try: action()
        except RuntimeError as error:
            assert str(error).startswith('ISOLATION:'), str(error)
            rejected.append(label)
        else: raise AssertionError('isolation accepted: ' + label)
    with tempfile.TemporaryDirectory(prefix='graph-isolation-') as temp:
        root = Path(temp)
        p = {k: root / ('.' + k) / 'pg19-graph-core/candidate' for k in ('build','install','runtime')}
        p.update(port=55450, regress_port=55460)
        source = root / '.worktrees/pg19-graph-core'
        isolation.layout(root, 'candidate', p, source)
        release = {k: root / ('.' + k) / 'pg19-graph-core/btree-release'
                   for k in ('build', 'install', 'runtime')}
        release.update(port=55471, regress_port=55481)
        release_source = release['build'] / 'src'
        isolation.layout(root, 'btree-release', release, release_source)
        rejects('Release reuses candidate port', lambda:isolation.layout(
            root, 'btree-release', dict(release, port=55450), release_source))
        rejects('Release reuses candidate install', lambda:isolation.layout(
            root, 'btree-release', dict(release, install=p['install']), release_source))
        rejects('Release reuses candidate source', lambda:isolation.layout(
            root, 'btree-release', release, source))
        opt = {k: root / ('.' + k) / 'pg19-graph-core/btree-opt'
               for k in ('build', 'install', 'runtime')}
        opt.update(port=55472, regress_port=55482)
        opt_source = opt['build'] / 'src'
        isolation.layout(root, 'btree-opt', opt, opt_source)
        rejects('optimization reuses Release port', lambda:isolation.layout(
            root, 'btree-opt', dict(opt, port=55471), opt_source))
        rejects('optimization reuses Release install', lambda:isolation.layout(
            root, 'btree-opt', dict(opt, install=release['install']), opt_source))
        rejects('optimization reuses candidate source', lambda:isolation.layout(
            root, 'btree-opt', opt, source))
        reuse = {k: root / ('.' + k) / 'pg19-graph-core/btree-reuse'
                 for k in ('build', 'install', 'runtime')}
        reuse.update(port=55473, regress_port=55485)
        reuse_source = reuse['build'] / 'src'
        isolation.layout(root, 'btree-reuse', reuse, reuse_source)
        rejects('reuse experiment shares opt port', lambda:isolation.layout(
            root, 'btree-reuse', dict(reuse, port=55472), reuse_source))
        rejects('reuse experiment shares opt install', lambda:isolation.layout(
            root, 'btree-reuse', dict(reuse, install=opt['install']), reuse_source))
        rejects('reuse experiment shares candidate source', lambda:isolation.layout(
            root, 'btree-reuse', reuse, source))
        compact = {k: root / ('.' + k) / 'pg19-graph-core/btree-compact'
                   for k in ('build', 'install', 'runtime')}
        compact.update(port=55474, regress_port=55489)
        compact_source = compact['build'] / 'src'
        isolation.layout(root, 'btree-compact', compact, compact_source)
        rejects('compact experiment shares reuse port', lambda:isolation.layout(
            root, 'btree-compact', dict(compact, port=55473), compact_source))
        rejects('compact experiment shares reuse install', lambda:isolation.layout(
            root, 'btree-compact', dict(compact, install=reuse['install']), compact_source))
        rejects('compact experiment shares candidate source', lambda:isolation.layout(
            root, 'btree-compact', compact, source))
        startup = {k: root / ('.' + k) / 'pg19-graph-core/btree-startup'
                   for k in ('build', 'install', 'runtime')}
        startup.update(port=55475, regress_port=55491)
        startup_source = startup['build'] / 'src'
        isolation.layout(root, 'btree-startup', startup, startup_source)
        rejects('startup experiment shares compact port', lambda:isolation.layout(
            root, 'btree-startup', dict(startup, port=55474), startup_source))
        rejects('startup experiment shares compact install', lambda:isolation.layout(
            root, 'btree-startup', dict(startup, install=compact['install']), startup_source))
        rejects('startup experiment shares candidate source', lambda:isolation.layout(
            root, 'btree-startup', startup, source))
        deform = {k: root / ('.' + k) / 'pg19-graph-core/btree-deform'
                  for k in ('build', 'install', 'runtime')}
        deform.update(port=55476, regress_port=55493)
        deform_source = deform['build'] / 'src'
        isolation.layout(root, 'btree-deform', deform, deform_source)
        rejects('deform experiment shares startup port', lambda:isolation.layout(
            root, 'btree-deform', dict(deform, port=55475), deform_source))
        rejects('deform experiment shares startup install', lambda:isolation.layout(
            root, 'btree-deform', dict(deform, install=startup['install']), deform_source))
        rejects('deform experiment shares candidate source', lambda:isolation.layout(
            root, 'btree-deform', deform, source))
        refview = {k: root / ('.' + k) / 'pg19-graph-core/btree-refview'
                   for k in ('build', 'install', 'runtime')}
        refview.update(port=55477, regress_port=55495)
        refview_source = refview['build'] / 'src'
        isolation.layout(root, 'btree-refview', refview, refview_source)
        rejects('refview experiment shares deform port', lambda:isolation.layout(
            root, 'btree-refview', dict(refview, port=55476), refview_source))
        rejects('refview experiment shares deform install', lambda:isolation.layout(
            root, 'btree-refview', dict(refview, install=deform['install']), refview_source))
        rejects('refview experiment shares candidate source', lambda:isolation.layout(
            root, 'btree-refview', refview, source))
        array = {k: root / ('.' + k) / 'pg19-graph-core/btree-array'
                 for k in ('build', 'install', 'runtime')}
        array.update(port=55478, regress_port=55497)
        array_source = array['build'] / 'src'
        isolation.layout(root, 'btree-array', array, array_source)
        rejects('array experiment shares refview port', lambda:isolation.layout(
            root, 'btree-array', dict(array, port=55477), array_source))
        rejects('array experiment shares refview install', lambda:isolation.layout(
            root, 'btree-array', dict(array, install=refview['install']), array_source))
        rejects('array experiment shares candidate source', lambda:isolation.layout(
            root, 'btree-array', array, source))
        tail = {k: root / ('.' + k) / 'pg19-graph-core/btree-tail'
                for k in ('build', 'install', 'runtime')}
        tail.update(port=55479, regress_port=55499)
        tail_source = tail['build'] / 'src'
        isolation.layout(root, 'btree-tail', tail, tail_source)
        rejects('tail experiment shares array port', lambda:isolation.layout(
            root, 'btree-tail', dict(tail, port=55478), tail_source))
        rejects('tail experiment shares array install', lambda:isolation.layout(
            root, 'btree-tail', dict(tail, install=array['install']), tail_source))
        rejects('tail experiment shares candidate source', lambda:isolation.layout(
            root, 'btree-tail', tail, source))
        factor = {k: root / ('.' + k) / 'pg19-graph-core/btree-factor'
                  for k in ('build', 'install', 'runtime')}
        factor.update(port=55501, regress_port=55502)
        factor_source = factor['build'] / 'src'
        isolation.layout(root, 'btree-factor', factor, factor_source)
        rejects('factor experiment shares tail port', lambda:isolation.layout(
            root, 'btree-factor', dict(factor, port=55479), factor_source))
        rejects('factor experiment shares array install', lambda:isolation.layout(
            root, 'btree-factor', dict(factor, install=array['install']), factor_source))
        rejects('factor experiment shares candidate source', lambda:isolation.layout(
            root, 'btree-factor', factor, source))
        factor_batch = {k: root / ('.' + k) / 'pg19-graph-core/btree-factor-batch'
                        for k in ('build', 'install', 'runtime')}
        factor_batch.update(port=55504, regress_port=55505)
        batch_source = factor_batch['build'] / 'src'
        isolation.layout(root, 'btree-factor-batch', factor_batch, batch_source)
        rejects('factor batch shares factor port', lambda:isolation.layout(
            root, 'btree-factor-batch', dict(factor_batch, port=55501), batch_source))
        rejects('factor batch shares factor install', lambda:isolation.layout(
            root, 'btree-factor-batch', dict(factor_batch, install=factor['install']), batch_source))
        rejects('factor batch shares candidate source', lambda:isolation.layout(
            root, 'btree-factor-batch', factor_batch, source))
        anchor_stats = {k: root / ('.' + k) / 'pg19-graph-core/btree-anchor-stats'
                        for k in ('build', 'install', 'runtime')}
        anchor_stats.update(port=55507, regress_port=55508)
        anchor_source = anchor_stats['build'] / 'src'
        isolation.layout(root, 'btree-anchor-stats', anchor_stats, anchor_source)
        rejects('anchor statistics share batch port', lambda:isolation.layout(
            root, 'btree-anchor-stats', dict(anchor_stats, port=55504), anchor_source))
        rejects('anchor statistics share batch install', lambda:isolation.layout(
            root, 'btree-anchor-stats', dict(anchor_stats, install=factor_batch['install']), anchor_source))
        rejects('anchor statistics share candidate source', lambda:isolation.layout(
            root, 'btree-anchor-stats', anchor_stats, source))
        suffix = {k: root / ('.' + k) / 'pg19-graph-core/btree-suffix'
                  for k in ('build', 'install', 'runtime')}
        suffix.update(port=55510, regress_port=55511)
        suffix_source = suffix['build'] / 'src'
        isolation.layout(root, 'btree-suffix', suffix, suffix_source)
        rejects('suffix shares batch port', lambda:isolation.layout(
            root, 'btree-suffix', dict(suffix, port=55504), suffix_source))
        rejects('suffix shares batch install', lambda:isolation.layout(
            root, 'btree-suffix', dict(suffix, install=factor_batch['install']), suffix_source))
        rejects('suffix shares candidate source', lambda:isolation.layout(
            root, 'btree-suffix', suffix, source))
        direct = {k: root / ('.' + k) / 'pg19-graph-core/btree-direct'
                  for k in ('build','install','runtime')}
        direct.update(port=55513,regress_port=55514)
        direct_source = direct['build']/'src'
        isolation.layout(root,'btree-direct',direct,direct_source)
        rejects('direct shares batch port',lambda:isolation.layout(
            root,'btree-direct',dict(direct,port=55504),direct_source))
        rejects('direct shares batch install',lambda:isolation.layout(
            root,'btree-direct',dict(direct,install=factor_batch['install']),direct_source))
        rejects('direct shares candidate source',lambda:isolation.layout(
            root,'btree-direct',direct,source))
        throughput = {k: root / ('.' + k) / 'pg19-graph-core/btree-throughput'
                      for k in ('build','install','runtime')}
        throughput.update(port=55516,regress_port=55517)
        throughput_source = throughput['build']/'src'
        isolation.layout(root,'btree-throughput',throughput,throughput_source)
        rejects('throughput shares batch port',lambda:isolation.layout(
            root,'btree-throughput',dict(throughput,port=55504),throughput_source))
        rejects('throughput shares batch install',lambda:isolation.layout(
            root,'btree-throughput',dict(throughput,install=factor_batch['install']),throughput_source))
        rejects('throughput shares candidate source',lambda:isolation.layout(
            root,'btree-throughput',throughput,source))
        fused = {k: root / ('.'+k) / 'pg19-graph-core/btree-fused'
                 for k in ('build','install','runtime')}
        fused.update(port=55519,regress_port=55520)
        fused_source = fused['build']/'src'
        isolation.layout(root,'btree-fused',fused,fused_source)
        rejects('fused shares batch port',lambda:isolation.layout(
            root,'btree-fused',dict(fused,port=55504),fused_source))
        rejects('fused shares batch install',lambda:isolation.layout(
            root,'btree-fused',dict(fused,install=factor_batch['install']),fused_source))
        rejects('fused shares candidate source',lambda:isolation.layout(
            root,'btree-fused',fused,source))
        cursor = {k: root / ('.'+k) / 'pg19-graph-core/btree-cursor'
                  for k in ('build','install','runtime')}
        cursor.update(port=55522,regress_port=55523)
        cursor_source = cursor['build']/'src'
        isolation.layout(root,'btree-cursor',cursor,cursor_source)
        rejects('cursor shares batch port',lambda:isolation.layout(
            root,'btree-cursor',dict(cursor,port=55504),cursor_source))
        rejects('cursor shares batch install',lambda:isolation.layout(
            root,'btree-cursor',dict(cursor,install=factor_batch['install']),cursor_source))
        rejects('cursor shares candidate source',lambda:isolation.layout(
            root,'btree-cursor',cursor,source))
        lease = {k: root / ('.'+k) / 'pg19-graph-core/btree-lease'
                 for k in ('build','install','runtime')}
        lease.update(port=55525,regress_port=55526)
        lease_source = lease['build']/'src'
        isolation.layout(root,'btree-lease',lease,lease_source)
        rejects('lease shares batch port',lambda:isolation.layout(
            root,'btree-lease',dict(lease,port=55504),lease_source))
        rejects('lease shares batch install',lambda:isolation.layout(
            root,'btree-lease',dict(lease,install=factor_batch['install']),lease_source))
        rejects('lease shares candidate source',lambda:isolation.layout(
            root,'btree-lease',lease,source))
        layout = {k: root / ('.'+k) / 'pg19-graph-core/btree-layout'
                  for k in ('build','install','runtime')}
        layout.update(port=55528,regress_port=55529)
        layout_source = layout['build']/'src'
        isolation.layout(root,'btree-layout',layout,layout_source)
        rejects('layout shares lease port',lambda:isolation.layout(
            root,'btree-layout',dict(layout,port=55525),layout_source))
        rejects('layout shares lease install',lambda:isolation.layout(
            root,'btree-layout',dict(layout,install=lease['install']),layout_source))
        rejects('layout shares candidate source',lambda:isolation.layout(
            root,'btree-layout',layout,source))
        old = root / '.install/pg19-pgorca-daft';old.mkdir(parents=True)
        p['install'].parent.mkdir(parents=True);p['install'].symlink_to(old, target_is_directory=True)
        rejects('symlink to old installation', lambda:isolation.layout(root,'candidate',p,source))
        p['install'].unlink();p['install'].mkdir()
        settings = isolation.expected_settings(p)
        isolation.check_settings(settings, p)
        rejects('preload ORCA',lambda:isolation.check_settings(dict(settings,shared_preload_libraries='pg_orca'),p))
        rejects('old dynamic library path',lambda:isolation.check_settings(dict(settings,dynamic_library_path=str(old/'lib/postgresql')),p))
        artifact = p['install'] / 'pg_orca.so';artifact.write_text('fixture')
        rejects('foreign extension artifact',lambda:isolation.check_install(p['install'],lab.clean_env()))
        artifact.unlink()
        (p['install']/'bin').mkdir()
        for name in ('pg_config','postgres','pg_ctl','initdb','psql'):
            (p['install']/'bin'/name).write_text('fixture')
        with patch.object(isolation,'command',return_value=str(old/'bin')):
            rejects('pg_config compiled for old prefix',lambda:isolation.check_install(p['install'],lab.clean_env()))
        for name in ('postgres19','apache-age-pg19-port'):
            repo = source/name;(repo/'.git/objects/info').mkdir(parents=True)
            (repo/'.git/objects/info/alternates').write_text(str(root/'old-git-objects'))
        with patch.object(isolation,'command',return_value=str(source/'postgres19/.git')):
            rejects('borrowed Git object database',lambda:isolation.offline(root,'candidate',p,source,lab.clean_env()))
        good = {'settings':settings,'foreign_settings':[],'extensions':['plpgsql','age'],
                'foreign_functions':[],'role_database_settings':[],'configuration_files':[]}
        isolation.check_runtime_record(good,p)
        rejects('role-level ORCA preload',lambda:isolation.check_runtime_record(dict(good,role_database_settings=['session_preload_libraries=pg_orca']),p))
        rejects('foreign configuration include',lambda:isolation.check_runtime_record(dict(good,configuration_files=[str(old/'postgresql.conf')]),p))
        rejects('registered ORCA hook GUC',lambda:isolation.check_runtime_record(dict(good,foreign_settings=['pg_orca.enable_orca']),p))
        # Exercise postgres -C itself: effective settings must include both
        # included files and ALTER SYSTEM's auto.conf, not just textual defaults.
        data = p['runtime']/'data';data.mkdir(parents=True)
        (data/'postgresql.conf').write_text("include = 'extra.conf'\n")
        (data/'extra.conf').write_text("shared_preload_libraries = 'pg_orca'\n")
        postgres = paths('baseline')['install']/'bin/postgres'
        actual = isolation.command([postgres,'-D',data,'-C','shared_preload_libraries'],lab.clean_env())
        assert actual == 'pg_orca'
        rejects('effective include preload',lambda:isolation.check_settings(dict(settings,shared_preload_libraries=actual),p))
        (data/'extra.conf').write_text("shared_preload_libraries = ''\n")
        (data/'postgresql.auto.conf').write_text("session_preload_libraries = 'pg_orca'\n")
        actual = isolation.command([postgres,'-D',data,'-C','session_preload_libraries'],lab.clean_env())
        assert actual == 'pg_orca'
        rejects('effective ALTER SYSTEM preload',lambda:isolation.check_settings(dict(settings,session_preload_libraries=actual),p))
        with patch.dict(os.environ, {'PATH':str(old/'bin'),'PGOPTIONS':'-c session_preload_libraries=pg_orca',
                                    'LD_LIBRARY_PATH':str(old/'lib'),'DESTDIR':str(old),'CPATH':str(old/'include'),
                                    'GIT_WORK_TREE':str(root/'postgres19'),'PKG_CONFIG_PATH':str(old/'lib/pkgconfig')}):
            env=lab.clean_env()
            assert env['PATH']=='/usr/bin:/bin'
            assert not any(k in env for k in ('PGOPTIONS','LD_LIBRARY_PATH','DESTDIR','CPATH','GIT_WORK_TREE','PKG_CONFIG_PATH'))
    print('PASS: isolation rejects ' + ', '.join(rejected) + '; caller environment sanitized')
    return rejected

def gate_checks():
    with tempfile.TemporaryDirectory(prefix='graph-gate-check-') as temp:
        root=Path(temp)
        old=root/'evidence/runs/001-fixture';old.mkdir(parents=True)
        write(old/'summary.json',{'status':'PASS'})
        latest=root/'evidence/runs/002-fixture';latest.mkdir()
        write(root/'specs/tasks.json',{'tasks':[{'id':'FIXTURE-NOT-IMPLEMENTED'}]})
        write(root/'specs/acceptance.json',{'product':{'status':'NEEDS_INPUT','slo':None}})
        with patch.object(gate,'PACKAGE',root):
            try:gate.receipt('fixture')
            except RuntimeError:pass
            else:raise AssertionError('latest failure was replaced with stale PASS')
            for action,signature in [(lambda:gate.task_check('FIXTURE-NOT-IMPLEMENTED'),'NOT_IMPLEMENTED'),
                                     (lambda:gate.check('business-input'),'NEEDS_INPUT')]:
                try:action()
                except RuntimeError as error:
                    if signature not in str(error):raise
                else:raise AssertionError('missing implementation/business input did not fail closed')
    print('PASS: missing feature, missing business input and latest failed run are rejected')

def v2_contract_checks():
    manifest=read(PACKAGE/'specs/design_baseline_v2.json')
    defs={t['id']:t for t in read(PACKAGE/'specs/tasks.json')['tasks']}
    rejected=[]
    def rejects(label, candidate_defs, candidate_manifest):
        try:validate_design_baseline(candidate_defs,candidate_manifest)
        except (ValueError,KeyError,OSError):rejected.append(label)
        else:raise AssertionError('V2 baseline accepted '+label)
    bad=copy.deepcopy(manifest);bad['design_sha256']='0'*64
    rejects('design drift',defs,bad)
    bad=copy.deepcopy(defs);bad['G1-050']['depends_on']=['G1-040']
    rejects('costing before statistics',bad,manifest)
    bad=copy.deepcopy(defs);bad['GS-010']['assertions'].remove('key_to_ref_leaf_no_descriptor')
    rejects('lost stable-ref assertion',bad,manifest)
    bad=copy.deepcopy(defs);bad['G1-010']['reading_route']['package_refs'].append('include/graph_provider_v1.reference.h')
    rejects('active legacy ABI',bad,manifest)
    bad=copy.deepcopy(manifest);bad['legacy_reference_sha256']['include/graph_provider_v1.reference.h']='0'*64
    rejects('rewritten historical reference',defs,bad)
    with tempfile.TemporaryDirectory(prefix='graph-statistics-gate-') as temp:
        root=Path(temp);(root/'checks').mkdir();(root/'checks/STATS.py').write_text('# test fixture only\n')
        evidence=root/'evidence';evidence.mkdir();recordfile=evidence/'actual.json'
        write(recordfile,{'fixture':'gate integrity check, not a database feature test'})
        item={'path':'evidence/actual.json','sha256':sha(recordfile)}
        task={'id':'STATS','assertions':['fixture'],'requires_maintenance_execution':True,'requires_statistics_evidence':True}
        write(root/'specs/tasks.json',{'tasks':[task]})
        good={'task_id':'STATS','status':'PASS','assertions_passed':['fixture'],'evidence_files':[item],
              'maintenance_execution_evidence':item,'statistics_evidence':item}
        import json
        def check_record(record, should_fail):
            result=SimpleNamespace(returncode=0,stdout=json.dumps(record)+'\n',stderr='')
            with patch.object(gate,'PACKAGE',root),patch.object(gate.subprocess,'run',return_value=result),contextlib.redirect_stdout(io.StringIO()):
                try:gate.task_check('STATS')
                except RuntimeError:
                    if should_fail:return
                    raise
                if should_fail:raise AssertionError('statistics gate accepted incomplete evidence')
        check_record(good,False)
        for field in ('maintenance_execution_evidence','statistics_evidence'):
            bad=copy.deepcopy(good);bad.pop(field);check_record(bad,True)
            bad=copy.deepcopy(good);bad[field]['sha256']='0'*64;check_record(bad,True)
    print('PASS: V2 drift/dependency/reference rejection and required statistics evidence integrity')
    return rejected

def main():
    validate();selfcheck();negative_checks();state_workflow_checks();clean_build_checks();gate_checks()
    isolation_cases=isolation_checks()
    v2_rejections=v2_contract_checks()
    out=run_dir('package-selfcheck')
    p=paths('baseline')
    commands=[]
    with tempfile.TemporaryDirectory(prefix='graph-header-check-') as temp:
        source=Path(temp)/'reference.c'
        headers=read(PACKAGE/'specs/design_baseline_v2.json')['active_reference_headers']
        source.write_text(''.join('#include "'+Path(h).name+'"\n' for h in headers)+'int main(void) { return 0; }\n')
        commands.append(run_logged(['gcc','-fsyntax-only','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
            '-I'+str(p['install']/'include/postgresql/server'),'-I'+str(p['install']/'include'),
            '-I'+str(PACKAGE/'include'),source],out,'reference-headers'))
    write(out/'summary.json',{'status':'PASS','scope':'execution package only','finished':now(),'commands':commands,
         'isolation_rejections':isolation_cases,'v2_rejections':v2_rejections,
         'oracle_cases':16,'task_count':len(read(PACKAGE/'specs/tasks.json')['tasks']),'product_gate':'NOT_RUN'})
    print(out)

if __name__=='__main__':main()
