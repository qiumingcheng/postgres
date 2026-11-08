#!/usr/bin/env python3
"""Conservative, manifest-bound cleanup of completed graph runtime copies.

Never removes a build, installation, source file, evidence file, DML baseline,
failed/incomplete experiment, or any runtime referenced as an input.  Preparing
a plan is non-destructive.  Applying it requires the reviewed plan SHA256.
"""
import argparse
import collections
import datetime
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import tarfile

WORKSPACE = Path('/home/qiumc/data/code/cpp')
TOPIC = WORKSPACE / '公共文档/PostgreSQL19原生多跳图执行与AGE适配'
PACKAGE = TOPIC / '执行包'
RUNS = PACKAGE / 'evidence/runs'
RUNTIME = WORKSPACE / '.runtime/pg19-graph-core'
ARCHIVES = PACKAGE / 'evidence/archives'
NAME = re.compile(r'(?:dml-aa|m6-commit-dml)-[a-z0-9_]{6,}')
RUN_ID = re.compile(r'2026\d{4}T\d{6}(?:\.\d+)?Z-[A-Za-z0-9_-]+')
SOURCE_SUFFIXES = {'.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.hxx',
                   '.inc', '.s', '.rs', '.py', '.sh', '.pl', '.pm', '.y', '.l'}
COMPLETE = {'PASS', 'INSUFFICIENT_CLEAN_ROUNDS', 'INSUFFICIENT_CLEAN_PERIODS',
            'ENGINEERING_COMPLETE', 'SCREEN_PASS', 'SCREEN_COMPLETE',
            'VALIDATION_PASS', 'AUDITED', 'DIAGNOSTIC_COMPLETE',
            'CAPTURE_WITH_ENVIRONMENT_NOISE'}
INPUT_KEY = re.compile(r'seed|prior|source|baseline|input', re.I)
ENV = dict(os.environ, GIT_OPTIONAL_LOCKS='0', PYTHONDONTWRITEBYTECODE='1')


def now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def save(path, value):
    # All generated records are confined to the new audit directory.
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n')


def emit(value):
    print(json.dumps(value, ensure_ascii=False), flush=True)


def scan(path):
    """No symlink following; record identity and exclude source-bearing trees."""
    if path.parent != RUNTIME or path.resolve() != path or not NAME.fullmatch(path.name):
        raise RuntimeError(f'Not an exact owned runtime directory: {path}')
    entries, sources, unsafe, occupied = [], [], [], []
    total = 0
    for directory, dirs, files in os.walk(path, followlinks=False):
        dirs.sort()
        for name in ['.'] + sorted(files + dirs):
            p = Path(directory) if name == '.' else Path(directory) / name
            s = p.lstat()
            if name != '.' and stat.S_ISDIR(s.st_mode):
                continue  # Each real directory is recorded when traversed.
            relative = str(p.relative_to(path))
            target = os.readlink(p) if stat.S_ISLNK(s.st_mode) else None
            entries.append([relative, s.st_dev, s.st_ino, s.st_mode, s.st_size,
                            s.st_mtime_ns, s.st_ctime_ns, s.st_nlink, target])
            total += s.st_blocks * 512
            if p.suffix.lower() in SOURCE_SUFFIXES or name in ('Makefile', 'GNUmakefile', 'meson.build', '.git'):
                sources.append(relative)
            if target is not None or os.path.ismount(p) or s.st_dev != path.stat().st_dev:
                unsafe.append(relative)
            if name == 'postmaster.pid' or stat.S_ISSOCK(s.st_mode):
                occupied.append(relative)
            if not (stat.S_ISREG(s.st_mode) or stat.S_ISDIR(s.st_mode) or stat.S_ISLNK(s.st_mode)):
                unsafe.append(relative)
    encoded = json.dumps(sorted(entries), separators=(',', ':')).encode()
    return dict(metadata_sha256=hashlib.sha256(encoded).hexdigest(),
                entries=len(entries), allocated_bytes=total, source_files=sources,
                unsafe=unsafe, occupied=occupied)


def input_strings(value, active=False):
    if isinstance(value, dict):
        for key, child in value.items():
            yield from input_strings(child, active or bool(INPUT_KEY.search(key)))
    elif isinstance(value, list):
        for child in value:
            yield from input_strings(child, active)
    elif active and isinstance(value, str):
        yield value


