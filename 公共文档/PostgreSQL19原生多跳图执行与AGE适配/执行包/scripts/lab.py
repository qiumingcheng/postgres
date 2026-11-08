#!/usr/bin/env python3
"""Isolated, local-only PG19/AGE baseline and candidate laboratory."""
from __future__ import annotations
import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import isolation
from common import PACKAGE, WORKSPACE, capture, identity, now, read, run_dir, run_logged, sha, write

LOCK = PACKAGE / 'specs/source_lock.json'
BASE_HEADS = {'postgres19': '7873db5369b967dd53984df34fd800003410d04f',
              'apache-age-pg19-port': '4d9b250663424ad9c5e2405f6e2d821182eaaef4'}
BTREE_RELEASE_HEAD = '7ff975e259ce4f256d5645431e25713b0c336e2a'
BTREE_VARIANTS = ('btree-release', 'btree-opt', 'btree-reuse', 'btree-compact', 'btree-startup', 'btree-deform', 'btree-refview', 'btree-array', 'btree-tail', 'btree-factor', 'btree-factor-batch', 'btree-anchor-stats', 'btree-suffix')
BTREE_OPT_MANIFEST = PACKAGE / 'golden/G1-080/btree_begin_opt_v1.json'
BTREE_REUSE_MANIFEST = PACKAGE / 'golden/G1-080/btree_reuse_opt_v2.json'
BTREE_COMPACT_MANIFEST = PACKAGE / 'golden/G1-080/btree_compact_opt_v2.json'
BTREE_STARTUP_MANIFEST = PACKAGE / 'golden/G1-080/btree_startup_opt_v2.json'
BTREE_DEFORM_MANIFEST = PACKAGE / 'golden/G1-080/btree_deform_opt_v1.json'
BTREE_REFVIEW_MANIFEST = PACKAGE / 'golden/G1-080/btree_refview_opt_v3.json'
BTREE_ARRAY_MANIFEST = PACKAGE / 'golden/G1-080/btree_array_opt_v2.json'
BTREE_TAIL_MANIFEST = PACKAGE / 'golden/G1-080/btree_tail_opt_v2.json'
BTREE_FACTOR_MANIFEST = PACKAGE / 'golden/G1-080/btree_factor_opt_v2.json'
BTREE_FACTOR_BATCH_MANIFEST = PACKAGE / 'golden/G1-080/btree_factor_opt_v3.json'
BTREE_ANCHOR_STATS_MANIFEST = PACKAGE / 'golden/G1-080/btree_anchor_opt_v1.json'
BTREE_SUFFIX_MANIFEST = PACKAGE / 'golden/G1-080/btree_suffix_opt_v1.json'
BTREE_DIRECT_MANIFEST = PACKAGE / 'golden/G1-080/btree_direct_opt_v2.json'
BTREE_VARIANTS += ('btree-direct',)
BTREE_THROUGHPUT_MANIFEST = PACKAGE / 'golden/G1-080/btree_throughput_opt_v1.json'
BTREE_VARIANTS += ('btree-throughput',)
BTREE_FUSED_MANIFEST = PACKAGE / 'golden/G1-080/btree_fused_opt_v1.json'
BTREE_VARIANTS += ('btree-fused',)
BTREE_CURSOR_MANIFEST = PACKAGE / 'golden/G1-080/btree_cursor_opt_v1.json'
BTREE_VARIANTS += ('btree-cursor',)
BTREE_LEASE_MANIFEST = PACKAGE / 'golden/G1-080/btree_lease_opt_v2.json'
BTREE_VARIANTS += ('btree-lease',)
BTREE_LAYOUT_MANIFEST = PACKAGE / 'golden/G1-080/btree_layout_opt_v1.json'
BTREE_VARIANTS += ('btree-layout',)
BTREE_SINGLETON_MANIFEST = PACKAGE / 'golden/G1-080/btree_singleton_opt_v1.json'
BTREE_VARIANTS += ('btree-singleton',)
BTREE_READER_MANIFEST = PACKAGE / 'golden/G1-080/btree_reader_opt_v1.json'
BTREE_VARIANTS += ('btree-reader',)
BTREE_PREFIX_MANIFEST = PACKAGE / 'golden/G1-080/btree_prefix_opt_v1.json'
BTREE_VARIANTS += ('btree-prefix',)
BTREE_DELTA_MANIFEST = PACKAGE / 'golden/G1-080/btree_delta_opt_v1.json'
BTREE_VARIANTS += ('btree-delta',)
BTREE_SINK_MANIFEST = PACKAGE / 'golden/G1-080/btree_sink_opt_v1.json'
BTREE_VARIANTS += ('btree-sink',)
BTREE_JOINT_MANIFEST = PACKAGE / 'golden/G1-080/btree_joint_opt_v1.json'
BTREE_VARIANTS += ('btree-joint',)
BTREE_SETUP_MANIFEST = PACKAGE / 'golden/G1-080/btree_setup_opt_v1.json'
BTREE_VARIANTS += ('btree-setup',)
BTREE_RANK_MANIFEST = PACKAGE / 'golden/G1-080/btree_rank_opt_v1.json'
BTREE_VARIANTS += ('btree-rank',)
BTREE_EMIT_MANIFEST = PACKAGE / 'golden/G1-080/btree_emit_opt_v1.json'
BTREE_VARIANTS += ('btree-emit',)
BTREE_PRODUCT_MANIFEST = PACKAGE / 'golden/G1-080/btree_product_opt_v1.json'
BTREE_VARIANTS += ('btree-product',)
BTREE_HEAPKEYS_MANIFEST = PACKAGE / 'golden/G1-080/btree_heapkeys_opt_v1.json'
BTREE_VARIANTS += ('btree-heapkeys',)
BTREE_WINDOW_MANIFEST = PACKAGE / 'golden/G1-080/btree_window_opt_v1.json'
BTREE_VARIANTS += ('btree-window',)
BTREE_EOF_MANIFEST = PACKAGE / 'golden/G1-080/btree_eof_opt_v1.json'
BTREE_VARIANTS += ('btree-eof',)
ADJ_VARIANTS = ('adj-eof', 'adj-factor', 'adj-scale', 'adj-inline', 'adj-create', 'adj-set', 'adj-refcache', 'adj-layout', 'adj-rootwal', 'adj-vacstats', 'adj-frontier', 'adj-allocmap', 'adj-frontier-reuse', 'adj-chain', 'pgq-equality', 'pgq-equality-safe', 'pgq-hints', 'pgq-sets')
ADJ_VARIANTS += ('pgq-relational',)
ADJ_VARIANTS += ('pgq-independent',)
ADJ_VARIANTS += ('pgq-cardinality',)
ADJ_VARIANTS += ('pgq-probe-batch',)
ADJ_VARIANTS += ('pgq-cover',)
ADJ_VARIANTS += ('pgq-pipeline',)
ADJ_VARIANTS += ('pgq-range',)
ADJ_VARIANTS += ('pgq-reuse',)
ADJ_VARIANTS += ('pgq-pointer',)
ADJ_VARIANTS += ('pgq-seek',)
ADJ_VARIANTS += ('pgq-routebatch',)
ADJ_VARIANTS += ('pgq-leafprep',)
ADJ_VARIANTS += ('pgq-leafprep-recovered',)
ADJ_VARIANTS += ('pgq-routebatch-recovered',)
ADJ_VARIANTS += ('pgq-leaf-isolated', 'pgq-prepare-isolated')
ADJ_VARIANTS += ('pgq-adj-tree',)
ADJ_VARIANTS += ('pgq-adj-tree-safe',)
ADJ_VARIANTS += ('pgq-age-memory',)
ADJ_VARIANTS += ('pgq-age-fusion',)
VARIANT_PORTS = {'baseline': (55449, 55459), 'candidate': (55450, 55460),
                 'btree-release': (55471, 55481), 'btree-opt': (55472, 55482),
                 'btree-reuse': (55473, 55485), 'btree-compact': (55474, 55489),
                 'btree-startup': (55475, 55491), 'btree-deform': (55476, 55493),
                 'btree-refview': (55477, 55495), 'btree-array': (55478, 55497),
                 'btree-tail': (55479, 55499), 'btree-factor': (55501, 55502),
                 'btree-factor-batch': (55504, 55505), 'btree-anchor-stats': (55507, 55508),
                 'btree-suffix': (55510, 55511), 'btree-direct': (55513, 55514),
                 'btree-throughput': (55516, 55517), 'btree-fused': (55519, 55520),
                 'btree-cursor': (55522, 55523), 'btree-lease': (55525, 55526),
                 'btree-layout': (55528, 55529), 'btree-singleton': (55531, 55532),
                 'btree-reader': (55534, 55535), 'btree-prefix': (55537, 55538),
                 'btree-delta': (55540, 55541), 'btree-sink': (55543, 55544),
                 'btree-joint': (55546, 55547), 'btree-setup': (55549, 55550),
                 'btree-rank': (55552, 55553), 'btree-emit': (55555, 55556),
                 'btree-product': (55558, 55559), 'btree-heapkeys': (55561, 55562),
                 'btree-window': (55564, 55565), 'btree-eof': (55567, 55568),
                 'adj-eof': (55570, 55571), 'adj-factor': (55573, 55574),
                 'adj-scale': (55576, 55577), 'adj-inline': (55579, 55580),
                 'adj-create': (55585, 55586), 'adj-set': (55588, 55589),
                 'adj-refcache': (55591, 55592), 'adj-layout': (55596, 55597),
                 'adj-rootwal': (55600, 55601), 'adj-vacstats': (55603, 55604),
                 'adj-frontier': (55606, 55607), 'adj-allocmap': (55609, 55610),
                 'adj-frontier-reuse': (55612, 55613), 'adj-chain': (55615, 55616),
                 'pgq-equality': (55618, 55619), 'pgq-equality-safe': (55621, 55622),
                 'pgq-hints': (55624, 55625), 'pgq-sets': (55627, 55628),
                 'pgq-relational': (55630, 55631), 'pgq-independent': (55636, 55637),
                 'pgq-cardinality': (55638, 55639), 'pgq-probe-batch': (55640, 55641),
                 'pgq-cover': (55642, 55643), 'pgq-pipeline': (55644, 55645), 'pgq-range': (55646, 55647), 'pgq-reuse': (55648, 55649), 'pgq-pointer': (55650, 55651), 'pgq-seek': (55652, 55653), 'pgq-routebatch': (55654, 55655), 'pgq-leafprep': (55656, 55657)}

