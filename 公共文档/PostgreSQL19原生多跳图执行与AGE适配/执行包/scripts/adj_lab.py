#!/usr/bin/env python3
"""Isolated C+B+A Release laboratory, preserving D083 and the main worktree."""
import lab
from common import PACKAGE,capture,identity,now,read,run_dir,run_logged,sha,write

HEAD='86fd33613da8b9e8ef2d6496381be38c42114fc5'
BASE=PACKAGE/'golden/G1-080/btree_eof_opt_v1.json'

def prepare(variant):
    assert variant in lab.ADJ_VARIANTS
    lab.guard_layout(variant); lab.verify_sources()
    src=lab.source_root(variant)/'postgres19'
    out=run_dir('prepare-'+variant)
    selected=(PACKAGE/'golden/G1-080/adj_stream_v1.json') if variant=='adj-scale' else BASE
    if variant == 'adj-inline':
        selected = PACKAGE/'golden/G1-080/adj_flatten_v1.json'
    if variant == 'adj-create':
        selected = PACKAGE/'golden/G1-080/adj_inline_v1.json'
    if variant == 'adj-set':
        frozen = PACKAGE/'golden/G1-080/endpoint_set_v1.json'
        selected = frozen if frozen.exists() else PACKAGE/'golden/G1-080/adj_create_v1.json'
    if variant == 'adj-refcache':
        frozen = PACKAGE/'golden/G1-080/adj_refcache_assoc_v1.json'
        selected = frozen if frozen.exists() else PACKAGE/'golden/G1-080/endpoint_set_v1.json'
    if variant == 'adj-layout':
        frozen = PACKAGE/'golden/G1-080/adj_refcache_layout_v1.json'
        selected = frozen if frozen.exists() else PACKAGE/'golden/G1-080/adj_refcache_assoc_v1.json'
    if variant == 'adj-rootwal':
        frozen = PACKAGE/'golden/G1-080/adj_rootwal_v1.json'
        selected = frozen if frozen.exists() else PACKAGE/'golden/G1-080/endpoint_set_v1.json'
    if variant == 'adj-vacstats':
        frozen = PACKAGE/'golden/G1-080/adj_vacstats_v1.json'
        selected = frozen if frozen.exists() else PACKAGE/'golden/G1-080/endpoint_set_v1.json'
    if variant == 'adj-frontier':
        frozen = PACKAGE/'golden/G1-080/adj_frontier_v1.json'
        selected = frozen if frozen.exists() else PACKAGE/'golden/G1-080/endpoint_set_v1.json'
    if variant == 'adj-allocmap':
        frozen = PACKAGE/'golden/G1-080/adj_allocmap_v1.json'
        selected = frozen if frozen.exists() else PACKAGE/'golden/G1-080/endpoint_set_v1.json'
    if variant == 'adj-frontier-reuse':
        frozen = PACKAGE/'golden/G1-080/adj_frontier_reuse_v1.json'
        selected = frozen if frozen.exists() else PACKAGE/'golden/G1-080/adj_frontier_v1.json'
    if variant == 'adj-chain':
        frozen = PACKAGE/'golden/G1-080/adj_chain_v1.json'
        selected = frozen if frozen.exists() else PACKAGE/'golden/G1-080/endpoint_set_v1.json'
    if variant == 'pgq-equality':
        frozen = PACKAGE/'golden/G1-080/pgq_equality_v1.json'
        selected = frozen if frozen.exists() else PACKAGE/'golden/G1-080/endpoint_set_v1.json'
    if variant == 'pgq-equality-safe':
        frozen = PACKAGE/'golden/G1-080/pgq_equality_v2.json'
        selected = frozen if frozen.exists() else PACKAGE/'golden/G1-080/pgq_equality_v1.json'
    if variant == 'pgq-hints':
        frozen = PACKAGE/'golden/G1-080/pgq_route_hints_v1.json'
        selected = frozen if frozen.exists() else PACKAGE/'golden/G1-080/pgq_equality_v2.json'
    if variant == 'pgq-sets':
        frozen = PACKAGE/'golden/G1-080/pgq_endpoint_sets_v1.json'
        selected = frozen if frozen.exists() else PACKAGE/'golden/G1-080/pgq_route_hints_v1.json'
    if variant == 'pgq-relational':
        frozen = PACKAGE/'golden/G1-080/pgq_relational_route_v1.json'
        selected = frozen if frozen.exists() else PACKAGE/'golden/G1-080/pgq_endpoint_sets_v1.json'
    if variant == 'pgq-independent':
        frozen = PACKAGE/'golden/G1-080/pgq_independent_route_v1.json'
        selected = frozen if frozen.exists() else PACKAGE/'golden/G1-080/pgq_relational_route_v1.json'
    if variant == 'pgq-cardinality':
        frozen = PACKAGE/'golden/G1-080/pgq_cardinality_v1.json'
        selected = frozen if frozen.exists() else PACKAGE/'golden/G1-080/pgq_independent_route_v1.json'
    if variant == 'pgq-probe-batch':
        frozen = PACKAGE/'golden/G1-080/pgq_probe_batch_v1.json'
        selected = frozen if frozen.exists() else PACKAGE/'golden/G1-080/pgq_cardinality_v1.json'
    if variant == 'pgq-cover':
        frozen = PACKAGE/'golden/G1-080/pgq_cover_v1.json'
        selected = frozen if frozen.exists() else PACKAGE/'golden/G1-080/pgq_probe_batch_v1.json'
    if variant == 'pgq-pipeline':
        frozen = PACKAGE/'golden/G1-080/pgq_pipeline_v1.json'
        selected = frozen if frozen.exists() else PACKAGE/'golden/G1-080/pgq_probe_batch_v1.json'
    if variant == 'pgq-range':
        frozen = PACKAGE/'golden/G1-080/pgq_range_v1.json'
        selected = frozen if frozen.exists() else PACKAGE/'golden/G1-080/pgq_pipeline_v1.json'
    if variant == 'pgq-reuse':
        frozen = PACKAGE/'golden/G1-080/pgq_reuse_v1.json'
        selected = frozen if frozen.exists() else PACKAGE/'golden/G1-080/pgq_range_v1.json'
    if variant == 'pgq-pointer':
        frozen = PACKAGE/'golden/G1-080/pgq_pointer_v1.json'
        selected = frozen if frozen.exists() else PACKAGE/'golden/G1-080/pgq_range_v1.json'
    if variant == 'pgq-seek':
        frozen = PACKAGE/'golden/G1-080/pgq_seek_v1.json'
        selected = frozen if frozen.exists() else PACKAGE/'golden/G1-080/pgq_pointer_v1.json'
    manifest=read(selected)
    if variant == 'pgq-routebatch':
        frozen = PACKAGE/'golden/G1-080/pgq_routebatch_v1.json'
        selected = frozen if frozen.exists() else PACKAGE/'golden/G1-080/pgq_seek_v1.json'
        manifest = read(selected)
    if variant == 'pgq-leafprep':
        frozen = PACKAGE/'golden/G1-080/pgq_leafprep_v1.json'
        selected = frozen if frozen.exists() else PACKAGE/'golden/G1-080/pgq_routebatch_v1.json'
        manifest = read(selected)
    if variant == 'pgq-leafprep-recovered':
        selected = PACKAGE/'golden/G1-080/pgq_leafprep_recovered_v1.json'
        manifest = read(selected)
    if variant == 'pgq-routebatch-recovered':
        selected = PACKAGE/'golden/G1-080/pgq_routebatch_v1.json'
        manifest = read(selected)
    if variant in ('pgq-leaf-isolated', 'pgq-prepare-isolated'):
        frozen = PACKAGE/'golden/G1-080'/((variant.replace('-', '_'))+'_v1.json')
        selected = frozen if frozen.exists() else PACKAGE/'golden/G1-080/pgq_leafprep_recovered_v1.json'
        manifest = read(selected)
    if variant == 'pgq-adj-tree':
        frozen = PACKAGE/'golden/G1-080/pgq_adj_tree_v1.json'
        selected = frozen if frozen.exists() else PACKAGE/'golden/G1-080/pgq_routebatch_v1.json'
        manifest = read(selected)
    if variant == 'pgq-adj-tree-safe':
        frozen = PACKAGE/'golden/G1-080/pgq_adj_tree_safe_v1.json'
        selected = frozen if frozen.exists() else PACKAGE/'golden/G1-080/pgq_adj_tree_v1.json'
        manifest = read(selected)
    if variant == 'pgq-age-memory':
        selected = PACKAGE/'golden/G1-080/pgq_adj_tree_safe_v1.json'
        manifest = read(selected)
    if variant == 'pgq-age-fusion':
        frozen = PACKAGE/'golden/G1-080/pgq_age_fusion_v1.json'
        selected = frozen if frozen.exists() else PACKAGE/'golden/G1-080/pgq_adj_tree_safe_v1.json'
        manifest = read(selected)
    if not src.exists():
        src.parent.mkdir(parents=True,exist_ok=True)
        run_logged(['git','clone','--local','--no-hardlinks','--no-checkout',lab.source_root('candidate')/'postgres19',src],out,'clone',env=lab.clean_env())
        run_logged(['git','checkout','--detach',HEAD],out,'checkout',cwd=src,env=lab.clean_env())
        patch=selected.with_suffix('.patch')
        assert sha(patch)==manifest['patch_sha256']
        run_logged(['git','apply','--check',patch],out,'check-base',cwd=src,env=lab.clean_env())
        run_logged(['git','apply',patch],out,'apply-base',cwd=src,env=lab.clean_env())
        if manifest.get('combined_patch'):
            patch=selected.parent/manifest['combined_patch']; assert sha(patch)==manifest['combined_patch_sha256']
            run_logged(['git','apply',patch],out,'combined-extra',cwd=src,env=lab.clean_env())
        if variant in ('pgq-leafprep-recovered', 'pgq-routebatch-recovered', 'pgq-leaf-isolated', 'pgq-prepare-isolated') or (variant in ('pgq-hints', 'pgq-sets', 'pgq-relational', 'pgq-independent', 'pgq-cardinality', 'pgq-probe-batch', 'pgq-cover', 'pgq-pipeline', 'pgq-range', 'pgq-reuse', 'pgq-pointer', 'pgq-seek', 'pgq-routebatch', 'pgq-leafprep') and selected.name in ('pgq_route_hints_v1.json', 'pgq_endpoint_sets_v1.json', 'pgq_relational_route_v1.json', 'pgq_independent_route_v1.json', 'pgq_cardinality_v1.json', 'pgq_probe_batch_v1.json', 'pgq_cover_v1.json', 'pgq_pipeline_v1.json', 'pgq_range_v1.json', 'pgq_reuse_v1.json', 'pgq_pointer_v1.json', 'pgq_seek_v1.json', 'pgq_routebatch_v1.json', 'pgq_leafprep_v1.json')):
            # Identity includes newly added source files, without committing.
            run_logged(['git','add','-N','--',
                'src/test/modules/test_graph_core/graph_route_hint.inc',
                'src/test/modules/test_graph_core/sql/graph_route.sql',
                'src/test/modules/test_graph_core/expected/graph_route.out'],
                out,'track-hint-files',cwd=src,env=lab.clean_env())
        if variant in ('pgq-adj-tree', 'pgq-adj-tree-safe', 'pgq-age-memory', 'pgq-age-fusion'):
            run_logged(['git','add','-N','--',
                'src/test/modules/test_graph_core/graph_route_hint.inc',
                'src/test/modules/test_graph_core/sql/graph_route.sql',
                'src/test/modules/test_graph_core/expected/graph_route.out'],
                out,'track-hint-files',cwd=src,env=lab.clean_env())
        if variant == 'pgq-age-fusion' and selected.name == 'pgq_age_fusion_v1.json':
            run_logged(['git','add','-N','--','src/include/executor/graph_visible_adj.h'],
                out,'track-visible-adj-header',cwd=src,env=lab.clean_env())
        write(lab.paths(variant)['build']/'base_source.json',identity(src))
    assert identity(src)['head']==HEAD and not identity(src)['untracked']
    if variant == 'adj-set' and selected.name == 'endpoint_set_v1.json':
        assert identity(src) == manifest['source'], 'frozen endpoint-set source drift'
    if variant == 'adj-refcache' and selected.name == 'adj_refcache_assoc_v1.json':
        assert identity(src) == manifest['source'], 'frozen associative-ref-cache source drift'
    if variant == 'adj-layout' and selected.name == 'adj_refcache_layout_v1.json':
        assert identity(src) == manifest['source'], 'frozen layout-diagnostic source drift'
    if variant == 'adj-rootwal' and selected.name == 'adj_rootwal_v1.json':
        assert identity(src) == manifest['source'], 'frozen root-WAL source drift'
    if variant == 'adj-vacstats' and selected.name == 'adj_vacstats_v1.json':
        assert identity(src) == manifest['source'], 'frozen VACUUM statistics source drift'
    if variant == 'adj-frontier' and selected.name == 'adj_frontier_v1.json':
        assert identity(src) == manifest['source'], 'frozen two-frontier source drift'
    if variant == 'adj-allocmap' and selected.name == 'adj_allocmap_v1.json':
        assert identity(src) == manifest['source'], 'frozen VACUUM allocation-map source drift'
    if variant == 'adj-frontier-reuse' and selected.name == 'adj_frontier_reuse_v1.json':
        assert identity(src) == manifest['source'], 'frozen two-frontier reader-reuse source drift'
    if variant == 'adj-chain' and selected.name == 'adj_chain_v1.json':
        assert identity(src) == manifest['source'], 'frozen multi-segment Base source drift'
    if variant == 'pgq-equality' and selected.name == 'pgq_equality_v1.json':
        assert identity(src) == manifest['source'], 'frozen PGQ equality source drift'
    if variant == 'pgq-equality-safe' and selected.name == 'pgq_equality_v2.json':
        assert identity(src) == manifest['source'], 'frozen PGQ nullable-fallback source drift'
    if variant == 'pgq-hints' and selected.name == 'pgq_route_hints_v1.json':
        assert identity(src) == manifest['source'], 'frozen PGQ route hint source drift'
    if variant == 'pgq-sets' and selected.name == 'pgq_endpoint_sets_v1.json':
        assert identity(src) == manifest['source'], 'frozen PGQ endpoint set source drift'
    if variant == 'pgq-relational' and selected.name == 'pgq_relational_route_v1.json':
        assert identity(src) == manifest['source'], 'frozen PGQ relational route source drift'
    if variant == 'pgq-independent' and selected.name == 'pgq_independent_route_v1.json':
        assert identity(src) == manifest['source'], 'frozen PGQ independent route source drift'
    if variant == 'pgq-cardinality' and selected.name == 'pgq_cardinality_v1.json':
        assert identity(src) == manifest['source'], 'frozen PGQ cardinality/vertex source drift'
    if variant == 'pgq-probe-batch' and selected.name == 'pgq_probe_batch_v1.json':
        assert identity(src) == manifest['source'], 'frozen PGQ probe-batch source drift'
    if variant == 'pgq-cover' and selected.name == 'pgq_cover_v1.json':
        assert identity(src) == manifest['source'], 'frozen PGQ covering source drift'
    if variant == 'pgq-pipeline' and selected.name == 'pgq_pipeline_v1.json':
        assert identity(src) == manifest['source'], 'frozen PGQ pipeline source drift'
    if variant == 'pgq-range' and selected.name == 'pgq_range_v1.json':
        assert identity(src) == manifest['source'], 'frozen PGQ range source drift'
    if variant == 'pgq-reuse' and selected.name == 'pgq_reuse_v1.json':
        assert identity(src) == manifest['source'], 'frozen PGQ reuse source drift'
    if variant == 'pgq-pointer' and selected.name == 'pgq_pointer_v1.json':
        assert identity(src) == manifest['source'], 'frozen PGQ pointer-sort source drift'
    if variant == 'pgq-seek' and selected.name == 'pgq_seek_v1.json':
        assert identity(src) == manifest['source'], 'frozen PGQ seek-hint source drift'
    if variant in lab.isolation.DURABLE_SOURCE_VARIANTS:
        developing = ((variant == 'pgq-adj-tree' and selected.name != 'pgq_adj_tree_v1.json') or
                      (variant == 'pgq-age-fusion' and selected.name != 'pgq_age_fusion_v1.json') or
                      (variant == 'pgq-adj-tree-safe' and selected.name != 'pgq_adj_tree_safe_v1.json'))
        if not developing:
            assert identity(src) == manifest['source'], 'frozen durable source drift'
        # Recreate disposable metadata only after checking the durable snapshot.
        base_path = lab.paths(variant)['build']/'base_source.json'
        if not base_path.exists():
            write(base_path, manifest['source'])
    base=read(lab.paths(variant)['build']/'base_source.json')
    if variant == 'pgq-leafprep' and selected.name == 'pgq_leafprep_v1.json':
        assert identity(src) == manifest['source'], 'frozen leaf/preparation source drift'
    if variant == 'pgq-routebatch' and selected.name == 'pgq_routebatch_v1.json':
        assert identity(src) == manifest['source'], 'frozen route-batch source drift'
    if variant=='adj-eof': assert identity(src)==base
    write(out/'summary.json',dict(status='PASS',source=identity(src),base=base,isolation=lab.guard_isolation(variant),finished=now()))
    return src

