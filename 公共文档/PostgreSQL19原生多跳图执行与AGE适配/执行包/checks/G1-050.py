#!/usr/bin/env python3
"""Verify exact GraphPath boundaries, natural CBO, setrefs, and parameterization."""
from __future__ import annotations

import json
from pathlib import Path
import shutil
import subprocess
import sys

SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS))
from common import PACKAGE, WORKSPACE, now, read, run_dir, run_logged, sha, write  # noqa: E402
from lab import db_env  # noqa: E402

TASK_ID = "G1-050"
SOURCE = WORKSPACE / ".worktrees/pg19-graph-core/postgres19"
INSTALL = WORKSPACE / ".install/pg19-graph-core/candidate"
MODULE = SOURCE / "src/test/modules/test_graph_core"
GOLDEN = PACKAGE / "golden/G1-050/contracts.json"


def evidence(path: Path) -> dict:
    return {"path": str(path.relative_to(PACKAGE)), "sha256": sha(path)}


def find_graph(node: object) -> dict | None:
    if isinstance(node, dict):
        if node.get("Custom Plan Provider") == "GraphExpandFixed":
            return node
        for value in node.values():
            found = find_graph(value)
            if found:
                return found
    elif isinstance(node, list):
        for value in node:
            found = find_graph(value)
            if found:
                return found
    return None


def source_contracts() -> dict:
    core_path = SOURCE / "src/backend/optimizer/path/graphpath.c"
    header_path = SOURCE / "src/include/optimizer/graphpath.h"
    shim_path = MODULE / "test_graph_fixed.c"
    core = core_path.read_text(encoding="utf-8")
    header = header_path.read_text(encoding="utf-8")
    shim = shim_path.read_text(encoding="utf-8")
    required_core = (
        "bms_equal(request->covered_relids, request->joinrel->relids)",
        "same_restrictinfo_set(request->available_quals",
        "bms_equal(request->required_outer",
        "get_joinrel_parampathinfo(request->root",
        "GgcEstimateGraphPath(&request->estimate",
        "path->custom_restrictinfo = param_quals",
    )
    missing = [token for token in required_core if token not in core]
    if missing:
        raise RuntimeError("GraphPath core contract is incomplete: " + repr(missing))
    if "rel->pathlist = NIL" in core or "joinrel->pathlist = NIL" in shim:
        raise RuntimeError("G1-050 still forces GraphPath by deleting ordinary alternatives")
    if "add_path(joinrel, &result.path->path)" not in shim or \
            "scan->custom_scan_tlist = tlist" not in shim:
        raise RuntimeError("join alternative competition or standard setrefs handoff is missing")
    if "GgcGraphPathRequest" not in header or "GgcGraphPathResult" not in header:
        raise RuntimeError("public GraphPath construction contract is missing")
    return {
        "exact_relids": True,
        "exact_restrictinfo_set": True,
        "standard_param_path_info": True,
        "ordinary_pathlist_preserved": True,
        "standard_custom_scan_tlist_setrefs": True,
        "files": {
            str(path.relative_to(WORKSPACE)): sha(path)
            for path in (core_path, header_path, shim_path)
        },
    }


def replay_sql() -> str:
    native = (MODULE / "sql/graph_cbo.sql").read_text(encoding="utf-8")
    cleanup = "SELECT pg_graph_statistic_drop(logical_oid) IS NOT NULL,"
    if cleanup not in native:
        raise RuntimeError("native CBO regression cleanup marker is missing")
    prefix = native.split(cleanup, 1)[0]
    return prefix + r'''
CREATE OR REPLACE FUNCTION pg_temp.ggc_plan_analyze(p_query text)
RETURNS jsonb LANGUAGE plpgsql AS $$
DECLARE result jsonb;
BEGIN
 EXECUTE 'EXPLAIN (ANALYZE, FORMAT JSON, COSTS OFF, TIMING OFF, SUMMARY OFF) ' ||
         p_query INTO result;
 RETURN result;
END
$$;
WITH selected AS
(
 SELECT pg_temp.ggc_plan_analyze($q$
  SELECT count(*),sum(r.anchor),sum(e1.id),sum(e2.id)
  FROM ggc_cbo_request r
  JOIN ggc_cbo_edge e1 ON e1.src=r.anchor
  JOIN ggc_cbo_edge e2 ON e2.src=e1.dst$q$) document
), outer_join AS
(
 SELECT pg_temp.ggc_plan($q$
  SELECT g.anchor,g.e1id,g.e2id,g.e2dst,d.name
  FROM
  (SELECT r.anchor,e1.id e1id,e2.id e2id,e2.dst e2dst
   FROM ggc_cbo_request r
   JOIN ggc_cbo_edge e1 ON e1.src=r.anchor
   JOIN ggc_cbo_edge e2 ON e2.src=e1.dst OFFSET 0) g
  LEFT JOIN ggc_cbo_dimension d ON d.id=g.e2dst$q$) document
), lateral_doc AS
(
 SELECT pg_temp.ggc_plan($q$
  SELECT s.id,e1.id,e2.id
  FROM ggc_cbo_seed s
  CROSS JOIN LATERAL
  (SELECT r.anchor FROM ggc_cbo_request r
   WHERE r.anchor=s.id OFFSET 0) r
  JOIN ggc_cbo_edge e1 ON e1.src=r.anchor
  JOIN ggc_cbo_edge e2 ON e2.src=e1.dst$q$) document
), actual AS
(
 SELECT count(*) rows, sum(r.anchor) anchor_sum,
        sum(e1.id) first_edge_sum, sum(e2.id) second_edge_sum
 FROM ggc_cbo_request r
 JOIN ggc_cbo_edge e1 ON e1.src=r.anchor
 JOIN ggc_cbo_edge e2 ON e2.src=e1.dst
)
SELECT jsonb_build_object(
 'selected_plan',(SELECT document FROM selected),
 'outer_plan',(SELECT document FROM outer_join),
 'lateral_plan',(SELECT document FROM lateral_doc),
 'actual',(SELECT to_jsonb(actual) FROM actual));
'''