VARIANT_PORTS['pgq-leafprep-recovered'] = (55658, 55659)
VARIANT_PORTS['pgq-routebatch-recovered'] = (55660, 55661)
VARIANT_PORTS['pgq-leaf-isolated'] = (55662, 55663)
VARIANT_PORTS['pgq-adj-tree'] = (55666, 55667)
VARIANT_PORTS['pgq-adj-tree-safe'] = (55668, 55669)
VARIANT_PORTS['pgq-age-memory'] = (55670, 55671)
VARIANT_PORTS['pgq-age-fusion'] = (55672, 55673)
VARIANT_PORTS['pgq-integrated'] = (55674, 55675)
VARIANT_PORTS['pgq-prepare-isolated'] = (55664, 55665)

def paths(variant):
    if variant not in VARIANT_PORTS:
        raise ValueError('unknown variant')
    return {'build': WORKSPACE / '.build/pg19-graph-core' / variant,
            'install': WORKSPACE / '.install/pg19-graph-core' / variant,
            'runtime': WORKSPACE / '.runtime/pg19-graph-core' / variant,
            'port': VARIANT_PORTS[variant][0],
            'regress_port': VARIANT_PORTS[variant][1]}

def source_root(variant):
    # Recovery sources are durable user work, not disposable build products.
    if variant in isolation.DURABLE_SOURCE_VARIANTS:
        return WORKSPACE / 'graph-experiments' / variant
    return (paths(variant)['build'] / 'src' if variant in ('baseline', *BTREE_VARIANTS, *ADJ_VARIANTS)
            else WORKSPACE / '.worktrees/pg19-graph-core')

