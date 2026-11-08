#!/usr/bin/env python3
"""Verify graph_adj stable refs through common batching and statistics."""
from __future__ import annotations

import json
from pathlib import Path
import shutil
import subprocess
import sys

SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS))
from common import PACKAGE, WORKSPACE, now, read, run_dir, run_logged, sha, write  # noqa: E402
from lab import db_env, sql  # noqa: E402

TASK_ID = "G1-055"
SOURCE = WORKSPACE / ".worktrees/pg19-graph-core/postgres19"
INSTALL = WORKSPACE / ".install/pg19-graph-core/candidate"
MODULE = SOURCE / "src/test/modules/test_graph_core"
GOLDEN = PACKAGE / "golden/G1-055/contracts.json"


def evidence(path: Path) -> dict:
    return {"path": str(path.relative_to(PACKAGE)), "sha256": sha(path)}


def source_contracts() -> dict:
    provider_path = SOURCE / "src/backend/access/graphadj/graphadj.c"
    batch_path = SOURCE / "src/backend/executor/graph_batch.c"
    stats_path = SOURCE / "src/backend/statistics/graph_stats_path_collect.c"
    provider_header = SOURCE / "src/include/executor/graph_adj_provider.h"
    provider = provider_path.read_text(encoding="utf-8")
    batch = batch_path.read_text(encoding="utf-8")
    stats = stats_path.read_text(encoding="utf-8")
    header = provider_header.read_text(encoding="utf-8")
    provider_tokens = (
        "GRAPHADJ_PROVIDER_CAPS",
        "GGC_CAP_HANDLE_INPUT",
        "GGC_CAP_HANDLE_OUTPUT",
        "GgcEndpointHandleScopeMatches",
        "GGC_HANDLE_PERSISTED_ADJACENCY",
        "graphadj_resolve_debug_handle",
        "PredicateLockRelation",
        "graphadj_collect_candidates",
        "graphadj_provider_get_instrumentation",
    )
    missing = [token for token in provider_tokens if token not in provider]
    if missing:
        raise RuntimeError("graph_adj provider contract is incomplete: " + repr(missing))
    error_pos = provider.find("if (scope_matches && handle->authoritative)")
    fallback_pos = provider.find("counters->directory_lookups++", error_pos)
    if error_pos < 0 or fallback_pos < 0 or error_pos > fallback_pos:
        raise RuntimeError("authoritative persistent ref does not fail before key fallback")
    if not all(token in batch for token in (
            "provider->get_instrumentation",
            "provider_ref_direct_lookups",
            "provider_physical_candidates")):
        raise RuntimeError("common batch instrumentation omits provider physical work")
    if not all(token in stats for token in (
            "GgcGetIndexAdjProvider(index)",
            "parents[i].handle = binding->handle",
            "next[*next_count].handle = row.candidate.opposite_handle",
            "GGC_CAP_BUILD_GENERATION",
            "physical_scope.build_generation : 0")):
        raise RuntimeError("statistics collector does not carry stable refs through graph_adj")
    if ".amgetgraphprovider = GetGgcGraphAdjProvider" not in provider or \
            "GGC_CAP_BUILD_GENERATION" not in provider:
        raise RuntimeError("graph_adj does not expose its provider/generation through the AM interface")
    if "GetGgcGraphAdjProvider" not in header:
        raise RuntimeError("public graph_adj provider declaration is missing")
    return {
        "candidate_only_provider": True,
        "common_tableam_mvcc_layer": True,
        "persistent_error_precedes_fallback": True,
        "statistics_handle_propagation": True,
        "files": {
            str(path.relative_to(WORKSPACE)): sha(path)
            for path in (provider_path, batch_path, stats_path, provider_header)
        },
    }


def verify_native(path: Path) -> dict:
    text = path.read_text(encoding="utf-8")
    if "\nf\n" in text:
        raise RuntimeError("native graph_adj provider regression contains a false assertion")
    required = (
        "ref_direct_lookups')::integer=5",
        "ref_fallbacks')::integer=5",
        "graph_adj found a conflicting persistent VertexRef",
        "visibility_rejects')::integer=1",
        "hot_redirects')::integer=1",
        "base_entries')::integer=4",
        "stages'->1->>'ref_direct_lookups",
        "count(*)=24",
    )
    missing = [token for token in required if token not in text]
    if missing:
        raise RuntimeError("native graph_adj provider evidence is incomplete: " + repr(missing))
    if "FROM ggc_adj_in_stats" not in text:
        raise RuntimeError("native regression omits independent IN statistics")
    if text.splitlines().count("t") != 9:
        raise RuntimeError("native graph_adj provider assertion count changed")
    return {"boolean_assertions": 9, "expected_structural_error": True}


