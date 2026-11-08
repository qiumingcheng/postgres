#!/usr/bin/env python3
"""Verify common graph Nodes and the provider/adapter ABI contracts."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS))

from common import PACKAGE, WORKSPACE, now, read, run_dir, run_logged, sha, write  # noqa: E402
from lab import db_env  # noqa: E402


TASK_ID = "G1-010"
SOURCE = WORKSPACE / ".worktrees/pg19-graph-core/postgres19"
INSTALL = WORKSPACE / ".install/pg19-graph-core/candidate"
BUILD = WORKSPACE / ".build/pg19-graph-core/candidate/postgres19"
RUNTIME = WORKSPACE / ".runtime/pg19-graph-core/candidate"
MODULE = SOURCE / "src/test/modules/test_graph_core"
GOLDEN = PACKAGE / "golden/G1-010/contracts.json"


def evidence(path: Path) -> dict:
    return {"path": str(path.relative_to(PACKAGE)), "sha256": sha(path)}


def node_format_digest() -> str:
    digest = hashlib.sha256()
    for path in (
        INSTALL / "include/postgresql/server/nodes/graphnodes.h",
        INSTALL / "include/postgresql/server/nodes/parsenodes.h",
        INSTALL / "include/postgresql/server/executor/graph_provider.h",
    ):
        digest.update(path.read_bytes())
    return digest.hexdigest()


def refresh_node_format_cluster(out: Path) -> dict:
    """Archive clusters initialized with an older serialized Query layout."""
    data = RUNTIME / "data"
    marker = RUNTIME / ".g1-010-node-format.sha256"
    current = node_format_digest()
    previous = marker.read_text().strip() if marker.exists() else None
    archived = None
    if data.exists() and previous != current:
        archive_root = RUNTIME / "catalog-archives"
        archive_root.mkdir(parents=True, exist_ok=True)
        archived = archive_root / ("pre-" + out.name)
        if archived.exists():
            raise RuntimeError("refusing to overwrite node-format archive: " + str(archived))
        data.rename(archived)
    return {
        "marker": marker,
        "current": current,
        "previous": previous,
        "archived_data": str(archived) if archived else None,
    }


def verify_generated_support(golden: dict) -> dict:
    generated = BUILD / "src/backend/nodes"
    operations = {
        "copy": (generated / "copyfuncs.funcs.c", "_copy"),
        "equal": (generated / "equalfuncs.funcs.c", "_equal"),
        "out": (generated / "outfuncs.funcs.c", "_out"),
        "read": (generated / "readfuncs.funcs.c", "_read"),
        "query_jumble": (generated / "queryjumblefuncs.funcs.c", "_jumble"),
    }
    if set(operations) != set(golden["generated_operations"]):
        raise RuntimeError("golden generated-operation set does not match the checker")

    files: dict[str, dict] = {}
    for operation, (path, prefix) in operations.items():
        text = path.read_text()
        missing = [
            node for node in golden["generated_nodes"]
            if prefix + node not in text
        ]
        if missing:
            raise RuntimeError(f"{operation} generation missing nodes: {missing}")
        if "graph_program" not in text:
            raise RuntimeError(f"Query.graph_program missing from generated {operation}")
        files[operation] = {
            "path": str(path),
            "sha256": sha(path),
            "nodes": list(golden["generated_nodes"]),
            "query_graph_program": True,
        }
    return files


def verify_no_age_dependency(module_so: Path, out: Path) -> dict:
    implementation_sources = [
        SOURCE / "src/include/nodes/graphnodes.h",
        SOURCE / "src/include/executor/graph_provider.h",
        SOURCE / "src/include/executor/graph_adapter.h",
        MODULE / "test_graph_core.c",
    ]
    forbidden_source = re.compile(r"\b(?:age|agtype|graphid)\b", re.IGNORECASE)
    source_hits = {
        str(path): sorted(set(forbidden_source.findall(path.read_text())))
        for path in implementation_sources
        if forbidden_source.search(path.read_text())
    }
    if source_hits:
        raise RuntimeError("common graph implementation depends on AGE vocabulary: " + repr(source_hits))

    ldd = run_logged(["ldd", module_so], out, "module-ldd", cwd=WORKSPACE)
    symbols = run_logged(["nm", "-u", module_so], out, "module-undefined", cwd=WORKSPACE)
    binary_text = (out / "module-ldd.log").read_text() + (out / "module-undefined.log").read_text()
    binary_hits = [token for token in ("age.so", "agtype", "graphid") if token.lower() in binary_text.lower()]
    if binary_hits:
        raise RuntimeError("test module has an AGE runtime dependency: " + repr(binary_hits))
    return {
        "implementation_sources": {str(path): sha(path) for path in implementation_sources},
        "source_hits": source_hits,
        "binary_hits": binary_hits,
        "commands": [ldd, symbols],
    }


def assert_contracts(node: dict, provider: dict, golden: dict) -> None:
    false_node = [name for name in golden["node_boolean_contracts"] if node.get(name) is not True]
    false_provider = [name for name in golden["provider_boolean_contracts"] if provider.get(name) is not True]
    if false_node or false_provider:
        raise RuntimeError(f"contract booleans failed: node={false_node}, provider={false_provider}")
    if node.get("serialized_bytes", 0) <= 0 or node.get("query_id_a", 0) == 0:
        raise RuntimeError("node serialization/query jumble did not produce concrete output")
    if node["query_id_a"] == node["query_id_mapping_changed"]:
        raise RuntimeError("RTI mapping change did not alter the query jumble")
    expected_abi = golden["abi_versions"]
    actual_abi = {
        "adjacency": provider.get("adj_abi_version"),
        "catalog": provider.get("catalog_abi_version"),
        "result": provider.get("result_abi_version"),
    }
    if actual_abi != expected_abi:
        raise RuntimeError(f"ABI versions differ: {actual_abi} != {expected_abi}")
    for field in (
        "provider_ref_size", "handle_size", "open_request_size",
        "estimate_request_size", "catalog_provider_size", "result_adapter_size",
    ):
        if not isinstance(provider.get(field), int) or provider[field] <= 0:
            raise RuntimeError("invalid ABI size: " + field)
    if provider.get("persisted_origin") != 2:
        raise RuntimeError("persistent adjacency provenance value changed")
    if provider.get("estimate_calls") != 1 or provider.get("begin_calls") != 0:
        raise RuntimeError("planner estimate opened executor state")


def main() -> None:
    out = run_dir("check-G1-010")
    golden = read(GOLDEN)
    lab = PACKAGE / "scripts/lab.py"
    python = sys.executable
    commands = []
    started = False
    try:
        commands.append(run_logged(
            [python, lab, "stop", "--variant=candidate"], out, "pre-stop",
            cwd=WORKSPACE, timeout=120,
        ))
        commands.append(run_logged(
            [python, lab, "build", "--variant=candidate", "--jobs=4"],
            out, "build", cwd=WORKSPACE, timeout=1800,
        ))
        format_state = refresh_node_format_cluster(out)

        pg_config = INSTALL / "bin/pg_config"
        make_args = ["make", "-C", MODULE, "USE_PGXS=1", f"PG_CONFIG={pg_config}"]
        commands.append(run_logged(make_args + ["clean"], out, "module-clean", cwd=WORKSPACE))
        commands.append(run_logged(make_args + ["-j4"], out, "module-build", cwd=WORKSPACE))
        commands.append(run_logged(make_args + ["install"], out, "module-install", cwd=WORKSPACE))

        commands.append(run_logged(
            [python, lab, "start", "--variant=candidate"], out, "start",
            cwd=WORKSPACE, timeout=180,
        ))
        started = True
        format_state["marker"].write_text(format_state["current"] + "\n")

        query = (
            "SET client_min_messages=warning; "
            "DROP EXTENSION IF EXISTS test_graph_core; "
            "CREATE EXTENSION test_graph_core; "
            "SELECT 'NODE|' || test_graph_node_contract(); "
            "SELECT 'PROVIDER|' || test_graph_provider_contract();"
        )
        psql = INSTALL / "bin/psql"
        sql_command = run_logged(
            [psql, "-X", "-qAt", "-v", "ON_ERROR_STOP=1", "-d", "graph_lab", "-c", query],
            out, "contracts", cwd=WORKSPACE, env=db_env("candidate"), timeout=120,
        )
        commands.append(sql_command)
        contract_log = out / "contracts.log"
        contract_text = contract_log.read_text()
        if "WARNING:" in contract_text or "unrecognized node type" in contract_text:
            raise RuntimeError("node contract emitted a query-jumble warning")
        records = {}
        for line in contract_text.splitlines():
            if "|" not in line:
                continue
            label, raw = line.split("|", 1)
            if label in ("NODE", "PROVIDER"):
                records[label.lower()] = json.loads(raw)
        if set(records) != {"node", "provider"}:
            raise RuntimeError("did not capture both SQL contract records")
        assert_contracts(records["node"], records["provider"], golden)

        generated = verify_generated_support(golden)
        graphnodes_text = (SOURCE / "src/include/nodes/graphnodes.h").read_text()
        forbidden_cached_state = [
            token for token in ("(*", "EState", "MemoryContext", "ResourceOwner", "Snapshot", "GgcAdjProviderV2")
            if token in graphnodes_text
        ]
        if forbidden_cached_state:
            raise RuntimeError("cached graph Nodes contain runtime state: " + repr(forbidden_cached_state))

        dependency = verify_no_age_dependency(
            INSTALL / "lib/postgresql/test_graph_core.so", out,
        )
        commands.append(run_logged(
            [python, lab, "stop", "--variant=candidate"], out, "post-stop",
            cwd=WORKSPACE, timeout=120,
        ))
        started = False
        format_record = {
            key: (str(value.relative_to(WORKSPACE)) if isinstance(value, Path) else value)
            for key, value in format_state.items()
        }
        results = {
            "task_id": TASK_ID,
            "finished": now(),
            "golden": str(GOLDEN.relative_to(PACKAGE)),
            "node_contract": records["node"],
            "provider_contract": records["provider"],
            "generated_support": generated,
            "cached_runtime_state_tokens": forbidden_cached_state,
            "age_dependency": dependency,
            "node_format_cluster": format_record,
            "commands": commands,
        }
        write(out / "results.json", results)
        write(out / "summary.json", {
            "status": "PASS",
            "task_id": TASK_ID,
            "finished": now(),
            "results_sha256": sha(out / "results.json"),
            "commands": commands,
        })
        record = {
            "task_id": TASK_ID,
            "status": "PASS",
            "assertions_passed": [
                "copy_equal_out_read",
                "query_jumble_mapping",
                "abi_size_version",
                "no_age_dependency",
                "provider_scoped_handle_provenance",
                "planner_estimate_not_executor_open",
                "catalog_result_abi_stays_v1",
                "bounded_batch_budget_contract",
            ],
            "evidence_files": [
                evidence(GOLDEN),
                evidence(out / "results.json"),
                evidence(out / "contracts.log"),
                evidence(out / "module-ldd.log"),
                evidence(out / "module-undefined.log"),
            ],
        }
        print(json.dumps(record, ensure_ascii=False, sort_keys=True))
    finally:
        if started:
            run_logged(
                [python, lab, "stop", "--variant=candidate"], out, "failure-stop",
                cwd=WORKSPACE, timeout=120,
            )


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError, ValueError, KeyError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