def clean_env():
    env = os.environ.copy()
    for k in list(env):
        if k.startswith(('PG', 'LD_', 'GIT_')) or k in (
                'GAUSSHOME', 'MAKEFLAGS', 'MFLAGS', 'MAKELEVEL', 'MAKEFILES', 'DESTDIR',
                'CFLAGS', 'CPPFLAGS', 'CXXFLAGS', 'LDFLAGS', 'LIBS', 'CC', 'CXX', 'CONFIG_SITE',
                'CPATH', 'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH', 'LIBRARY_PATH',
                'PKG_CONFIG', 'PKG_CONFIG_PATH', 'PKG_CONFIG_LIBDIR', 'PKG_CONFIG_SYSROOT_DIR'):
            env.pop(k, None)
    # Use the same system toolchain as the frozen lab, even if an old integration
    # install, virtual environment or compiler wrapper is first in the caller's PATH.
    env['PATH'] = '/usr/bin:/bin'
    env.update({'LC_ALL': 'C', 'LANG': 'C'})
    return env

def guard_layout(variant):
    return isolation.layout(WORKSPACE, variant, paths(variant), source_root(variant))

def guard_isolation(variant, require_install=False, online=False):
    report = isolation.offline(WORKSPACE, variant, paths(variant), source_root(variant),
                               clean_env(), require_install=require_install or online)
    if online:
        report['runtime'] = isolation.online(paths(variant), lambda query, db: sql(variant, query, db),
            probe_library='test_graph_core' if variant in (*BTREE_VARIANTS, *ADJ_VARIANTS) else 'age')
    return report