def setup_fixture() -> None:
    sql("candidate", r'''
DROP SCHEMA IF EXISTS g1055_check CASCADE;
DROP EXTENSION IF EXISTS test_graph_core CASCADE;
CREATE EXTENSION test_graph_core;
CREATE SCHEMA g1055_check;
SET search_path=g1055_check,public;
CREATE TABLE vertices(id bigint PRIMARY KEY);
CREATE TABLE edges(
 id bigint PRIMARY KEY,src bigint NOT NULL,dst bigint NOT NULL,
 payload bigint) WITH(fillfactor=40);
INSERT INTO vertices SELECT i FROM generate_series(1,9) i;
INSERT INTO edges VALUES
 (101,1,2,1),(102,1,3,1),(103,2,4,1),(104,3,4,1),
 (105,4,5,1),(106,4,6,1),(107,5,7,1),(108,6,7,1);
CREATE INDEX edges_adj ON edges USING graph_adj(src,dst);
CREATE PROPERTY GRAPH graph
 VERTEX TABLES(vertices KEY(id))
 EDGE TABLES(edges KEY(id)
  SOURCE KEY(src) REFERENCES vertices(id)
  DESTINATION KEY(dst) REFERENCES vertices(id));
''')


def walk(mode: int, hops: int = 4) -> dict:
    return json.loads(sql("candidate",
        "SELECT test_graph_adj_walk('g1055_check.edges',"
        "'g1055_check.edges_adj',ARRAY[1::bigint,1],"
        f"{hops},1,{mode});").strip())


def stale_ref_error(out: Path) -> tuple[dict, dict]:
    argv = [INSTALL / "bin/psql", "-XqAt", "-v", "ON_ERROR_STOP=1",
            "-v", "VERBOSITY=verbose",
            "-d", "graph_lab", "-c",
            "SELECT test_graph_adj_walk('g1055_check.edges',"
            "'g1055_check.edges_adj',ARRAY[1::bigint],2,1,2);"]
    log = out / "persistent-ref-error.log"
    started = now()
    result = subprocess.run([str(item) for item in argv], env=db_env("candidate"),
                            text=True, capture_output=True, timeout=60)
    log.write_text(result.stdout + result.stderr, encoding="utf-8")
    record = {
        "argv": [str(item) for item in argv], "cwd": None,
        "started": started, "finished": now(), "exit_code": result.returncode,
        "log": str(log.relative_to(PACKAGE)), "sha256": sha(log),
        "expected_failure": True,
    }
    write(log.with_suffix(".json"), record)
    return record, {"returncode": result.returncode, "text": log.read_text()}


def create_and_collect_statistics() -> tuple[dict, dict]:
    sql("candidate", r'''
SET search_path=g1055_check,public;
CREATE TABLE stats_state AS
WITH ids AS
(
 SELECT 'g1055_check.graph'::regclass::oid::bigint graph_oid,
        'g1055_check.edges'::regclass::oid::bigint edge_relid,
        'g1055_check.edges_adj'::regclass::oid::bigint index_oid,
        (SELECT oid::bigint FROM pg_roles WHERE rolname=current_user) role_oid
), mapped AS
(
 SELECT ids.*,p.oid::bigint edge_oid
 FROM ids JOIN pg_propgraph_element p
   ON p.pgepgid=ids.graph_oid AND p.pgekind='e'
), scopes AS
(
 SELECT jsonb_build_object(
   'catalog_provider_oid',0,'graph_oid',graph_oid,'edge_element_oid',edge_oid,
   'index_relid',0,'effective_role',role_oid,'endpoint_domain',edge_relid,
   'scope_kind',1,'direction',1,'path_mode',1,'result_mode',1,
   'catalog_generation',7,'security_generation',11,'build_generation',0,
   'semantic_fingerprint',98766) logical_scope,
  jsonb_build_object(
   'catalog_provider_oid',0,'graph_oid',graph_oid,'edge_element_oid',edge_oid,
   'index_relid',index_oid,'effective_role',role_oid,'endpoint_domain',edge_relid,
   'scope_kind',2,'direction',1,'path_mode',1,'result_mode',1,
   'catalog_generation',7,'security_generation',11,'build_generation',1,
   'semantic_fingerprint',98766) physical_scope
 FROM mapped
), configured AS
(
 SELECT *,jsonb_build_object(
   'seed',20260921,'seed_target',9,'hops',4,
   'predicate_attnum',4,'predicate_min',-100,'predicate_max',100,
   'predicate_clause_id',1,'max_scan_pages',1000,
   'max_examined_entries',100000,'max_bindings',10000,
   'max_bindings_per_seed',5000,'max_entries_per_seed',5000,
   'max_memory_bytes',67108864,'max_payload_bytes',1048576,
   'max_profiles',64,'max_parent_batch',8,'deadline_ms',10000,
   'lock_timeout_ms',1000) options
 FROM scopes
)
SELECT logical_scope,physical_scope,options,
       pg_graph_statistic_create(logical_scope,'{}'::jsonb) logical_oid,
       pg_graph_statistic_create(physical_scope,'{}'::jsonb) physical_oid
FROM configured;
''')
    collected = json.loads(sql("candidate",
        "SELECT pg_graph_statistic_collect_path(logical_oid,physical_oid,options) "
        "FROM g1055_check.stats_state;").strip())
    profiles = json.loads(sql("candidate", r'''
WITH document AS
(
 SELECT pg_graph_statistic_read(physical_oid,physical_scope) d
 FROM g1055_check.stats_state
), profiles AS
(
 SELECT p FROM document,jsonb_array_elements(d->'profiles') p
)
SELECT jsonb_build_object(
 'profile_count',count(*),
 'physical_entry_profile_count',count(*) FILTER
   (WHERE (p->>'metric_kind')::integer=220),
 'all_nonnegative',bool_and((p->'estimate'->>'expected')::numeric>=0))
FROM profiles;
''').strip())
    return collected, profiles