def top_metadata(value):
    # Large sample/case arrays are not needed to retain their input identities.
    selected = {k: v for k, v in value.items() if INPUT_KEY.search(k)}
    return dict(status=value.get('status', 'NO_STATUS'),
                inputs=list(input_strings(selected)),
                seed_output=any('output_seed' in k for k in value),
                stopped={k: v for k, v in value.items() if 'stopped' in k})


def metadata_guard(roots, excluded):
    h = hashlib.sha256(); count = 0
    for root in roots:
        if not root.exists():
            continue
        for directory, dirs, files in os.walk(root, followlinks=False):
            dirs[:] = sorted(d for d in dirs if Path(directory) / d not in excluded)
            for name in ['.'] + sorted(files + dirs):
                p = Path(directory) if name == '.' else Path(directory) / name
                if p in excluded:
                    continue
                s = p.lstat()
                row = [str(p), s.st_dev, s.st_ino, s.st_mode, s.st_size,
                       s.st_mtime_ns, s.st_ctime_ns,
                       os.readlink(p) if p.is_symlink() else None]
                h.update(json.dumps(row, ensure_ascii=False).encode()); count += 1
    return dict(sha256=h.hexdigest(), entries=count)


def source_guard():
    result = {}
    for name in ('postgres19', 'apache-age-pg19-port'):
        repo = WORKSPACE / '.worktrees/pg19-graph-core' / name
        def git(*args):
            return subprocess.check_output(['git', '-C', str(repo), *args], env=ENV)
        files = git('ls-files', '-z', '--cached', '--others', '--exclude-standard').split(b'\0')
        h = hashlib.sha256(); count = 0
        for raw in sorted(set(files)):
            if not raw:
                continue
            p = repo / os.fsdecode(raw)
            if p.is_file():
                h.update(raw + b'\0' + digest(p).encode()); count += 1
        result[name] = dict(head=git('rev-parse', 'HEAD').decode().strip(),
                            status=git('status', '--porcelain', '-uall').decode(),
                            contents_sha256=h.hexdigest(), files=count)
    return result


def in_use(targets):
    prefixes = tuple(str(p) + '/' for p in targets)
    exact = {str(p) for p in targets}; busy = []
    for process in Path('/proc').iterdir():
        if not process.name.isdigit() or int(process.name) == os.getpid():
            continue
        locations = []
        try:
            for item in [process/'cwd', process/'exe', *(process/'fd').iterdir()]:
                try:
                    locations.append(os.readlink(item))
                except (FileNotFoundError, PermissionError, ProcessLookupError):
                    pass
            for line in (process/'maps').read_text().splitlines():
                parts = line.split(maxsplit=5)
                if len(parts) == 6:
                    locations.append(parts[5])
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            pass
        for location in locations:
            if location in exact or location.startswith(prefixes):
                busy.append([process.name, location])
    if busy:
        raise RuntimeError(f'Runtime is in use: {busy[:20]}')