def audit_isolation(variant, online=False):
    out = run_dir('isolation-' + variant + ('-online' if online else '-offline'))
    try:
        report = guard_isolation(variant, online=online)
    except (RuntimeError, OSError, subprocess.SubprocessError) as error:
        write(out / 'summary.json', {'status': 'FAIL', 'variant': variant, 'error': str(error), 'finished': now()})
        raise
    report['finished'] = now()
    write(out / 'summary.json', report)
    print(json.dumps(report, ensure_ascii=False, indent=2))
    print(out)
    return report

def freeze():
    if LOCK.exists():
        verify_sources()
        print('Source lock already exists and matches; not overwritten.')
        return
    records = {}
    for name, expected in BASE_HEADS.items():
        repo = WORKSPACE / name
        ident = identity(repo)
        if ident['head'] != expected:
            raise RuntimeError(f'{name}: unexpected HEAD')
        if ident['untracked']:
            raise RuntimeError(f'{name}: untracked files require explicit archival before freeze')
        patch = capture(['git', 'diff', '--binary', 'HEAD', '--'], repo, binary=True)
        target = PACKAGE / 'baseline' / (name + '.patch')
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(patch)
        status = capture(['git', 'status', '--porcelain=v1'], repo)
        (target.parent / (name + '.status.txt')).write_text(status)
        records[name] = dict(ident, path=name, patch=str(target.relative_to(PACKAGE)),
                             modified_count=len(status.splitlines()), status=status)
    write(LOCK, {'version': 1, 'captured_at': now(), 'sources': records,
                 'configure': ['--enable-debug', '--enable-cassert', '--enable-depend'],
                 'cflags': '-O2 -g', 'baseline_is_product_pass': False})
    print(LOCK)

def verify_sources():
    lock = read(LOCK)
    for name, spec in lock['sources'].items():
        if sha(PACKAGE / spec['patch']) != spec['patch_sha256']:
            raise RuntimeError(f'{name}: archived patch digest mismatch')
        actual = identity(WORKSPACE / spec['path'])
        if any(actual[k] != spec[k] for k in ('head', 'patch_sha256', 'untracked')):
            raise RuntimeError(f'{name}: protected original worktree changed; do not reset it')
    return lock

def preflight():
    lock = verify_sources()
    checks = {variant: guard_isolation(variant) for variant in ('baseline', 'candidate')}
    required = ['git', 'gcc', 'g++', 'make', 'bison', 'flex', 'perl', 'pkg-config', 'python3']
    report = {'checked_at': now(), 'platform': platform.platform(), 'cpu_count': os.cpu_count(),
              'memory_kb': {s.split(':')[0]: int(s.split()[1]) for s in Path('/proc/meminfo').read_text().splitlines() if s.startswith(('MemTotal:', 'MemAvailable:'))},
              'disk_free_bytes': shutil.disk_usage(WORKSPACE).free,
              'tools': {t: shutil.which(t, path=clean_env()['PATH']) for t in required}, 'source_lock_sha256': sha(LOCK),
              'isolation': checks,
              'sources': {n: s['head'] for n, s in lock['sources'].items()},
              'runtime': {v: {k: str(x) for k, x in paths(v).items()} for v in ('baseline', 'candidate')},
              'product_gate': 'NOT_RUN'}
    print(json.dumps(report, ensure_ascii=False, indent=2))
    if not all(report['tools'].values()):
        raise RuntimeError('missing build tool')
    return report

def prepare(variant):
    if variant in ADJ_VARIANTS:
        from adj_lab import prepare as prepare_adj
        return prepare_adj(variant)
    if variant in BTREE_VARIANTS:
        return prepare_btree_release(variant)
    guard_isolation(variant)
    lock = verify_sources()
    p = paths(variant)
    evidence = run_dir('prepare-' + variant)
    for name, spec in lock['sources'].items():
        target = source_root(variant) / name
        marker = target / '.graph-baseline.json'
        if target.exists():
            if not marker.exists() or read(marker)['source_lock_sha256'] != sha(LOCK):
                raise RuntimeError(f'partial or foreign source tree: {target}; inspect, do not overwrite')
            if variant == 'baseline' and identity(target) != {k: spec[k] for k in ('head', 'patch_sha256', 'untracked')}:
                raise RuntimeError('baseline clone drift')
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        run_logged(['git', 'clone', '--local', '--no-hardlinks', '--no-checkout', WORKSPACE / name, target], evidence, name + '-clone', env=clean_env())
        run_logged(['git', 'checkout', '--detach', spec['head']], evidence, name + '-checkout', cwd=target, env=clean_env())
        patch = PACKAGE / spec['patch']
        if patch.stat().st_size:
            run_logged(['git', 'apply', '--binary', patch], evidence, name + '-patch', cwd=target, env=clean_env())
        with (target / '.git/info/exclude').open('a') as f:
            f.write('\n.graph-baseline.json\n')
        write(marker, {'source_lock_sha256': sha(LOCK), 'variant': variant})
        if identity(target) != {k: spec[k] for k in ('head', 'patch_sha256', 'untracked')}:
            raise RuntimeError('reconstruction mismatch')
    guard_isolation(variant)
    write(evidence / 'summary.json', {'status': 'PASS', 'scope': 'source reconstruction', 'variant': variant, 'finished': now()})

