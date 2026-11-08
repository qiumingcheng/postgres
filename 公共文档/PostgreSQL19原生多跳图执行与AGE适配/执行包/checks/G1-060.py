#!/usr/bin/env python3
"""Verify prepared-plan, startup fallback, rescan, and invalidation lifecycle."""
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

TASK_ID = "G1-060"
SOURCE = WORKSPACE / ".worktrees/pg19-graph-core/postgres19"
INSTALL = WORKSPACE / ".install/pg19-graph-core/candidate"
MODULE = SOURCE / "src/test/modules/test_graph_core"
GOLDEN = PACKAGE / "golden/G1-060/contracts.json"


def evidence(path: Path) -> dict:
    return {"path": str(path.relative_to(PACKAGE)), "sha256": sha(path)}


def require_tokens(text: str, tokens: tuple[str, ...], label: str) -> None:
    missing = [token for token in tokens if token not in text]
    if missing:
        raise RuntimeError(f"{label} contract is incomplete: {missing!r}")


def source_contracts() -> dict:
    provider_path = SOURCE / "src/backend/executor/graph_provider.c"
    fixed_path = SOURCE / "src/backend/executor/graph_fixed.c"
    fixed_header_path = SOURCE / "src/include/executor/graph_fixed.h"
    stats_path = SOURCE / "src/backend/optimizer/path/graphstats.c"
    cache_path = SOURCE / "src/backend/utils/cache/plancache.c"
    shim_path = MODULE / "test_graph_fixed.c"
    provider = provider_path.read_text(encoding="utf-8")
    fixed = fixed_path.read_text(encoding="utf-8")
    fixed_header = fixed_header_path.read_text(encoding="utf-8")
    stats = stats_path.read_text(encoding="utf-8")
    cache = cache_path.read_text(encoding="utf-8")
    shim = shim_path.read_text(encoding="utf-8")

    require_tokens(provider, (
        "GgcRegisterAdjProvider", "GgcUnregisterAdjProvider",
        "GgcResolveAdjProvider", "GgcGetIndexAdjProvider",
        "index->rd_indam->amgetgraphprovider",
        "GgcProviderRefEqual",
    ), "provider registry")
    if "GetGgcBtreeAdjProvider" in provider or "GetGgcGraphAdjProvider" in provider:
        raise RuntimeError("common registry still links to concrete provider factories")
    require_tokens(fixed_header, (
        "GgcProviderRef provider", "GgcFixedAccessPlan primary",
        "GgcFixedAccessPlan fallback", "statistics_generation",
        "runtime_retries",
    ), "serialized fixed plan")
    stage_start = fixed_header.index("typedef struct GgcFixedStagePlan")
    stage_end = fixed_header.index("} GgcFixedStagePlan;", stage_start)
    if "GgcAdjProviderV2" in fixed_header[stage_start:stage_end]:
        raise RuntimeError("cached fixed plan still serializes a callback pointer")

    require_tokens(fixed, (
        "GgcResolveAdjProvider(index, &access->provider",
        "try_index_open(access->index_relid, AccessShareLock)",
        "fixed graph stage has no valid startup provider",
        "Keep only the selected index's DDL lock through query execution",
        "GgcEndpointHandleScopeMatches",
        "state->failed = true", "PG_RE_THROW()",
        "validate_execution_scope(state)",
        "revalidate_selected_stage(state, i)",
        "MemoryContextReset(state->arenas[0])",
        "MemoryContextReset(state->arenas[1])",
        "state->next_ordinal = 0",
    ), "executor lifecycle")
    begin_pos = fixed.index("GgcFixedBegin(")
    resolve_pos = fixed.index("resolve_stage(state", begin_pos)
    next_pos = fixed.index("GgcFixedNext(", resolve_pos)
    anchor_pos = fixed.index("load_anchor_batch(state)", next_pos)
    if not begin_pos < resolve_pos < next_pos < anchor_pos:
        raise RuntimeError("provider resolution is not completed before anchor side effects")
    if "runtime_retries++" in fixed:
        raise RuntimeError("runtime provider errors can increment a retry path")

    require_tokens(stats, (
        "record_graph_statistics_dependency",
        "GRAPHSTATISTICOID", "root->glob->invalItems",
    ), "statistics plan dependency")
    require_tokens(cache, (
        "CacheRegisterSyscacheCallback(GRAPHSTATISTICOID",
        "PlanCacheObjectCallback",
    ), "statistics plan invalidation")
    require_tokens(shim, (
        "root->glob->relationOids = list_append_unique_oid",
        "cbo_diagnostic.plan_builds++",
        "deserialize_provider_ref",
    ), "custom plan DDL dependency")
    return {
        "serialized_provider_identity_only": True,
        "startup_resolution_before_child_execution": True,
        "selected_index_ddl_lock": True,
        "runtime_error_no_retry": True,
        "rescan_scope_revalidation": True,
        "statistics_generation_plan_dependency": True,
        "ddl_plan_dependency": True,
        "files": {
            str(path.relative_to(WORKSPACE)): sha(path)
            for path in (provider_path, fixed_path, fixed_header_path,
                         stats_path, cache_path, shim_path)
        },
    }