def prepare():
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    out = ARCHIVES / (stamp + '-graph-runtime-cleanup')
    out.mkdir(mode=0o700)
    emit({'audit_directory': str(out), 'phase': 'inventory'})
    candidates = sorted(p for p in RUNTIME.iterdir() if p.is_dir() and NAME.fullmatch(p.name))
    records, summaries, owners = {}, {}, collections.defaultdict(list)
    references, referenced_runs = collections.defaultdict(set), set()
    for root in candidates:
        marker = root / ('.aa-owned.json' if root.name.startswith('dml-aa-') else '.owned.json')
        rec = dict(path=str(root), reasons=[])
        records[root.name] = rec
        if not marker.is_file() or marker.is_symlink():
            rec['reasons'].append('missing/unsafe ownership marker'); continue
        meta = json.loads(marker.read_text()); evidence = Path(meta.get('evidence', ''))
        if evidence.parent != RUNS or not evidence.is_dir():
            rec['reasons'].append('ownership evidence cannot be verified'); continue
        rec.update(marker=str(marker), marker_sha256=digest(marker), evidence=str(evidence))
        owners[evidence.name].append(root.name)
        if evidence.name not in summaries:
            summary = evidence/'summary.json'
            if summary.is_file():
                doc = json.loads(summary.read_text())
                summaries[evidence.name] = top_metadata(doc)
                del doc
            else:
                summaries[evidence.name] = dict(status='NO_SUMMARY', inputs=[], seed_output=False, stopped={})
        info = summaries[evidence.name]; rec['status'] = info['status']
        if info['status'] not in COMPLETE:
            rec['reasons'].append('failure, unfinished run, or unverified completion')
        if info['seed_output']:
            rec['reasons'].append('experiment explicitly produces a reusable seed')
        if root.name.startswith('m6-commit-dml-'):
            rec['reasons'].append('preserve DML baselines/causal evidence until outstanding DML gate closes')
        if re.search(r'auto|dml|delete|costing|stats-drift|baseline', evidence.name, re.I):
            rec['reasons'].append('preserve outstanding AUTO/cost/DML investigation data')
        for value in info['inputs']:
            for name in NAME.findall(value):
                references[name].add('input field in '+evidence.name)
            referenced_runs.update(RUN_ID.findall(value))
    # Preserve every explicit runtime dependency in current documentation and
    # scripts. Only executable/configuration files promote run IDs, so historical
    # citations do not turn every output copy into a required input.
    files = subprocess.check_output(['rg', '--files', str(TOPIC), '-g', '!执行包/evidence/**',
                                     '-g', '!**/__pycache__/**'], env=ENV).decode().splitlines()
    for filename in files + [str(WORKSPACE/'AGENTS.md')]:
        p = Path(filename)
        if 'evidence' in p.parts or p.suffix.lower() not in ('.md', '.py', '.sh', '.json', '.sql', '.yaml', '.yml'):
            continue
        data = p.read_text(errors='replace')
        for name in NAME.findall(data):
            references[name].add('explicit reference in '+str(p.relative_to(WORKSPACE)))
        if p.suffix.lower() != '.md':
            referenced_runs.update(RUN_ID.findall(data))
    for name in ('dml-aa-6__5g7iz', 'dml-aa-ody3pxi5'):
        references[name].add('current quality/sparse performance seed; mandatory retention')
    for evidence in referenced_runs:
        for name in owners.get(evidence, []):
            references[name].add('experiment consumed by a script/protocol or another run: '+evidence)
    # Even an output with no current scripted consumer can be useful for follow-up
    # work. Keep the latest completed instance(s) of EVERY experiment family.
    latest = {}
    for evidence, info in summaries.items():
        if info['status'] in COMPLETE:
            family = evidence.split('Z-', 1)[-1]
            latest[family] = max(evidence, latest.get(family, ''))
    for evidence in latest.values():
        for name in owners[evidence]:
            references[name].add('latest completed experiment family; retain for follow-up without data rebuild')
    for i, (name, rec) in enumerate(records.items()):
        rec['reasons'].extend(sorted(references.get(name, ())))
        rec['inventory'] = scan(Path(rec['path']))
        inv = rec['inventory']
        for flag in ('source_files', 'unsafe', 'occupied'):
            if inv[flag]:
                rec['reasons'].append('protected tree: '+flag)
        rec['action'] = 'KEEP' if rec['reasons'] else 'DELETE_COMPLETED_COPY'
        if i % 50 == 0:
            emit({'inventoried': i + 1, 'total': len(records)})
    in_use([Path(r['path']) for r in records.values() if r['action'] != 'KEEP'])
    plan = dict(created=now(), scope='only completed, unreferenced dml-aa runtime copies',
                builds='ALL RETAINED', installations='ALL RETAINED',
                public_documents='ALL EXISTING CONTENT RETAINED', source_files='ALL RETAINED',
                records=list(records.values()))
    save(out/'plan.json', plan)
    summary = collections.defaultdict(lambda: dict(count=0, bytes=0))
    for rec in records.values():
        summary[rec['action']]['count'] += 1
        summary[rec['action']]['bytes'] += rec['inventory']['allocated_bytes']
    save(out/'plan-summary.json', summary)
    emit({'phase': 'plan_ready', 'plan': str(out/'plan.json'), 'sha256': digest(out/'plan.json'),
          'summary': dict(summary)})