def build(variant, jobs):
    if variant in ADJ_VARIANTS:
        from adj_lab import build as build_adj
        return build_adj(variant, jobs)
    if variant in BTREE_VARIANTS:
        return build_btree_release(jobs, variant)
    p = paths(variant)
    prepare(variant)
    if (p['runtime'] / 'data/postmaster.pid').exists():
        raise RuntimeError('stop this laboratory instance before installing')
    pgsrc = source_root(variant) / 'postgres19'
    agesrc = source_root(variant) / 'apache-age-pg19-port'
    pgbuild = p['build'] / 'postgres19'
    pgbuild.mkdir(parents=True, exist_ok=True)
    out = run_dir('build-' + variant)
    env = clean_env()
    env['CFLAGS'] = read(LOCK)['cflags']
    commands = []
    commands.append(run_logged([pgsrc / 'configure', '--prefix=' + str(p['install']), *read(LOCK)['configure']], out, 'configure', cwd=pgbuild, env=env))
    commands.append(run_logged(['make', '-j' + str(jobs), 'world-bin'], out, 'pg-build', cwd=pgbuild, env=env))
    commands.append(run_logged(['make', 'install-world-bin'], out, 'pg-install', cwd=pgbuild, env=env))
    pc = p['install'] / 'bin/pg_config'
    commands.append(run_logged(['make', '-j' + str(jobs), 'PG_CONFIG=' + str(pc)], out, 'age-build', cwd=agesrc, env=env))
    commands.append(run_logged(['make', 'install', 'PG_CONFIG=' + str(pc)], out, 'age-install', cwd=agesrc, env=env))
    isolation_report = guard_isolation(variant, require_install=True)
    meta = {'status': 'PASS', 'scope': 'build only', 'variant': variant, 'finished': now(),
            'isolation': isolation_report,
            'source_lock_sha256': sha(LOCK), 'sources': {n: identity(source_root(variant) / n) for n in BASE_HEADS},
            'configure': capture([pc, '--configure']), 'cflags': capture([pc, '--cflags']),
            'postgres_sha256': sha(p['install'] / 'bin/postgres'),
            'age_sha256': sha(p['install'] / 'lib/postgresql/age.so'), 'commands': commands,
            'evidence': str(out.relative_to(PACKAGE))}
    write(out / 'summary.json', meta)
    write(p['build'] / 'build_manifest.json', meta)
    print('Built isolated ' + variant, flush=True)

