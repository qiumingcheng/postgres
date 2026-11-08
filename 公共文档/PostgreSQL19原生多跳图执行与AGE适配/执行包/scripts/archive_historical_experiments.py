#!/usr/bin/env python3
"""Archive the explicitly selected obsolete graph experiments before removal.

No database data, current source, current installation, or benchmark evidence is
removed. Full Git directories, dirty/untracked files and old binaries are kept
in a self-contained archive. Removal requires a successful extraction rehearsal.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import time

ROOT = Path('/home/qiumc/data/code/cpp')
PACKAGE = ROOT / '公共文档/PostgreSQL19原生多跳图执行与AGE适配/执行包'
ARCHIVES = PACKAGE / 'evidence/archives'
VARIANTS = (
    'pgq-adj-tree', 'pgq-adj-tree-safe', 'pgq-age-fusion', 'pgq-age-memory',
    'pgq-cardinality', 'pgq-cover', 'pgq-hints', 'pgq-independent',
    'pgq-leaf-isolated', 'pgq-leafprep-recovered', 'pgq-pipeline',
    'pgq-pointer', 'pgq-prepare-isolated', 'pgq-probe-batch', 'pgq-range',
    'pgq-reuse', 'pgq-routebatch', 'pgq-routebatch-recovered', 'pgq-seek',
    'pgq-sets',
)
TARGETS = tuple(f'{parent}/{name}' for parent in (
    'graph-experiments', '.build/pg19-graph-core', '.install/pg19-graph-core'
) for name in VARIANTS)
PROTECTED = (
    '.worktrees/pg19-graph-core/postgres19',
    '.worktrees/pg19-graph-core/apache-age-pg19-port',
    '.build/pg19-graph-core/pgq-integrated',
    '.install/pg19-graph-core/pgq-integrated',
    'graph-experiments/tools',
)
ENV = dict(os.environ, GIT_OPTIONAL_LOCKS='0')


def emit(message):
    print(time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), message, flush=True)


def run(args, cwd=None):
    return subprocess.check_output(args, cwd=cwd, env=ENV)


def save(path, value):
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n')
    os.replace(temporary, path)


def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as source:
        while block := source.read(1024 * 1024):
            h.update(block)
    return h.hexdigest()


def validate_targets(base):
    for relative in TARGETS:
        path = base / relative
        if not path.is_dir() or path.is_symlink() or path.resolve() != path:
            raise RuntimeError(f'Not a real, existing, exact target: {path}')
        if os.path.ismount(path):
            raise RuntimeError(f'Refusing mount: {path}')


def inventory(base, relatives, progress=False, reject_database_data=True):
    files = {}
    inodes = {}
    cache = {}

    def add(path):
        relative = str(path.relative_to(base))
        s = path.lstat()
        mode = stat.S_IMODE(s.st_mode)
        if stat.S_ISDIR(s.st_mode):
            if path != base and os.path.ismount(path):
                raise RuntimeError(f'Nested mount: {path}')
            files[relative] = ['dir', mode]
        elif stat.S_ISLNK(s.st_mode):
            files[relative] = ['symlink', mode, os.readlink(path)]
        elif stat.S_ISREG(s.st_mode):
            if reject_database_data and path.name in ('PG_VERSION', 'postmaster.pid'):
                raise RuntimeError(f'Possible database data, refusing: {path}')
            key = (s.st_dev, s.st_ino)
            identity = (key, s.st_size, s.st_mtime_ns, s.st_ctime_ns)
            if identity not in cache:
                cache[identity] = sha(path)
            files[relative] = ['file', mode, s.st_size, cache[identity]]
            entry = inodes.setdefault(key, [s.st_blocks * 512, s.st_nlink, 0])
            entry[2] += 1
        else:
            raise RuntimeError(f'Special file cannot be archived safely: {path}')

    for relative in relatives:
        top = base / relative
        add(top)
        for current, dirs, names in os.walk(top, followlinks=False):
            dirs.sort()
            for name in sorted(dirs + names):
                add(Path(current) / name)
        if progress:
            emit(f'Inventoried {relative}')
    allocated = sum(v[0] for v in inodes.values())
    reclaimable = sum(v[0] for v in inodes.values() if v[1] == v[2])
    return files, {'unique_file_allocated_bytes': allocated,
                   'file_allocated_bytes_without_external_hardlinks': reclaimable,
                   'entries': len(files)}


def digest_inventory(files):
    return hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest()


def repository_info(repo):
    if not (repo / '.git').is_dir() or (repo / '.git').is_symlink():
        raise RuntimeError(f'External Git directory: {repo}')
    alternates = repo / '.git/objects/info/alternates'
    if alternates.exists() and alternates.read_text().strip():
        raise RuntimeError(f'External object dependency: {repo}')
    def git(*args):
        return run(['git', *args], cwd=repo)
    return {
        'head': git('rev-parse', 'HEAD').decode().strip(),
        'status': git('status', '--porcelain=v1', '--untracked-files=all').decode(),
        'patch': git('diff', '--binary', '--full-index', 'HEAD'),
        'cached_patch': git('diff', '--cached', '--binary', '--full-index'),
        'unstaged_patch': git('diff', '--binary', '--full-index'),
    }


def check_in_use():
    targets = tuple(str(ROOT / p) for p in TARGETS)
    active = []
    for process in Path('/proc').iterdir():
        if not process.name.isdigit() or int(process.name) == os.getpid():
            continue
        locations = []
        for name in ('cwd', 'exe'):
            try:
                locations.append(os.readlink(process / name))
            except (FileNotFoundError, PermissionError, ProcessLookupError):
                pass
        try:
            for fd in (process / 'fd').iterdir():
                try:
                    locations.append(os.readlink(fd))
                except (FileNotFoundError, PermissionError, ProcessLookupError):
                    pass
            for line in (process / 'maps').read_text().splitlines():
                fields = line.split(maxsplit=5)
                if len(fields) == 6:
                    locations.append(fields[5])
        except (FileNotFoundError, PermissionError, ProcessLookupError):
            pass
        for location in locations:
            if any(location == p or location.startswith(p + '/') for p in targets):
                active.append([process.name, location])
    if active:
        raise RuntimeError(f'Targets are in use: {active[:20]}')


def protected_digest():
    # The retained integrated build has an initdb template. Hash it, but do not
    # apply deletion-target checks to data that is expressly protected.
    return digest_inventory(inventory(ROOT, PROTECTED, reject_database_data=False)[0])


def archive():
    validate_targets(ROOT)
    check_in_use()
    ARCHIVES.mkdir(parents=True, exist_ok=True)
    out = ARCHIVES / time.strftime('%Y%m%dT%H%M%SZ-graph-history', time.gmtime())
    out.mkdir(mode=0o700)
    emit(f'Archive directory: {out}')
    report = {'state': 'PREPARING', 'root': str(ROOT), 'targets': list(TARGETS),
              'variants': list(VARIANTS), 'protected': list(PROTECTED),
              'repositories': {}, 'deleted': [], 'created_utc': time.strftime(
                  '%Y-%m-%dT%H:%M:%SZ', time.gmtime())}
    save(out / 'summary.json', report)
    report['protected_digest_before'] = protected_digest()
    (out / 'patches').mkdir()
    for name in VARIANTS:
        for child in sorted((ROOT / 'graph-experiments' / name).iterdir()):
            if not child.is_dir() or not (child / '.git').exists():
                continue
            relative = str(child.relative_to(ROOT))
            info = repository_info(child)
            for key in ('patch', 'cached_patch', 'unstaged_patch'):
                data = info.pop(key)
                filename = f'{name}--{child.name}--{key}.patch'
                (out / 'patches' / filename).write_bytes(data)
                info[key + '_sha256'] = hashlib.sha256(data).hexdigest()
                info[key + '_file'] = 'patches/' + filename
            report['repositories'][relative] = info
    original, report['original_inventory_stats'] = inventory(ROOT, TARGETS, True)
    save(out / 'file_manifest.json', original)
    report['file_manifest_sha256'] = sha(out / 'file_manifest.json')
    report['state'] = 'COMPRESSING'
    save(out / 'summary.json', report)
    packed = out / 'historical-experiments.tar.zst'
    emit('Compressing complete sources, Git metadata, old builds and installations')
    subprocess.run(['tar', '--sort=name', '-I', 'zstd -T2 -8 --long=29',
                    '-cf', str(packed), '-C', str(ROOT), '--', *TARGETS], check=True)
    report['archive_sha256'] = sha(packed)
    report['archive_bytes'] = packed.stat().st_size
    subprocess.run(['zstd', '-q', '-t', '--long=29', str(packed)], check=True)
    report['state'] = 'ARCHIVE_WRITTEN'
    save(out / 'summary.json', report)
    verify_existing(out)


def verify_existing(out):
    out = out.resolve(strict=True)
    if out.parent != ARCHIVES:
        raise RuntimeError('Not an archive under the designated evidence directory')
    report = json.loads((out / 'summary.json').read_text())
    if report['state'] not in ('ARCHIVE_WRITTEN', 'RESTORE_REHEARSAL'):
        raise RuntimeError('Unexpected state for extraction verification')
    packed = out / 'historical-experiments.tar.zst'
    if sha(packed) != report['archive_sha256']:
        raise RuntimeError('Archive checksum changed')
    if sha(out / 'file_manifest.json') != report['file_manifest_sha256']:
        raise RuntimeError('Manifest checksum changed')
    original = json.loads((out / 'file_manifest.json').read_text())
    report['state'] = 'RESTORE_REHEARSAL'
    previous = report.get('verification_temporary_directory')
    if previous:
        temporary = Path(previous)
        if (temporary.parent != Path('/tmp') or
                not temporary.name.startswith('graph-history-verify-') or
                temporary.resolve(strict=True) != temporary or
                temporary.stat().st_uid != os.getuid()):
            raise RuntimeError('Unexpected existing verification directory')
    else:
        temporary = Path(run(['mktemp', '-d', '/tmp/graph-history-verify-XXXXXXXX']).decode().strip())
    report['verification_temporary_directory'] = str(temporary)
    save(out / 'summary.json', report)
    emit(f'Extracting for complete recovery verification: {temporary}')
    subprocess.run(['tar', '--same-permissions', '-I', 'zstd -d --long=29', '-xf', str(packed),
                    '-C', str(temporary)], check=True)
    restored, _ = inventory(temporary, TARGETS, True)
    if restored != original:
        changed = [p for p in original.keys() | restored.keys()
                   if original.get(p) != restored.get(p)]
        raise RuntimeError(f'Restored file mismatch: {changed[:30]}')
    for relative, expected in report['repositories'].items():
        repo = temporary / relative
        actual = repository_info(repo)
        if actual['head'] != expected['head'] or actual['status'] != expected['status']:
            raise RuntimeError(f'Restored Git identity/status mismatch: {relative}')
        for key in ('patch', 'cached_patch', 'unstaged_patch'):
            if hashlib.sha256(actual[key]).hexdigest() != expected[key + '_sha256']:
                raise RuntimeError(f'Restored patch mismatch: {relative}/{key}')
        checked = subprocess.run(['git', 'fsck', '--connectivity-only', '--no-dangling'],
                                 cwd=repo, env=ENV, capture_output=True, text=True)
        (out / f'{repo.parent.name}--{repo.name}--fsck.log').write_text(
            checked.stdout + checked.stderr)
        if checked.returncode:
            raise RuntimeError(f'Restored Git connectivity failed: {relative}')
        emit(f'Restored Git verified: {relative}')
    # Remove only the temporary extraction created above, never an original tree.
    if temporary.parent != Path('/tmp') or not temporary.name.startswith('graph-history-verify-'):
        raise RuntimeError('Unexpected verification directory')
    shutil.rmtree(temporary)
    if protected_digest() != report['protected_digest_before']:
        raise RuntimeError('Protected paths changed during archiving')
    report['state'] = 'VERIFIED_NOT_DELETED'
    report['restore_rehearsal'] = 'PASS: contents, modes, symlinks, Git HEAD/status/diffs/connectivity'
    report['verification_temporary_removed'] = True
    save(out / 'summary.json', report)
    emit(f'VERIFIED_NOT_DELETED: {out}')


def remove_verified(out):
    out = out.resolve(strict=True)
    if out.parent != ARCHIVES:
        raise RuntimeError('Not an archive under the designated evidence directory')
    report = json.loads((out / 'summary.json').read_text())
    if report['state'] != 'VERIFIED_NOT_DELETED' or report['targets'] != list(TARGETS):
        raise RuntimeError('Archive is not verified or exact target list differs')
    packed = out / 'historical-experiments.tar.zst'
    if sha(packed) != report['archive_sha256']:
        raise RuntimeError('Archive checksum changed')
    if sha(out / 'file_manifest.json') != report['file_manifest_sha256']:
        raise RuntimeError('Manifest checksum changed')
    original = json.loads((out / 'file_manifest.json').read_text())
    validate_targets(ROOT)
    if inventory(ROOT, TARGETS)[0] != original:
        raise RuntimeError('Targets changed after archiving; do not delete')
    if protected_digest() != report['protected_digest_before']:
        raise RuntimeError('Protected paths changed; do not delete')
    check_in_use()
    report['filesystem_free_bytes_before_removal'] = shutil.disk_usage(ROOT).free
    report['state'] = 'REMOVING_VERIFIED_TARGETS'
    save(out / 'summary.json', report)
    for relative in TARGETS:
        # Targets are a fixed 60-path allowlist, not a caller-supplied directory.
        target = ROOT / relative
        if target.resolve(strict=True) != target or target.is_symlink():
            raise RuntimeError(f'Target identity changed: {target}')
        shutil.rmtree(target)
        report['deleted'].append(relative)
        save(out / 'summary.json', report)
        emit(f'Removed archived target: {relative}')
    report['protected_digest_after'] = protected_digest()
    if report['protected_digest_after'] != report['protected_digest_before']:
        raise RuntimeError('Post-cleanup protected contents differ')
    if any((ROOT / p).exists() for p in TARGETS):
        raise RuntimeError('Some selected targets remain')
    report['filesystem_free_bytes_after_removal'] = shutil.disk_usage(ROOT).free
    report['filesystem_free_delta_bytes'] = (report['filesystem_free_bytes_after_removal'] -
                                              report['filesystem_free_bytes_before_removal'])
    report['state'] = 'CLEANUP_COMPLETE'
    report['completed_utc'] = time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())
    save(out / 'summary.json', report)
    emit(f'CLEANUP_COMPLETE: {out}')


def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_mutually_exclusive_group()
    actions.add_argument('--verify-existing', type=Path,
                         help='Resume a recovery rehearsal without rebuilding the archive')
    actions.add_argument('--remove-verified', type=Path,
                        help='Delete the fixed targets only after verified archival')
    args = parser.parse_args()
    if args.remove_verified:
        remove_verified(args.remove_verified)
    elif args.verify_existing:
        verify_existing(args.verify_existing)
    else:
        archive()


if __name__ == '__main__':
    main()