def verify_native(path: Path) -> dict:
    text = path.read_text(encoding="utf-8")
    if "ERROR:" in text or "\nf\n" in text:
        raise RuntimeError("native graph CBO regression contains an error or false assertion")
    required = (
        "Graph Confidence'='unknown'",
        "Graph Required Outer')::int=1",
        "NOT jsonb_path_exists",
        "Graph Covered Relids')::int=3",
        "Graph Consumed Quals')::int=2",
        "count(*)=5 AND count(d.name)=4",
    )
    missing = [token for token in required if token not in text]
    if missing:
        raise RuntimeError("native graph CBO evidence is incomplete: " + repr(missing))
    return {"boolean_assertions": text.splitlines().count("t"),
            "outer_join": True, "unknown_confidence": True,
            "natural_fallback": True}


def verify_selected(document: dict, contract: dict) -> dict:
    selected = find_graph(document["selected_plan"])
    outer = find_graph(document["outer_plan"])
    lateral = find_graph(document["lateral_plan"])
    if not selected or not outer or not lateral:
        raise RuntimeError("selected, outer-join, or lateral plan lacks GraphExpandFixed")
    if selected.get("Graph Covered Relids") != contract["covered_relids"] or \
            selected.get("Graph Consumed Quals") != contract["consumed_quals"] or \
            selected.get("Graph Required Outer") != contract["required_outer_unparameterized"]:
        raise RuntimeError("selected GraphPath boundary differs from the frozen contract")
    if selected.get("Graph Confidence") != contract["required_confidence"] or \
            not selected.get("Graph Cost Profile", "").startswith("calibrated v1 generation ") or \
            "logical=READY" not in selected.get("Graph Statistics", "") or \
            "physical=READY" not in selected.get("Graph Statistics", ""):
        raise RuntimeError("selected GraphPath did not consume READY scoped statistics")
    if selected.get("Actual Rows") != contract["actual_rows"]:
        raise RuntimeError("EXPLAIN ANALYZE did not execute the selected graph node")
    if lateral.get("Graph Required Outer") != contract["required_outer_lateral"]:
        raise RuntimeError("parameterized GraphPath lost required_outer")
    actual = document["actual"]
    for field in ("rows", "anchor_sum", "first_edge_sum", "second_edge_sum"):
        if actual[field] != contract["actual_" + field]:
            raise RuntimeError(f"selected GraphPath result mismatch for {field}")
    return {"selected": selected, "outer": outer, "lateral": lateral,
            "actual": actual}


def main() -> None:
    out = run_dir("check-G1-050")
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
        commands.append(run_logged(make + ["installcheck", "REGRESS=graph_cbo"],
                                   out, "native-regression", env=db_env("candidate"),
                                   timeout=300))
        native_path = out / "native-results.out"
        shutil.copyfile(MODULE / "results/graph_cbo.out", native_path)
        native = verify_native(native_path)

        replay = out / "selected-feature-replay.sql"
        replay.write_text(replay_sql(), encoding="utf-8")
        commands.append(run_logged([
            INSTALL / "bin/psql", "-XqAt", "-v", "ON_ERROR_STOP=1",
            "-d", "contrib_regression", "-f", replay,
        ], out, "selected-feature", env=db_env("candidate"), timeout=180))
        lines = [line for line in (out / "selected-feature.log").read_text().splitlines()
                 if line.startswith("{")]
        if len(lines) != 1:
            raise RuntimeError("selected feature replay did not emit one JSON document")
        document = json.loads(lines[0])
        selected = verify_selected(document, read(GOLDEN))
        plan_path = out / "selected-feature-plan.json"
        write(plan_path, document)
        sources = source_contracts()
        write(out / "source-contracts.json", sources)
        write(out / "results.json", {
            "native": native, "selected_feature": selected,
            "source_contracts": sources,
            "postgres_sha256": sha(INSTALL / "bin/postgres"),
            "module_sha256": sha(INSTALL / "lib/postgresql/test_graph_core.so"),
            "finished": now(),
        })
        commands.append(run_logged([sys.executable, lab, "stop", "--variant=candidate"],
                                   out, "post-stop", timeout=120))
        started = False
        write(out / "summary.json", {
            "status": "PASS", "task_id": TASK_ID, "finished": now(),
            "results_sha256": sha(out / "results.json"), "commands": commands,
        })
        files = (GOLDEN, native_path, out / "native-regression.log", replay,
                 out / "selected-feature.log", plan_path,
                 out / "source-contracts.json", out / "results.json")
        print(json.dumps({
            "task_id": TASK_ID,
            "status": "PASS",
            "assertions_passed": [
                "relids_quals_exact", "required_outer",
                "natural_cbo_alternatives", "setrefs_correct",
                "statistics_profile_consumed", "unknown_confidence_explain",
            ],
            "selected_feature_plan_evidence": evidence(plan_path),
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