def build(variant,jobs):
    src=prepare(variant); p=lab.paths(variant)
    assert not (p['runtime']/'data/postmaster.pid').exists()
    out=run_dir('build-'+variant); print(out,flush=True)
    report=dict(status='RUNNING',variant=variant,sources={'postgres19':identity(src)},commands=[])
    directory=p['build']/'postgres19'; directory.mkdir(parents=True,exist_ok=True)
    env=lab.clean_env(); env['CFLAGS']='-O2 -g'
    try:
        for cmd,name,cwd in [([src/'configure','--prefix='+str(p['install']),'--disable-debug','--disable-cassert','--enable-depend'],'configure',directory),
                (['make','-j'+str(jobs)],'pg-build',directory),(['make','install'],'pg-install',directory),
                (['make','-j'+str(jobs),'all'],'module-build',directory/'src/test/modules/test_graph_core'),
                (['make','install'],'module-install',directory/'src/test/modules/test_graph_core')]:
            report['commands'].append(run_logged(cmd,out,name,cwd=cwd,env=env,timeout=1800))
        assert identity(src)==report['sources']['postgres19']
        symbols=capture(['nm',p['install']/'bin/postgres'])
        assert all(s in symbols for s in ('GetGgcBtreeAdjProvider','GetGgcGraphAdjProvider','GgcFixedEnableFactorized'))
        pc=p['install']/'bin/pg_config'
        report.update(status='PASS',configure=capture([pc,'--configure']),cflags=capture([pc,'--cflags']),
            postgres_sha256=sha(p['install']/'bin/postgres'),module_sha256=sha(p['install']/'lib/postgresql/test_graph_core.so'),
            isolation=lab.guard_isolation(variant,require_install=True))
        write(p['build']/'build_manifest.json',report)
    except BaseException as exc:
        report.update(status='FAIL',error=repr(exc)); raise
    finally:
        report['finished']=now(); write(out/'summary.json',report)