def main() -> None:
    contract = read(GOLDEN)
    out = run_dir("check-G1-055")
    lab = PACKAGE / "scripts/lab.py"
    commands: list[dict] = []
    started = False
    try:
        commands.append(run_logged([sys.executable, lab, "stop", "--variant=candidate"],
                                   out, "pre-stop", timeout=120))
        commands.append(run_logged([sys.executable, lab, "build", "--variant=candidate",
                                    "--jobs=4"], out, "build", timeout=1800))
        make = ["make", "-C", MODULE, "USE_PGXS=1",
                f"PG_CONFIG={INSTALL / 'bin/pg_config'}"]
        for label, arguments in (("clean", ["clean"]), ("build", ["-j4"]),
                                 ("install", ["install"])):
            commands.append(run_logged(make + arguments, out, "module-" + label,
                                       timeout=240))
        commands.append(run_logged([sys.executable, lab, "start", "--variant=candidate"],
                                   out, "start", timeout=180))
        started = True
        commands.append(run_logged(make + ["installcheck", "REGRESS=graph_adj_provider"],
                                   out, "native-regression", env=db_env("candidate"),
                                   timeout=300))
        native_path = out / "native-results.out"
        shutil.copyfile(MODULE / "results/graph_adj_provider.out", native_path)
        native = verify_native(native_path)

        setup_fixture()
        normal = walk(0)
        foreign = walk(1)
        expected_normal = contract["normal"]
        if normal["provider"] != contract["provider"] or \
                normal["final_keys"] != expected_normal["final_keys"]:
            raise RuntimeError("selected graph_adj provider returned the wrong four-hop result")
        for key in ("final_binding_count", "directory_lookups",
                    "ref_direct_lookups", "ref_fallbacks",
                    "shared_parent_requests"):
            if normal[key] != expected_normal[key]:
                raise RuntimeError(f"normal stable-ref counter mismatch for {key}")
        if normal["physical_candidates"] != normal["provider_physical_candidates"]:
            raise RuntimeError("common batch and provider physical candidate counts disagree")
        if foreign["final_keys"] != normal["final_keys"]:
            raise RuntimeError("cross-provider key fallback changed the result bag/order")
        for key, expected in contract["cross_provider"].items():
            if foreign[key] != expected:
                raise RuntimeError(f"cross-provider fallback counter mismatch for {key}")

        stale_record, stale = stale_ref_error(out)
        commands.append(stale_record)
        if stale["returncode"] == 0 or \
                contract["persistent_error_sqlstate"] not in stale["text"] or \
                contract["persistent_error_text"] not in stale["text"]:
            raise RuntimeError("persistent same-scope ref did not fail structurally")

        sql("candidate",
            "UPDATE g1055_check.edges SET payload=payload+10 WHERE id=107;"
            "DELETE FROM g1055_check.edges WHERE id=108;"
            "INSERT INTO g1055_check.edges VALUES (109,5,8,1),(110,8,9,1);")
        mvcc_first = walk(0)
        mvcc_second = walk(0)
        oracle_bag = json.loads(sql("candidate", r'''
WITH RECURSIVE walk(instance,depth,key) AS
(
 SELECT instance,0,1::bigint FROM generate_series(1,2) instance
 UNION ALL
 SELECT w.instance,w.depth+1,e.dst
 FROM walk w JOIN g1055_check.edges e ON e.src=w.key WHERE w.depth<4
)
SELECT to_jsonb(array_agg(key ORDER BY key)) FROM walk WHERE depth=4;
''').strip())
        expected_mvcc = contract["mvcc"]
        if mvcc_first["final_keys"] != expected_mvcc["ordered_final_keys"] or \
                mvcc_second["final_keys"] != mvcc_first["final_keys"] or \
                sorted(mvcc_first["final_keys"]) != oracle_bag or \
                oracle_bag != expected_mvcc["sorted_bag"]:
            raise RuntimeError("MVCC graph batch did not preserve the SQL bag and stable order")
        for key in ("visibility_rejects", "hot_redirects",
                    "physical_candidates", "tableam_fetches"):
            if mvcc_first[key] != expected_mvcc[key]:
                raise RuntimeError(f"MVCC physical counter mismatch for {key}")

        compactions = [json.loads(sql("candidate",
            "SELECT graph_adj_debug_compact('g1055_check.edges_adj',"
            f"{key},true,0);").strip()) for key in (1, 4)]
        if not all(item["found"] and item["compacted"] for item in compactions):
            raise RuntimeError("packed Base preparation did not compact both endpoints")
        index_info = json.loads(sql("candidate",
            "SELECT graph_adj_index_info('g1055_check.edges_adj');").strip())
        packed_walk = walk(0)
        expected_packed = contract["packed"]
        if index_info["base_entries"] != expected_packed["base_entries"] or \
                not index_info["packed_small_segments"] or \
                packed_walk["physical_candidates"] != expected_packed["physical_candidates"] or \
                packed_walk["heap_page_groups"] < expected_packed["minimum_heap_page_groups"]:
            raise RuntimeError("packed-page batch physical profile changed")

        collected, profiles = create_and_collect_statistics()
        expected_stats = contract["statistics"]
        stages = collected["stages"]
        if collected["status"] != "READY" or not collected["published"] or \
                collected["complete_depth"] != expected_stats["complete_depth"] or \
                [stage["directory_lookups"] for stage in stages] != \
                expected_stats["stage_directory_lookups"] or \
                [stage["ref_direct_lookups"] for stage in stages] != \
                expected_stats["stage_ref_direct_lookups"] or \
                [stage["X"] for stage in stages] != \
                expected_stats["stage_physical_entries"]:
            raise RuntimeError("graph_adj statistics collection did not retain key/ref work")
        if profiles["profile_count"] != expected_stats["profile_count"] or \
                profiles["physical_entry_profile_count"] != \
                expected_stats["physical_entry_profile_count"] or \
                not profiles["all_nonnegative"]:
            raise RuntimeError("packed graph_adj physical profiles were not published")

        feature = {
            "normal": normal, "cross_provider": foreign,
            "persistent_error": stale,
            "mvcc_first": mvcc_first, "mvcc_second": mvcc_second,
            "oracle_bag": oracle_bag, "compactions": compactions,
            "index_info": index_info, "packed_walk": packed_walk,
        }
        statistics = {"collection": collected, "profiles": profiles}
        sources = source_contracts()
        write(out / "feature-execution.json", feature)
        write(out / "statistics.json", statistics)
        write(out / "source-contracts.json", sources)
        write(out / "results.json", {
            "native": native, "feature": feature, "statistics": statistics,
            "source_contracts": sources,
            "postgres_sha256": sha(INSTALL / "bin/postgres"),
            "module_sha256": sha(INSTALL / "lib/postgresql/test_graph_core.so"),
            "finished": now(),
        })
        sql("candidate", "DROP SCHEMA g1055_check CASCADE;")
        commands.append(run_logged([sys.executable, lab, "stop", "--variant=candidate"],
                                   out, "post-stop", timeout=120))
        started = False
        write(out / "summary.json", {
            "status": "PASS", "task_id": TASK_ID, "finished": now(),
            "results_sha256": sha(out / "results.json"), "commands": commands,
        })
        files = (GOLDEN, native_path, out / "native-regression.log",
                 out / "persistent-ref-error.log", out / "feature-execution.json",
                 out / "statistics.json", out / "source-contracts.json",
                 out / "results.json")
        print(json.dumps({
            "task_id": TASK_ID,
            "status": "PASS",
            "assertions_passed": [
                "selected_stable_ref_provider",
                "four_hop_key_ref_counters",
                "cross_provider_safe_key_fallback",
                "persistent_ref_error_no_retry",
                "packed_batch_physical_profile",
                "heap_mvcc_bag_order_preserved",
            ],
            "actual_feature_execution": evidence(out / "feature-execution.json"),
            "selected_feature_plan_evidence": evidence(out / "feature-execution.json"),
            "statistics_evidence": evidence(out / "statistics.json"),
            "evidence_files": [evidence(path) for path in files],
        }, ensure_ascii=False, sort_keys=True))
    finally:
        if started:
            run_logged([sys.executable, lab, "stop", "--variant=candidate"],
                       out, "failure-stop", timeout=120)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, KeyError,
            subprocess.SubprocessError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
