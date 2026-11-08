#!/usr/bin/env python3
"""Fail closed on accidental reuse of the ORCA/Daft development environment."""
from pathlib import Path
import json
import subprocess

PRELOADS = ('shared_preload_libraries', 'session_preload_libraries', 'local_preload_libraries')
DURABLE_SOURCE_VARIANTS = frozenset((
    'pgq-hints', 'pgq-sets', 'pgq-relational', 'pgq-independent',
    'pgq-cardinality', 'pgq-probe-batch', 'pgq-cover', 'pgq-pipeline',
    'pgq-range', 'pgq-reuse', 'pgq-pointer', 'pgq-seek', 'pgq-routebatch',
    'pgq-leafprep-recovered', 'pgq-routebatch-recovered',
    'pgq-leaf-isolated', 'pgq-prepare-isolated', 'pgq-adj-tree', 'pgq-adj-tree-safe',
    'pgq-age-memory', 'pgq-age-fusion'))


def require(condition, message):
    if not condition:
        raise RuntimeError('ISOLATION: ' + message)


def foreign(value):
    return any(token in str(value).lower() for token in ('orca', 'daft'))


def layout(workspace, variant, paths, source):
    """Check before any mkdir, configure, archive, install or pg_ctl operation."""
    ports = {'baseline': (55449, 55459), 'candidate': (55450, 55460),
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
    ports['pgq-leafprep-recovered'] = (55658, 55659)
    ports['pgq-routebatch-recovered'] = (55660, 55661)
    ports['pgq-leaf-isolated'] = (55662, 55663)
    ports['pgq-prepare-isolated'] = (55664, 55665)
    ports['pgq-adj-tree'] = (55666, 55667)
    ports['pgq-adj-tree-safe'] = (55668, 55669)
    ports['pgq-age-memory'] = (55670, 55671)
    ports['pgq-age-fusion'] = (55672, 55673)
    ports['pgq-integrated'] = (55674, 55675)
    require(variant in ports, 'unknown laboratory variant')
    workspace = Path(workspace).resolve()
    expected = {k: workspace / ('.' + k) / 'pg19-graph-core' / variant
                for k in ('build', 'install', 'runtime')}
    expected['source'] = (expected['build'] / 'src' if variant not in ('candidate', 'pgq-integrated')
                          else workspace / '.worktrees/pg19-graph-core')
    if variant in DURABLE_SOURCE_VARIANTS:
        expected['source'] = workspace / 'graph-experiments' / variant
    actual = dict(paths, source=source)
    for key, path in expected.items():
        require(Path(actual[key]).absolute() == path and path.resolve() == path,
                f'{key} redirected outside its dedicated path: {actual[key]}')
    for key in ('build', 'install'):
        archive = expected[key].parent / '_archive'
        require(archive.resolve() == archive, f'archive directory redirected: {archive}')
    critical = [expected['build'] / 'postgres19', expected['runtime'] / 'socket',
                expected['runtime'] / '.graph-lab-owned', expected['runtime'] / 'data']
    critical += [expected['runtime'] / 'data' / name for name in
                 ('postgresql.conf', 'postgresql.auto.conf', 'pg_hba.conf', 'pg_ident.conf',
                  'postmaster.pid', 'postmaster.opts', 'pg_wal')]
    critical += [Path(source) / name for name in ('postgres19', 'apache-age-pg19-port')]
    for path in critical:
        require(path.resolve() == path, f'critical path is a symlink: {path}')
    require(paths['port'] == ports[variant][0], 'unexpected service port')
    require(paths['regress_port'] == ports[variant][1], 'unexpected regression port')
    return {key: str(path) for key, path in expected.items()}


def command(argv, env, cwd=None):
    return subprocess.check_output([str(x) for x in argv], env=env, cwd=cwd,
                                   text=True, stderr=subprocess.PIPE, timeout=30).strip()


def expected_settings(paths):
    data = paths['runtime'] / 'data'
    return {**{key: '' for key in PRELOADS}, 'dynamic_library_path': '$libdir',
            'data_directory': str(data), 'config_file': str(data / 'postgresql.conf'),
            'hba_file': str(data / 'pg_hba.conf'), 'ident_file': str(data / 'pg_ident.conf'),
            'port': str(paths['port']), 'listen_addresses': '',
            'unix_socket_directories': str(paths['runtime'] / 'socket')}


def check_settings(settings, paths):
    for key, expected in expected_settings(paths).items():
        require(settings.get(key) == expected,
                f'{key}: expected {expected!r}, found {settings.get(key)!r}')


def check_install(install, env):
    hardlinks = {}
    for path in install.rglob('*'):
        require(path.resolve().is_relative_to(install), f'installed path escapes prefix: {path}')
        require(not foreign(path.relative_to(install)), f'ORCA/Daft artifact installed: {path}')
        if path.is_file() and not path.is_symlink():
            info = path.stat()
            if info.st_nlink > 1:
                item = hardlinks.setdefault((info.st_dev, info.st_ino), [0, info.st_nlink, path])
                item[0] += 1
    # zic deliberately hard-links timezone aliases within one installation.
    for count, total, path in hardlinks.values():
        require(count == total, f'installed file shares a hard link outside this prefix: {path}')
    pc = install / 'bin/pg_config'
    for binary in ('pg_config', 'postgres', 'pg_ctl', 'initdb', 'psql'):
        require((install / 'bin' / binary).is_file(), f'missing installed binary: {binary}')
    dirs = {}
    for key, suffix in (('bindir', 'bin'), ('libdir', 'lib'),
                        ('pkglibdir', 'lib/postgresql'), ('sharedir', 'share/postgresql')):
        dirs[key] = command([pc, '--' + key], env)
        require(dirs[key] == str(install / suffix), f'pg_config --{key} points to another install')
    return dirs


def offline(workspace, variant, paths, source, env, require_install=False):
    report = {'variant': variant, 'paths': layout(workspace, variant, paths, source),
              'sources': {}, 'runtime': 'NOT_CHECKED_OFFLINE', 'resource_isolation': 'NOT_ENFORCED'}
    for name in ('postgres19', 'apache-age-pg19-port'):
        repo = source / name
        if not repo.exists():
            report['sources'][name] = 'NOT_PREPARED'
            continue
        gitdir = repo / '.git'
        require(gitdir.is_dir() and not gitdir.is_symlink(), f'not an independent clone: {repo}')
        common = command(['git', 'rev-parse', '--path-format=absolute', '--git-common-dir'], env, repo)
        require(common == str(gitdir), f'Git common directory shared: {repo}')
        alternates = gitdir / 'objects/info/alternates'
        require(not alternates.exists() or not alternates.read_text().strip(), f'Git objects borrowed: {repo}')
        report['sources'][name] = {'git_common_dir': common, 'object_alternates': False}
    install = paths['install']
    if not (install / 'bin/pg_config').exists():
        require(not require_install, f'{variant} has not been installed')
        report.update(status='PARTIAL', installation='NOT_BUILT', configuration='NOT_INITIALIZED')
        return report
    report['installation'] = check_install(install, env)
    data = paths['runtime'] / 'data'
    if (data / 'PG_VERSION').exists():
        marker = paths['runtime'] / '.graph-lab-owned'
        require(marker.is_file() and marker.read_text().strip() == 'pg19-graph-core', 'foreign data directory')
        # postgres -C reads effective includes and postgresql.auto.conf without starting a server.
        settings = {key: command([install / 'bin/postgres', '-D', data, '-C', key], env)
                    for key in expected_settings(paths)}
        check_settings(settings, paths)
        report['configuration'] = settings
    else:
        report['configuration'] = 'NOT_INITIALIZED'
    report['status'] = 'PASS'
    return report


def check_runtime_record(record, paths):
    check_settings(record['settings'], paths)
    require(not record['foreign_settings'], 'ORCA/Daft GUC registered in backend')
    require(not record['foreign_functions'], 'C function references ORCA/Daft')
    require(not any(foreign(name) for name in record['extensions']), 'ORCA/Daft extension in database')
    for item in record['role_database_settings']:
        key, value = item.split('=', 1)
        require(not foreign(key) and not foreign(value), 'ORCA/Daft role/database setting')
        if key in (*PRELOADS, 'dynamic_library_path'):
            require(value == expected_settings(paths)[key], f'role/database override: {item}')
    for path in record['configuration_files']:
        require(Path(path).resolve().is_relative_to(paths['runtime'] / 'data'),
                f'configuration include outside graph PGDATA: {path}')


def online(paths, query, probe_library='age'):
    """Check every connectable database and a fresh, local library probe."""
    require(probe_library in ('age', 'test_graph_core'), 'unexpected probe library')
    databases = json.loads(query("SELECT json_agg(datname ORDER BY datname) FROM pg_database WHERE datallowconn;", 'postgres'))
    records = {}
    keys = ','.join("'" + key + "'" for key in expected_settings(paths))
    for database in databases:
        record = json.loads(query(f"""
          SELECT json_build_object(
            'settings', (SELECT json_object_agg(name,setting) FROM pg_settings WHERE name IN ({keys})),
            'foreign_settings', (SELECT coalesce(json_agg(name),'[]') FROM pg_settings WHERE name ~* 'orca|daft'),
            'extensions', (SELECT coalesce(json_agg(extname),'[]') FROM pg_extension),
            'foreign_functions', (SELECT coalesce(json_agg(oid::text),'[]') FROM pg_proc WHERE probin ~* 'orca|daft'),
            'role_database_settings', (SELECT coalesce(json_agg(setting),'[]') FROM pg_db_role_setting CROSS JOIN LATERAL unnest(setconfig) AS setting),
            'configuration_files', (SELECT coalesce(json_agg(DISTINCT sourcefile),'[]') FROM pg_file_settings));
        """, database))
        check_runtime_record(record, paths)
        records[database] = record
    # /proc is used deliberately: the laboratory is Linux/WSL. This checks actual
    # mapped libraries in a new backend after loading the AGE adapter.
    maps = query(f"LOAD '{probe_library}'; SELECT pg_read_file('/proc/self/maps');", 'postgres')
    require(not foreign(maps), 'ORCA/Daft is mapped into the AGE probe backend')
    library = paths['install'] / 'lib/postgresql' / (probe_library + '.so')
    require(str(library) in maps, 'probe did not load this installation\'s library')
    mapped = sorted({line.split(maxsplit=5)[5] for line in maps.splitlines() if '/' in line})
    require(str(paths['install'] / 'bin/postgres') in mapped, 'probe backend is not this installation\'s postgres')
    return {'status': 'PASS', 'databases': records,
            ('age_probe_mapped_files' if probe_library == 'age' else 'graph_probe_mapped_files'): mapped,
            'scope': 'current databases and fresh probe backend; not a sandbox against manual superuser changes'}