def verify_btree_opt_source(target, variant='btree-opt'):
    """Only the declared, content-addressed experiment is allowed."""
    manifests = {'btree-opt': BTREE_OPT_MANIFEST, 'btree-reuse': BTREE_REUSE_MANIFEST,
                 'btree-compact': BTREE_COMPACT_MANIFEST,
                 'btree-startup': BTREE_STARTUP_MANIFEST,
                 'btree-deform': BTREE_DEFORM_MANIFEST,
                 'btree-refview': BTREE_REFVIEW_MANIFEST,
                 'btree-array': BTREE_ARRAY_MANIFEST, 'btree-tail': BTREE_TAIL_MANIFEST,
                 'btree-factor': BTREE_FACTOR_MANIFEST,
                 'btree-factor-batch': BTREE_FACTOR_BATCH_MANIFEST,
                 'btree-anchor-stats': BTREE_ANCHOR_STATS_MANIFEST,
                 'btree-suffix': BTREE_SUFFIX_MANIFEST,
                 'btree-direct': BTREE_DIRECT_MANIFEST,
                 'btree-throughput': BTREE_THROUGHPUT_MANIFEST,
                 'btree-fused': BTREE_FUSED_MANIFEST,
                 'btree-cursor': BTREE_CURSOR_MANIFEST,
                 'btree-lease': BTREE_LEASE_MANIFEST,
                 'btree-layout': BTREE_LAYOUT_MANIFEST,
                 'btree-singleton': BTREE_SINGLETON_MANIFEST,
                 'btree-reader': BTREE_READER_MANIFEST, 'btree-prefix': BTREE_PREFIX_MANIFEST,
                 'btree-delta': BTREE_DELTA_MANIFEST, 'btree-sink': BTREE_SINK_MANIFEST,
                 'btree-joint': BTREE_JOINT_MANIFEST, 'btree-setup': BTREE_SETUP_MANIFEST,
                 'btree-rank': BTREE_RANK_MANIFEST, 'btree-emit': BTREE_EMIT_MANIFEST,
                 'btree-product': BTREE_PRODUCT_MANIFEST,
                 'btree-heapkeys': BTREE_HEAPKEYS_MANIFEST,
                 'btree-window': BTREE_WINDOW_MANIFEST, 'btree-eof': BTREE_EOF_MANIFEST}
    manifest_path = manifests[variant]
    manifest = read(manifest_path)
    ident = identity(target)
    if ident['head'] != manifest['base_head'] or ident['head'] != BTREE_RELEASE_HEAD or ident['untracked']:
        raise RuntimeError('B-tree optimization source base/untracked drift')
    if manifest.get('patch_sha256') not in (None, ident['patch_sha256']):
        raise RuntimeError('B-tree optimization patch digest drift')
    changed = set(capture(['git', 'diff', '--name-only', 'HEAD', '--'], target).splitlines())
    if changed != set(manifest['files']):
        raise RuntimeError('B-tree optimization file set differs from frozen experiment')
    for name, expected in manifest['files'].items():
        path = target / name
        if not path.resolve().is_relative_to(target.resolve()) or sha(path) != expected:
            raise RuntimeError('B-tree optimization content drift: ' + name)
    return {'manifest_sha256': sha(manifest_path), 'identity': ident}

def prepare_btree_release(variant='btree-release'):
    """Pinned C+B diagnostic clone, never switch or install the candidate."""
    guard_isolation(variant)
    verify_sources()
    target = source_root(variant) / 'postgres19'
    out = run_dir('prepare-' + variant)
    if not target.exists():
        target.parent.mkdir(parents=True, exist_ok=True)
        run_logged(['git', 'clone', '--local', '--no-hardlinks', '--no-checkout',
                    source_root('candidate') / 'postgres19', target], out, 'clone', env=clean_env())
        run_logged(['git', 'checkout', '--detach', BTREE_RELEASE_HEAD], out, 'checkout',
                   cwd=target, env=clean_env())
    ident = identity(target)
    dirty = capture(['git', 'status', '--porcelain=v1'], target).strip()
    if ident['head'] != BTREE_RELEASE_HEAD or ident['untracked'] or (dirty and variant == 'btree-release'):
        raise RuntimeError('B-tree Release source differs from the pinned clean C+B commit')
    if dirty:
        verify_btree_opt_source(target, variant)
    if (target / 'src/backend/access/graphadj').exists():
        raise RuntimeError('graph_adj must not be present in B-tree-only source')
    write(out / 'summary.json', {'status': 'PASS', 'sources': {'postgres19': ident},
        'isolation': guard_isolation(variant), 'finished': now()})

def build_btree_release(jobs, variant='btree-release'):
    prepare_btree_release(variant)
    p = paths(variant)
    experiment = (verify_btree_opt_source(source_root(variant) / 'postgres19', variant)
                  if variant != 'btree-release' else None)
    if (p['runtime'] / 'data/postmaster.pid').exists():
        raise RuntimeError('stop B-tree Release before installing')
    pgsrc = source_root(variant) / 'postgres19'
    pgbuild = p['build'] / 'postgres19'
    pgbuild.mkdir(parents=True, exist_ok=True)
    out = run_dir('build-' + variant)
    env = clean_env()
    env['CFLAGS'] = '-O2 -g'
    commands = []
    commands.append(run_logged([pgsrc / 'configure', '--prefix=' + str(p['install']),
        '--disable-debug', '--disable-cassert', '--enable-depend'], out, 'configure', cwd=pgbuild, env=env))
    commands.append(run_logged(['make', '-j' + str(jobs)], out, 'pg-build', cwd=pgbuild, env=env))
    commands.append(run_logged(['make', 'install'], out, 'pg-install', cwd=pgbuild, env=env))
    module = pgbuild / 'src/test/modules/test_graph_core'
    commands.append(run_logged(['make', '-j' + str(jobs), 'all'], out, 'module-build', cwd=module, env=env))
    commands.append(run_logged(['make', 'install'], out, 'module-install', cwd=module, env=env))
    pc = p['install'] / 'bin/pg_config'
    symbols = capture(['nm', p['install'] / 'bin/postgres'])
    if 'GetGgcBtreeAdjProvider' not in symbols or 'GetGgcGraphAdjProvider' in symbols:
        raise RuntimeError('B-tree-only binary symbol check failed')
    meta = {'status': 'PASS', 'scope': 'pinned B-tree-only Release build; not a product gate',
        'variant': variant, 'finished': now(), 'commands': commands,
        'experiment': experiment,
        'sources': {'postgres19': identity(pgsrc)}, 'isolation': guard_isolation(variant, require_install=True),
        'configure': capture([pc, '--configure']), 'cflags': capture([pc, '--cflags']),
        'postgres_sha256': sha(p['install'] / 'bin/postgres'),
        'module_sha256': sha(p['install'] / 'lib/postgresql/test_graph_core.so')}
    write(out / 'summary.json', meta)
    write(p['build'] / 'build_manifest.json', meta)
    print('Built isolated ' + variant, flush=True)

