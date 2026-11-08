#!/usr/bin/env python3
"""Verify bounded planner statistics loading, costing, calibration, and feedback."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS))
from common import PACKAGE, WORKSPACE, now, read, run_dir, run_logged, sha, write  # noqa: E402
from lab import db_env  # noqa: E402

TASK_ID = "G1-048"
SOURCE = WORKSPACE / ".worktrees/pg19-graph-core/postgres19"
INSTALL = WORKSPACE / ".install/pg19-graph-core/candidate"
RUNTIME = WORKSPACE / ".runtime/pg19-graph-core/candidate"
MODULE = SOURCE / "src/test/modules/test_graph_core"
GOLDEN = PACKAGE / "golden/G1-048"
CALIBRATOR = PACKAGE / "scripts/graph_stats_calibrate.py"


def evidence(path: Path) -> dict:
    return {"path": str(path.relative_to(PACKAGE)), "sha256": sha(path)}


def source_contracts() -> dict:
    loader_path = SOURCE / "src/backend/optimizer/path/graphstats.c"
    cost_path = SOURCE / "src/backend/optimizer/path/costgraph.c"
    feedback_path = SOURCE / "src/backend/statistics/graph_stats_feedback.c"
    loader = loader_path.read_text(encoding="utf-8")
    cost = cost_path.read_text(encoding="utf-8")
    feedback = feedback_path.read_text(encoding="utf-8")

    opened = set(re.findall(r"table_open\(([^,\n]+)", loader))
    if opened != {"GraphStatisticRelationId", "GraphStatisticDataRelationId"}:
        raise RuntimeError(f"planner loader opens a non-statistics relation: {sorted(opened)}")
    forbidden_loader = ("GgcBatch", "GetGgc", "table_beginscan",
                        "heap_beginscan", "index_beginscan")
    if any(re.search(r"\b" + re.escape(token) + r"\s*\(", loader)
           for token in forbidden_loader) or \
            loader.count("systable_beginscan") != 2 or \
            "loaded->decoded_bytes > max_bytes" not in loader or \
            "loaded->loaded_profiles > max_profiles" not in loader:
        raise RuntimeError("planner profile load is not a bounded catalog-index-only operation")

    if not all(token in loader for token in (
            "LOAD_LOGICAL", "LOAD_PHYSICAL", "LOAD_COST",
            "loaded->logical_state", "loaded->physical_state",
            "loaded->cost_state")):
        raise RuntimeError("logical, physical, and cost profiles are not independently loaded")
    if any(token in cost for token in (
            "clauselist_selectivity", "clause_selectivity", "cost_index",
            "genericcostestimate", "cost_qual_eval")):
        raise RuntimeError("graph path cost applies a second generic selectivity or index cost")
    if cost.count("target->heap_page_groups * result->cost_profile.heap_page_group") != 1 or \
            cost.count("target->edge_heap_fetches * result->cost_profile.heap_fetch") != 1:
        raise RuntimeError("heap page I/O or TableAM fetch work is not charged exactly once")
    if not all(token in cost for token in (
            "request->ref_fractions[stage]",
            "target->ref_requests = target->requests * ref_fraction",
            "target->key_requests = target->requests - target->ref_requests",
            "logical_ready = loaded.logical_state == GGC_STATS_READY",
            "physical_ready = loaded.physical_state == GGC_STATS_READY")):
        raise RuntimeError("K/R scope or logical/physical fallback separation is missing")
    if not all(token in feedback for token in (
            "buffer->count == buffer->capacity", "buffer->dropped++",
            "if (buffer->records[i].complete)", "summary->timing_records")):
        raise RuntimeError("feedback buffer is not bounded or does not retain completeness")
    return {
        "planner_table_open_targets": sorted(opened),
        "planner_index_scans": loader.count("systable_beginscan"),
        "forbidden_loader_tokens_absent": list(forbidden_loader),
        "generic_selectivity_or_index_cost_tokens_absent": [
            "clauselist_selectivity", "clause_selectivity", "cost_index",
            "genericcostestimate", "cost_qual_eval"],
        "heap_page_io_charge_occurrences": 1,
        "heap_fetch_cpu_charge_occurrences": 1,
        "files": {
            str(path.relative_to(WORKSPACE)): sha(path)
            for path in (loader_path, cost_path, feedback_path)
        },
    }


def verify_calibration(output: dict, source: dict, contract: dict) -> dict:
    if output.get("format_version") != 1 or \
            output.get("cost_profile_version") != 1 or \
            output.get("algorithm") != "nonnegative_coordinate_descent_v1":
        raise RuntimeError("calibrator emitted an unsupported contract")
    normalization = output["normalization"]
    if abs(normalization["us_per_pg_cost_unit"] -
           contract["expected_us_per_pg_cost_unit"]) > 1e-12:
        raise RuntimeError("raw normalization samples were not converted to PG cost units")
    coefficients = output["fit"]["coefficients_pg_cost_units"]
    if set(coefficients) != set(contract["expected_coefficients_pg_cost_units"]):
        raise RuntimeError("cost coefficient set is incomplete")
    tolerance = contract["coefficient_absolute_tolerance"]
    for component, expected in contract["expected_coefficients_pg_cost_units"].items():
        actual = coefficients[component]
        if actual < 0 or abs(actual - expected) > tolerance:
            raise RuntimeError(f"bad non-negative coefficient for {component}: {actual}")
    if output["fit"]["matrix_rank"] != len(coefficients) or \
            not output["fit"]["converged"]:
        raise RuntimeError("calibration fit is rank-deficient or unconverged")
    report = output["holdout_estimate_error_report"]
    if report["case_count"] != contract["expected_holdout_count"] or \
            report["max_relative_error"] > contract["holdout_max_relative_error"]:
        raise RuntimeError("independent holdout error exceeded the frozen contract")
    train_ids = {item["id"] for item in source["train"]}
    holdout_ids = {item["id"] for item in source["holdout"]}
    report_ids = {item["id"] for item in report["cases"]}
    if train_ids & holdout_ids or report_ids != holdout_ids:
        raise RuntimeError("holdout observations overlap training or were not all reported")
    profiles = output["profiles"]
    if len(profiles) != contract["expected_profile_count"] or \
            {item["metric_kind"] for item in profiles} != set(range(300, 307)) or \
            any(item["estimate"]["expected"] < 0 or
                item["estimate"]["censored"] or
                item["estimate"]["state"] != 1 for item in profiles):
        raise RuntimeError("publishable cost profiles are incomplete or unsafe")
    if len(source["train"]) != contract["expected_train_count"]:
        raise RuntimeError("frozen training fixture count changed")
    return {
        "normalization": normalization,
        "coefficients_pg_cost_units": coefficients,
        "train_error_report": output["fit"]["train_error_report"],
        "holdout_estimate_error_report": report,
        "training_ids": sorted(train_ids),
        "holdout_ids": sorted(holdout_ids),
        "formal_performance_evidence": False,
    }


def verify_native_result(path: Path) -> dict:
    text = path.read_text(encoding="utf-8")
    required = (
        "bounded graph statistics planner load exceeded its selected profile budget",
        "bounded graph statistics planner load exceeded its byte budget",
        "REINDEX INDEX ggc_cost_edge_out;",
        '"logical_state":1,"physical_state":1,"cost_state":1',
        '"loaded_profiles":47',
        '"K":1,"R":0',
        '"first":true,"second":true,"third":false',
    )
    missing = [token for token in required if token not in text]
    if missing:
        raise RuntimeError("native cost regression lacks contract evidence: " + repr(missing))
    diagnostic_lines = [line for line in text.splitlines()
                        if line.startswith('{"logical_state"')]
    if len(diagnostic_lines) != 1:
        raise RuntimeError("native regression did not emit one READY cost diagnostic")
    diagnostic = json.loads(diagnostic_lines[0])
    stages = diagnostic["stages"]
    if len(stages) != 4 or not diagnostic["calibrated"] or \
            diagnostic["uncertainty_unknown"] or \
            any(stage["logical_fallback"] or stage["physical_fallback"]
                for stage in stages):
        raise RuntimeError("READY path unexpectedly used a fallback")
    if text.splitlines().count("t") != 9:
        raise RuntimeError("missing READY/MISSING/PARTIAL/STALE/feedback native assertions")
    return {
        "ready_diagnostic": diagnostic,
        "boolean_assertions": 9,
        "profile_budget_error": True,
        "byte_budget_error": True,
        "missing_partial_stale_exercised": True,
    }


def main() -> None:
    out = run_dir("check-G1-048")
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

        catalog_digest = hashlib.sha256(
            (INSTALL / "share/postgresql/postgres.bki").read_bytes()).hexdigest()
        marker = RUNTIME / ".catalog-bki.sha256"
        previous = marker.read_text().strip() if marker.exists() else None
        if previous != catalog_digest and (RUNTIME / "data").exists():
            archive = RUNTIME / "catalog-archives" / ("pre-" + out.name)
            archive.parent.mkdir(exist_ok=True)
            if archive.exists():
                raise RuntimeError("refusing to overwrite candidate catalog archive")
            (RUNTIME / "data").rename(archive)
        commands.append(run_logged([sys.executable, lab, "start", "--variant=candidate"],
                                   out, "start", timeout=180))
        started = True
        marker.write_text(catalog_digest + "\n")
        commands.append(run_logged(make + ["installcheck"], out, "native-regression",
                                   env=db_env("candidate"), timeout=300))
        native_result = out / "native-results.out"
        shutil.copyfile(MODULE / "results/graph_stats_cost.out", native_result)
        native = verify_native_result(native_result)

        calibration_input = GOLDEN / "calibration_input.json"
        calibration_output = out / "calibrated-profile.json"
        commands.append(run_logged([sys.executable, CALIBRATOR, calibration_input,
                                    calibration_output], out, "calibration", timeout=60))
        calibration_document = read(calibration_output)
        calibration = verify_calibration(calibration_document,
                                         read(calibration_input),
                                         read(GOLDEN / "contracts.json"))
        profiles_literal = json.dumps(
            calibration_document["profiles"], ensure_ascii=False,
            sort_keys=True, separators=(",", ":")).replace("'", "''")
        publish_sql = out / "publish-calibration.sql"
        publish_sql.write_text(
            "\\set ON_ERROR_STOP on\n"
            "SELECT pg_graph_statistic_publish((\n"
            "  SELECT oid FROM pg_graph_statistic\n"
            "  WHERE gstkind=3 AND gstowner=(SELECT oid FROM pg_roles "
            "WHERE rolname=current_user) ORDER BY oid DESC LIMIT 1\n"
            f"),'{profiles_literal}'::jsonb) > 0;\n"
            "SELECT count(*)=7 FROM pg_graph_statistic_data d\n"
            "JOIN pg_graph_statistic h ON h.oid=d.gstdstatid "
            "AND h.gstgeneration=d.gstdgeneration\n"
            "WHERE h.gstkind=3 AND h.gstowner=(SELECT oid FROM pg_roles "
            "WHERE rolname=current_user);\n",
            encoding="utf-8",
        )
        publish_env = db_env("candidate", "contrib_regression")
        commands.append(run_logged([
            INSTALL / "bin/psql", "-XAt", "-v", "ON_ERROR_STOP=1",
            "-d", "contrib_regression", "-f", publish_sql,
        ], out, "publish-calibration", env=publish_env, timeout=60))
        publish_lines = [line.strip() for line in
                         (out / "publish-calibration.log").read_text().splitlines()
                         if line.strip()]
        if publish_lines != ["t", "t"]:
            raise RuntimeError("calibrator output was not accepted as seven live cost profiles")
        sources = source_contracts()

        statistics = {
            "native_catalog_execution": native,
            "offline_pg_cost_calibration": calibration,
            "calibration_profiles_published": True,
            "planner_source_contract": sources,
            "scope_note": (
                "The frozen calibration data validates units, constraints, and holdout "
                "plumbing; it is deliberately not a product performance claim."
            ),
        }
        write(out / "statistics.json", statistics)
        source_manifest = {
            str(path.relative_to(WORKSPACE)): sha(path)
            for path in (
                SOURCE / "src/backend/optimizer/path/graphstats.c",
                SOURCE / "src/backend/optimizer/path/costgraph.c",
                SOURCE / "src/backend/statistics/graph_stats_feedback.c",
                SOURCE / "src/test/modules/test_graph_core/sql/graph_stats_cost.sql",
                SOURCE / "src/test/modules/test_graph_core/expected/graph_stats_cost.out",
                CALIBRATOR,
                GOLDEN / "calibration_input.json",
                GOLDEN / "contracts.json",
            )
        }
        write(out / "source-manifest.json", source_manifest)
        write(out / "results.json", {
            "native": native,
            "calibration": calibration,
            "source_contracts": sources,
            "postgres_sha256": sha(INSTALL / "bin/postgres"),
            "catalog_bki_sha256": catalog_digest,
            "finished": now(),
        })
        commands.append(run_logged([sys.executable, lab, "stop", "--variant=candidate"],
                                   out, "post-stop", timeout=120))
        started = False
        write(out / "summary.json", {
            "status": "PASS", "task_id": TASK_ID, "finished": now(),
            "results_sha256": sha(out / "results.json"), "commands": commands,
        })
        files = (
            GOLDEN / "calibration_input.json", GOLDEN / "contracts.json",
            out / "native-results.out", out / "native-regression.log",
            out / "calibrated-profile.json", out / "publish-calibration.sql",
            out / "publish-calibration.log", out / "statistics.json",
            out / "source-manifest.json", out / "results.json",
        )
        print(json.dumps({
            "task_id": TASK_ID,
            "status": "PASS",
            "assertions_passed": [
                "bounded_profile_load_no_graph_scan",
                "logical_physical_cost_separate",
                "safe_missing_stale_partial_fallback",
                "no_double_selectivity_or_io_cost",
                "key_ref_scope_drives_cost",
                "calibrated_pg_cost_units",
                "feedback_bounded_complete_flag",
                "holdout_estimate_error_report",
            ],
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
