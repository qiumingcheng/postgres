#!/usr/bin/env python3
"""Shared paths and evidence helpers; Python standard library only."""
from __future__ import annotations
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

PACKAGE = Path(__file__).resolve().parents[1]
WORKSPACE = PACKAGE.parents[2]
TOPIC = PACKAGE.parent
DESIGN = WORKSPACE / '公共文档/02. PostgreSQL19原生多跳图执行与AGE适配详细设计.md'

def now():
    return dt.datetime.now(dt.timezone.utc).isoformat()

def sha(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()

def read(path):
    return json.loads(Path(path).read_text())

def write(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', dir=path.parent, delete=False) as f:
        json.dump(value, f, ensure_ascii=False, indent=2)
        f.write('\n')
        tmp = Path(f.name)
    os.replace(tmp, path)

def capture(argv, cwd=None, binary=False):
    return subprocess.check_output([str(x) for x in argv], cwd=cwd, text=not binary)

def identity(repo):
    repo = Path(repo)
    patch = capture(['git', 'diff', '--binary', 'HEAD', '--'], repo, binary=True)
    untracked = capture(['git', 'ls-files', '--others', '--exclude-standard', '-z'], repo).split('\0')
    return {
        'head': capture(['git', 'rev-parse', 'HEAD'], repo).strip(),
        'patch_sha256': hashlib.sha256(patch).hexdigest(),
        'untracked': {p: sha(repo / p) for p in sorted(untracked) if p},
    }

def run_dir(label):
    tag = dt.datetime.now(dt.timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    path = PACKAGE / 'evidence/runs' / (tag + '-' + label)
    path.mkdir(parents=True)
    return path

def run_logged(argv, directory, label, cwd=None, env=None, timeout=3600):
    argv = [str(x) for x in argv]
    log = Path(directory) / (label + '.log')
    started = now()
    with log.open('w') as out:
        try:
            result = subprocess.run(argv, cwd=cwd, env=env, stdout=out,
                                    stderr=subprocess.STDOUT, timeout=timeout)
            code = result.returncode
        except subprocess.TimeoutExpired:
            code = 124
            out.write('\nTIMEOUT\n')
    record = {'argv': argv, 'cwd': str(cwd) if cwd else None,
              'started': started, 'finished': now(), 'exit_code': code,
              'log': str(log.relative_to(PACKAGE)), 'sha256': sha(log)}
    write(log.with_suffix('.json'), record)
    print(f'{label}: exit={code}; {log}', flush=True)
    if code:
        print(log.read_text(errors='replace')[-5000:], flush=True)
        raise RuntimeError(f'{label} failed ({code}); see {log}')
    return record