def verify_native(path: Path, contract: dict) -> dict:
    text = path.read_text(encoding="utf-8")
    if text.splitlines().count("t") != contract["native_boolean_assertions"]:
        raise RuntimeError("native lifecycle boolean assertion count changed")
    if text.splitlines().count("t|t") != contract["native_cleanup_assertions"]:
        raise RuntimeError("native lifecycle cleanup assertion count changed")
    for error in contract["expected_errors"]:
        if text.count("ERROR:  " + error) != 1:
            raise RuntimeError(f"missing or repeated expected lifecycle error: {error}")
    if text.count("ERROR:") != len(contract["expected_errors"]):
        raise RuntimeError("native lifecycle regression contains an unexpected error")

    markers = (
        "generic_plans>=2 AND custom_plans=0",
        f"Graph Startup Revalidations: {contract['startup_revalidations']}",
        f"Graph Startup Fallbacks: {contract['startup_fallbacks']}",
        f"Graph Runtime Retries: {contract['runtime_retries']}",
        f"Graph Rescans: {contract['rescan_count']}",
        f"Graph Rescan Revalidations: {contract['rescan_revalidations']}",
        "Stage 1 Batch Summary: executions=1 input=1 requests=1 candidates=2 pages=1 output=2 startup_fallback=false handles=0 scope_fallbacks=0 directory=1 direct_refs=0",
        "Stage 2 Batch Summary: executions=1 input=2 requests=2 candidates=2 pages=1 output=2 startup_fallback=false handles=2 scope_fallbacks=0 directory=0 direct_refs=2",
        "Stage 3 Batch Summary: executions=1 input=2 requests=1 candidates=2 pages=1 output=4 startup_fallback=false handles=0 scope_fallbacks=2 directory=1 direct_refs=0",
        "Stage 4 Batch Summary: executions=1 input=4 requests=2 candidates=3 pages=1 output=6 startup_fallback=false handles=4 scope_fallbacks=0 directory=0 direct_refs=2",
        "pg_temp.ggc_life_publish_cost(0.000001)",
        "test_graph_cbo_diagnostic()::jsonb->>'plan_builds'",
        "ALTER TABLE ggc_life_request ADD COLUMN ddl_marker integer",
    )
    require_tokens(text, markers, "native lifecycle evidence")
    if "SELECT NOT is_called FROM ggc_life_side_effect;\nt" not in text or \
            "SELECT last_value=1 AND is_called FROM ggc_life_side_effect;\nt" not in text:
        raise RuntimeError("startup failure/fallback did not preserve side-effect ordering")
    return {
        "generic_custom_parameters": True,
        "before_side_effect_fallback": True,
        "runtime_error_no_retry": True,
        "abi_ddl_invalidation": True,
        "handle_scope_and_stats_generation_invalidation": True,
        "boolean_assertions": contract["native_boolean_assertions"],
        "expected_errors": len(contract["expected_errors"]),
    }


def main() -> None:
    contract = read(GOLDEN)
    out = run_dir("check-G1-060")
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
                                       timeout=300))
        commands.append(run_logged([sys.executable, lab, "start", "--variant=candidate"],
                                   out, "start", timeout=180))
        started = True
        commands.append(run_logged(make + ["installcheck", "REGRESS=graph_lifecycle"],
                                   out, "native-regression", env=db_env("candidate"),
                                   timeout=300))
        native_path = out / "native-results.out"
        shutil.copyfile(MODULE / "results/graph_lifecycle.out", native_path)
        native = verify_native(native_path, contract)
        sources = source_contracts()
        source_path = out / "source-contracts.json"
        write(source_path, sources)
        results_path = out / "results.json"
        write(results_path, {
            "native": native,
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
            "results_sha256": sha(results_path), "commands": commands,
        })
        print(json.dumps({
            "task_id": TASK_ID,
            "status": "PASS",
            "assertions_passed": [
                "generic_custom_parameters", "before_side_effect_fallback",
                "runtime_error_no_retry", "abi_ddl_invalidation",
                "handle_scope_and_stats_generation_invalidation",
            ],
            "selected_feature_plan_evidence": evidence(native_path),
            "evidence_files": [evidence(path) for path in
                               (GOLDEN, native_path, out / "native-regression.log",
                                source_path, results_path)],
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