def build_clean(variant, jobs):
    """Archive owned products instead of deleting them, then build from empty."""
    guard_isolation(variant)
    verify_sources()
    p = paths(variant)
    if (p['runtime'] / 'data/postmaster.pid').exists():
        raise RuntimeError('stop this laboratory instance before clean build')
    out = run_dir('clean-build-' + variant)
    tag = variant + '-' + dt.datetime.now(dt.timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    # Candidate AGE is an in-source PGXS build; clear generated objects using its
    # own Makefile before archiving the old PG_CONFIG installation.
    agesrc = source_root(variant) / 'apache-age-pg19-port'
    if variant == 'candidate' and agesrc.exists() and any(agesrc.rglob('*.o')):
        pc = p['install'] / 'bin/pg_config'
        if not pc.exists(): pc = paths('baseline')['install'] / 'bin/pg_config'
        run_logged(['make','clean','PG_CONFIG='+str(pc)],out,'age-clean',cwd=agesrc,env=clean_env())
    archived = {}
    for key in ('build','install'):
        path = p[key]
        if not path.exists(): continue
        if key == 'build' and not (path/'build_manifest.json').exists() and not (path/'src/postgres19/.graph-baseline.json').exists():
            raise RuntimeError('refusing to archive an unrecognized build tree: '+str(path))
        if key == 'install' and not (path/'bin/pg_config').is_file():
            raise RuntimeError('refusing to archive an unrecognized installation: '+str(path))
        target = path.parent / '_archive' / tag
        target.parent.mkdir(parents=True,exist_ok=True)
        path.rename(target)
        archived[key] = str(target)
    write(out/'archived.json',archived)
    # An interrupted build leaves its own partial products and the old archive;
    # it never silently restores mismatched libraries under a running server.
    build(variant,jobs)
    write(out/'summary.json',{'status':'PASS','scope':'build from empty build/install trees',
                              'variant':variant,'archived':archived,'finished':now(),
                              'build_manifest':read(p['build']/'build_manifest.json')})

def db_env(variant, db='graph_lab'):
    p = paths(variant)
    env = clean_env()
    env.update({'PGHOST': str(p['runtime'] / 'socket'), 'PGPORT': str(p['port']), 'PGDATABASE': db, 'PGUSER': 'graph_lab', 'PGCONNECT_TIMEOUT': '5'})
    return env

def sql(variant, query, db='graph_lab'):
    p = paths(variant)
    result = subprocess.run([p['install'] / 'bin/psql', '-X', '-qAt', '-v', 'ON_ERROR_STOP=1', '-d', db],
                            input=query, text=True, capture_output=True, env=db_env(variant, db), timeout=120)
    if result.returncode:
        raise RuntimeError(result.stderr.strip())
    return result.stdout

def cluster(variant, action):
    # A contaminated configuration must remain stoppable with this lab's own ctl.
    guard_layout(variant)
    if action == 'start':
        guard_isolation(variant, require_install=True)
    p = paths(variant)
    data = p['runtime'] / 'data'
    socket = p['runtime'] / 'socket'
    ctl = p['install'] / 'bin/pg_ctl'
    p['runtime'].mkdir(parents=True, exist_ok=True)
    marker = p['runtime'] / '.graph-lab-owned'
    if data.exists() and not marker.exists():
        raise RuntimeError('refusing foreign data directory')
    env = clean_env()
    out = run_dir('cluster-' + variant + '-' + action)
    if action == 'status':
        return subprocess.call([ctl, '-D', data, 'status'], env=env)
    if action == 'stop':
        if (data / 'postmaster.pid').exists():
            run_logged([ctl, '-D', data, '-m', 'fast', '-w', '-t', '45', 'stop'], out, 'stop', env=env, timeout=60)
        return 0
    if not (data / 'PG_VERSION').exists():
        marker.write_text('pg19-graph-core\n')
        socket.mkdir(parents=True, exist_ok=True)
        run_logged([p['install'] / 'bin/initdb', '-D', data, '-U', 'graph_lab', '--encoding=UTF8', '--locale=C', '--auth-local=trust', '--auth-host=reject'], out, 'initdb', env=env)
        with (data / 'postgresql.conf').open('a') as f:
            f.write("\nlisten_addresses = ''\nport = " + str(p['port']) + "\nunix_socket_directories = '" + str(socket) + "'\nshared_buffers = '128MB'\nwork_mem = '8MB'\nmax_connections = 32\nmax_parallel_workers_per_gather = 0\nfsync = on\nsynchronous_commit = on\nfull_page_writes = on\nautovacuum = on\ncheckpoint_timeout = '5min'\n")
    if not (data / 'postmaster.pid').exists():
        guard_isolation(variant, require_install=True)
        run_logged([ctl, '-D', data, '-l', p['runtime'] / 'postgresql.log', '-w', '-t', '45', 'start'], out, 'start', env=env, timeout=60)
    actual = sql(variant, 'SHOW data_directory;', 'postgres').strip()
    if Path(actual).resolve() != data.resolve():
        raise RuntimeError('unexpected server')
    write(out / 'isolation.json', guard_isolation(variant, online=True))
    if sql(variant, "SELECT 1 FROM pg_database WHERE datname = 'graph_lab';", 'postgres').strip() != '1':
        sql(variant, 'CREATE DATABASE graph_lab;', 'postgres')
    print(f'Connected to isolated {variant}: {data}', flush=True)
    return 0

def regress(variant, suite):
    if variant in (*BTREE_VARIANTS, *ADJ_VARIANTS) and suite != 'pg':
        raise RuntimeError('B-tree-only diagnostic variant has no AGE installation')
    guard_isolation(variant, require_install=True)
    p = paths(variant)
    out = run_dir('regress-' + variant + '-' + suite)
    env = clean_env()
    env['PGPORT'] = str(p['regress_port'])
    if suite == 'pg':
        cwd = p['build'] / 'postgres19/src/test/regress'
        # Preserve upstream --dlpath=. and other REGRESS_OPTS defaults.
        cmd = ['make', 'check', 'MAX_CONNECTIONS=4', 'EXTRA_REGRESS_OPTS=--port=' + str(p['regress_port'])]
    else:
        cwd = source_root(variant) / 'apache-age-pg19-port'
        instance = p['runtime'] / ('regress-' + out.name)
        opts = f'--dbname=contrib_regression --load-extension=age --inputdir={cwd}/regress --outputdir={out} --temp-instance={instance} --port={p["regress_port"] + 2} --encoding=UTF-8 --temp-config={cwd}/regress/age_regression.conf'
        cmd = ['make', 'installcheck', 'PG_CONFIG=' + str(p['install'] / 'bin/pg_config'), 'REGRESS_OPTS=' + opts]
    rec = run_logged(cmd, out, 'regression', cwd=cwd, env=env, timeout=1800)
    write(out / 'summary.json', {'status': 'PASS', 'scope': suite + ' upstream regression', 'variant': variant, 'command': rec, 'finished': now()})

def main():
    a = argparse.ArgumentParser(description=__doc__)
    a.add_argument('command', choices=['freeze', 'preflight', 'isolation', 'prepare', 'build', 'build-clean', 'start', 'stop', 'status', 'regress'])
    a.add_argument('--online', action='store_true', help='isolation: inspect an already running graph lab')
    a.add_argument('--variant', choices=list(VARIANT_PORTS), default='baseline')
    a.add_argument('--jobs', type=int, default=4)
    a.add_argument('--suite', choices=['pg', 'age'], default='pg')
    x = a.parse_args()
    if x.online and x.command != 'isolation':
        a.error('--online is only valid with isolation')
    if not 1 <= x.jobs <= 8:
        a.error('jobs must be 1..8 on this laboratory')
    if x.command == 'freeze': freeze()
    elif x.command == 'preflight': preflight()
    elif x.command == 'isolation': audit_isolation(x.variant, online=x.online)
    elif x.command == 'prepare': prepare(x.variant)
    elif x.command == 'build': build(x.variant, x.jobs)
    elif x.command == 'build-clean': build_clean(x.variant, x.jobs)
    elif x.command == 'regress': regress(x.variant, x.suite)
    else: return cluster(x.variant, x.command)
    return 0

if __name__ == '__main__':
    try:
        sys.exit(main())
    except (RuntimeError, OSError, subprocess.SubprocessError) as e:
        print(str(e), file=sys.stderr)
        sys.exit(1)