def apply(plan_path, approved_sha):
    plan_path = plan_path.absolute(); out = plan_path.parent
    if out.parent != ARCHIVES or plan_path.name != 'plan.json' or digest(plan_path) != approved_sha:
        raise RuntimeError('Not the reviewed plan and SHA256')
    plan = json.loads(plan_path.read_text())
    targets = [r for r in plan['records'] if r['action'] == 'DELETE_COMPLETED_COPY']
    if not targets or any(r['reasons'] or not Path(r['path']).name.startswith('dml-aa-') for r in targets):
        raise RuntimeError('Invalid deletion set')
    paths = [Path(r['path']) for r in targets]
    lock = (RUNTIME/'recovery.lock').open('r+')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    in_use(paths)
    for rec in targets:
        if scan(Path(rec['path'])) != rec['inventory'] or digest(Path(rec['marker'])) != rec['marker_sha256']:
            raise RuntimeError('Changed since review: '+rec['path'])
    # Guard all retained runtime trees, all builds/installations, all existing
    # public documents and source. No service is started for this operation.
    retained = [Path(r['path']) for r in plan['records'] if r['action'] == 'KEEP']
    other_runtime = [p for p in (WORKSPACE/'.runtime').iterdir() if p.is_dir() and p != RUNTIME]
    guarded = [WORKSPACE/'.build', WORKSPACE/'.install', WORKSPACE/'公共文档',
               WORKSPACE/'graph-bench-data', *other_runtime, *retained]
    emit({'phase': 'protected_assets_before'})
    guards_before = metadata_guard(guarded, {out})
    sources_before = source_guard()
    free_before = shutil.disk_usage(RUNTIME)
    save(out/'protected-before.json', dict(metadata=guards_before, source=sources_before,
                                         filesystem=free_before._asdict()))
    # Save all non-database runtime files and configuration. Raw relation/WAL
    # files are deliberately not duplicated; these are reproducible test copies.
    archive = out/'runtime-logs-configs.tar.gz'
    with tarfile.open(archive, 'x:gz', compresslevel=1) as tar:
        for root in paths:
            for directory, dirs, files in os.walk(root):
                for name in files:
                    p = Path(directory)/name
                    rel = p.relative_to(root)
                    if 'data' not in rel.parts or name in ('postgresql.conf', 'postgresql.auto.conf',
                                                          'pg_hba.conf', 'pg_ident.conf', 'postmaster.opts'):
                        tar.add(p, arcname=str(p.relative_to(RUNTIME)), recursive=False)
    with tarfile.open(archive) as tar:
        archive_members = len(tar.getmembers())
        for member in tar:
            if member.isfile():
                source = tar.extractfile(member)
                while source.read(1024 * 1024):
                    pass
    save(out/'archive.json', dict(path=str(archive), sha256=digest(archive), members=archive_members,
                                contains_database_or_wal=False))
    # Re-check the complete exact set after archiving, immediately before any removal.
    in_use(paths)
    for rec in targets:
        if scan(Path(rec['path'])) != rec['inventory']:
            raise RuntimeError('Changed before deletion: '+rec['path'])
    report = dict(status='REMOVING', started=now(), approved_plan_sha256=approved_sha,
                  deleted=[], protected_before=guards_before, free_before=free_before.free)
    save(out/'summary.json', report)
    for number, rec in enumerate(targets, 1):
        target = Path(rec['path'])
        if target.parent != RUNTIME or target.resolve() != target or not NAME.fullmatch(target.name):
            raise RuntimeError('Unsafe exact target')
        shutil.rmtree(target)
        report['deleted'].append(rec['path']); save(out/'summary.json', report)
        if number % 20 == 0:
            emit({'deleted': number, 'total': len(targets)})
    emit({'phase': 'protected_assets_after'})
    guards_after = metadata_guard(guarded, {out}); sources_after = source_guard()
    unchanged = guards_before == guards_after and sources_before == sources_after
    free_after = shutil.disk_usage(RUNTIME)
    report.update(status='CLEANUP_COMPLETE' if unchanged else 'PROTECTION_CHECK_FAILED',
                  finished=now(), protected_after=guards_after, protected_assets_unchanged=unchanged,
                  free_after=free_after.free, filesystem_free_delta_bytes=free_after.free-free_before.free,
                  removed_allocated_bytes=sum(r['inventory']['allocated_bytes'] for r in targets),
                  retained_runtime_directories=len(retained), all_builds_and_installs_retained=True,
                  all_public_documents_retained=True, source_guards_unchanged=sources_before==sources_after)
    save(out/'summary.json', report)
    save(out/'protected-after.json', dict(metadata=guards_after, source=sources_after,
                                        filesystem=free_after._asdict()))
    emit({k: v for k, v in report.items() if k not in ('deleted', 'protected_before', 'protected_after')})
    if not unchanged:
        raise RuntimeError('Protected asset guard mismatch; inspect evidence')


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--apply', type=Path)
    ap.add_argument('--approved-sha256')
    args = ap.parse_args()
    if args.apply:
        if not args.approved_sha256:
            ap.error('--apply requires --approved-sha256')
        apply(args.apply, args.approved_sha256)
    else:
        prepare()
