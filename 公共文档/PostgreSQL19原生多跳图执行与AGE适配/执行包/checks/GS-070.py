#!/usr/bin/env python3
"""Crash graph_adj at WAL publication boundaries and verify recovery."""
from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import time

SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS))

from common import PACKAGE, WORKSPACE, now, read, run_dir, run_logged, sha, write  # noqa: E402
from lab import db_env, sql  # noqa: E402


TASK_ID = "GS-070"
GOLDEN = PACKAGE / "golden/GS-070/recovery_faults.json"
RUNTIME = WORKSPACE / ".runtime/pg19-graph-core/candidate"
INSTALL = WORKSPACE / ".install/pg19-graph-core/candidate"


def refresh_catalog_cluster(out: Path) -> tuple[Path, dict]:
    data = RUNTIME / "data"
    marker = RUNTIME / ".catalog-bki.sha256"
    bki = INSTALL / "share/postgresql/postgres.bki"
    digest = sha(bki)
    previous = marker.read_text().strip() if marker.exists() else None
    archived = None
    if previous != digest and data.exists():
        archive = RUNTIME / "catalog-archives"
        archive.mkdir(parents=True, exist_ok=True)
        archived = archive / ("pre-" + out.name)
        data.rename(archived)
    return marker, {
        "installed_postgres_bki_sha256": digest,
        "previous_postgres_bki_sha256": previous,
        "archived_data": str(archived) if archived else None,
    }


def info() -> dict:
    return json.loads(
        sql(
            "candidate",
            "SELECT pg_catalog.graph_adj_index_info('gs070.edges_adj'::regclass);",
        ).strip()
    )


def edge_ids() -> str:
    return sql(
        "candidate",
        "SELECT coalesce(string_agg(edge_id::text,',' ORDER BY edge_id),'') "
        "FROM gs070.edges;",
    ).strip()


def ref_for(key: int) -> dict:
    return json.loads(
        sql(
            "candidate",
            "SELECT pg_catalog.graph_adj_debug_ref_probe("
            f"'gs070.edges_adj'::regclass,{key},"
            "'gs070.edges_adj'::regclass::oid,1,"
            "'gs070.edges'::regclass::oid,0,0,false);",
        ).strip()
    )


def values_sql(edges: list[list[int]]) -> str:
    return ",".join(
        "(" + ",".join(str(value) for value in edge) + ")" for edge in edges
    )


def wait_ready(timeout: float = 45.0) -> float:
    deadline = time.monotonic() + timeout
    started = time.monotonic()
    binary = INSTALL / "bin/psql"
    while time.monotonic() < deadline:
        try:
            result = subprocess.run(
                [binary, "-X", "-qAt", "-d", "graph_lab", "-c", "SELECT 1;"],
                text=True,
                capture_output=True,
                env=db_env("candidate"),
                timeout=3,
            )
            if result.returncode == 0 and result.stdout.strip() == "1":
                return (time.monotonic() - started) * 1000
        except subprocess.TimeoutExpired:
            pass
        time.sleep(0.1)
    raise RuntimeError("candidate did not finish automatic crash recovery")


def crash_at(out: Path, label: str, point: str, key: int, action: str) -> dict:
    sql("candidate", "CHECKPOINT;")
    server_log = RUNTIME / "postgresql.log"
    log_offset = server_log.stat().st_size if server_log.exists() else 0
    script = (
        "SELECT pg_catalog.graph_adj_debug_fault("
        f"'gs070.edges_adj'::regclass,{key},'{point}');\n"
        + action
        + "\n"
    )
    started = now()
    process = subprocess.run(
        [INSTALL / "bin/psql", "-X", "-qAt", "-v", "ON_ERROR_STOP=1", "-d", "graph_lab"],
        input=script,
        text=True,
        capture_output=True,
        env=db_env("candidate"),
        timeout=60,
    )
    if process.returncode == 0:
        raise RuntimeError(f"fault {point} did not crash its backend")
    recovery_ms = wait_ready()
    server_tail = ""
    if server_log.exists():
        with server_log.open("r", errors="replace") as stream:
            stream.seek(log_offset)
            server_tail = stream.read()
    transcript = (
        f"SQL:\n{script}\nRETURN CODE: {process.returncode}\n"
        f"STDOUT:\n{process.stdout}\nSTDERR:\n{process.stderr}\n"
        f"SERVER LOG SINCE ARM:\n{server_tail}"
    )
    log = out / f"fault-{label}.log"
    log.write_text(transcript)
    if f"graph_adj injected crash after {point}" not in server_tail:
        raise RuntimeError(f"server log did not prove fault point {point}: {server_tail[-3000:]}")
    return {
        "label": label,
        "point": point,
        "key": key,
        "started": started,
        "finished": now(),
        "psql_returncode": process.returncode,
        "recovery_ms": recovery_ms,
        "log": str(log.relative_to(PACKAGE)),
        "sha256": sha(log),
    }


def assert_visible(expected_ids: list[int], label: str) -> dict:
    expected = ",".join(str(value) for value in sorted(expected_ids))
    actual = edge_ids()
    if actual != expected:
        raise RuntimeError(f"{label}: visible heap truth differs: {actual} != {expected}")
    state = info()
    if state["out_visible_entries"] != len(expected_ids) or state["in_visible_entries"] != len(expected_ids):
        raise RuntimeError(f"{label}: visible adjacency copies differ: {state}")
    return state


def evidence(path: Path) -> dict:
    return {"path": str(path.relative_to(PACKAGE)), "sha256": sha(path)}


def main() -> None:
    out = run_dir("check-GS-070")
    golden = read(GOLDEN)
    lab = PACKAGE / "scripts/lab.py"
    python = sys.executable
    stop = [python, lab, "stop", "--variant=candidate"]
    commands = [run_logged(stop, out, "pre-stop", cwd=WORKSPACE, timeout=120)]
    commands.append(
        run_logged(
            [python, lab, "build", "--variant=candidate", "--jobs=4"],
            out,
            "build",
            cwd=WORKSPACE,
            timeout=1800,
        )
    )
    catalog_marker, catalog_refresh = refresh_catalog_cluster(out)
    commands.append(
        run_logged(
            [python, lab, "start", "--variant=candidate"],
            out,
            "start",
            cwd=WORKSPACE,
            timeout=120,
        )
    )
    catalog_marker.write_text(catalog_refresh["installed_postgres_bki_sha256"] + "\n")

    faults: list[dict] = []
    expected_ids = [golden["seed_edge"][0]]
    snapshots: dict[str, dict] = {}
    try:
        sql(
            "candidate",
            "DROP SCHEMA IF EXISTS gs070 CASCADE; CREATE SCHEMA gs070; "
            "CREATE TABLE gs070.edges("
            "edge_id bigint PRIMARY KEY,src bigint NOT NULL,dst bigint NOT NULL); "
            "CREATE INDEX edges_adj ON gs070.edges USING graph_adj(src,dst); "
            f"INSERT INTO gs070.edges VALUES {values_sql([golden['seed_edge']])};",
        )
        snapshots["seed"] = assert_visible(expected_ids, "seed")

        # The Slot WAL record is durable, but neither directory maps it yet.
        before_slot = info()
        edge = golden["slot_retry_edge"]
        faults.append(
            crash_at(
                out,
                "slot-initialized",
                "slot_initialized",
                edge[1],
                f"INSERT INTO gs070.edges VALUES ({edge[0]},{edge[1]},{edge[2]});",
            )
        )
        after_slot_crash = assert_visible(expected_ids, "slot crash")
        if after_slot_crash["slot_count"] != before_slot["slot_count"] + 1:
            raise RuntimeError("initialized Slot was not replayed")
        if (
            after_slot_crash["out_directory_items"] != before_slot["out_directory_items"]
            or after_slot_crash["in_directory_items"] != before_slot["in_directory_items"]
        ):
            raise RuntimeError("slot-only crash unexpectedly published a directory mapping")
        sql("candidate", f"INSERT INTO gs070.edges VALUES ({edge[0]},{edge[1]},{edge[2]});")
        expected_ids.append(edge[0])
        slot_ref = ref_for(edge[1])
        if slot_ref["slot_generation"] != 1:
            raise RuntimeError(f"slot retry changed generation: {slot_ref}")
        snapshots["slot_retry"] = assert_visible(expected_ids, "slot retry")

        # OUT mapping is durable while IN is deliberately absent; retry repairs it.
        before_directory = info()
        edge = golden["directory_retry_edge"]
        faults.append(
            crash_at(
                out,
                "one-directory",
                "out_directory_published",
                edge[1],
                f"INSERT INTO gs070.edges VALUES ({edge[0]},{edge[1]},{edge[2]});",
            )
        )
        after_directory_crash = assert_visible(expected_ids, "one-directory crash")
        if after_directory_crash["out_directory_items"] != before_directory["out_directory_items"] + 1:
            raise RuntimeError("OUT directory publication was not replayed")
        if after_directory_crash["in_directory_items"] != before_directory["in_directory_items"]:
            raise RuntimeError("IN directory was published before its fault boundary")
        sql("candidate", f"INSERT INTO gs070.edges VALUES ({edge[0]},{edge[1]},{edge[2]});")
        expected_ids.append(edge[0])
        snapshots["directory_retry"] = assert_visible(expected_ids, "directory retry")
        if not snapshots["directory_retry"]["canonical_mapping_valid"]:
            raise RuntimeError("directory retry did not restore canonical mappings")

        # A new owner's OUT segment shares an existing packed page.  The
        # uncommitted physical entry must not hide or damage other owners.
        edge = golden["shared_segment_retry_edge"]
        faults.append(
            crash_at(
                out,
                "shared-segment",
                "shared_segment_published",
                edge[1],
                f"INSERT INTO gs070.edges VALUES ({edge[0]},{edge[1]},{edge[2]});",
            )
        )
        shared_crash = assert_visible(expected_ids, "shared segment crash")
        if shared_crash["shared_data_pages"] < 1:
            raise RuntimeError("fault did not occur on the packed shared-page baseline")
        sql("candidate", "VACUUM gs070.edges;")
        shared_vacuum = assert_visible(expected_ids, "shared segment vacuum")
        sql("candidate", f"INSERT INTO gs070.edges VALUES ({edge[0]},{edge[1]},{edge[2]});")
        expected_ids.append(edge[0])
        snapshots["shared_segment_retry"] = assert_visible(expected_ids, "shared segment retry")

        # OUT append is durable and IN append has not started.  Heap MVCC must
        # make the aborted half edge invisible until VACUUM removes it.
        edge = golden["half_direction_retry_edge"]
        faults.append(
            crash_at(
                out,
                "half-direction",
                "out_entry_published",
                edge[1],
                f"INSERT INTO gs070.edges VALUES ({edge[0]},{edge[1]},{edge[2]});",
            )
        )
        half_crash = assert_visible(expected_ids, "half-direction crash")
        if half_crash["out_physical_entries"] <= half_crash["in_physical_entries"]:
            raise RuntimeError("half-direction WAL record was not replayed")
        sql("candidate", "VACUUM gs070.edges;")
        half_vacuum = assert_visible(expected_ids, "half-direction vacuum")
        if half_vacuum["out_physical_entries"] != half_vacuum["in_physical_entries"]:
            raise RuntimeError("VACUUM did not converge half-direction residue")
        sql("candidate", f"INSERT INTO gs070.edges VALUES ({edge[0]},{edge[1]},{edge[2]});")
        expected_ids.append(edge[0])
        snapshots["half_direction_retry"] = assert_visible(expected_ids, "half-direction retry")

        # Seal/new-active is a recoverable authoritative state.
        edges = golden["seal_edges"]
        sql("candidate", f"INSERT INTO gs070.edges VALUES {values_sql(edges)};")
        expected_ids.extend(edge[0] for edge in edges)
        faults.append(
            crash_at(
                out,
                "sealed",
                "sealed_published",
                100,
                "SELECT pg_catalog.graph_adj_debug_compact("
                "'gs070.edges_adj'::regclass,100,true,0);",
            )
        )
        sealed_crash = assert_visible(expected_ids, "sealed crash")
        if sealed_crash["sealed_entries"] < len(edges):
            raise RuntimeError("recovery lost the sealed cutoff")
        sealed_resume = json.loads(
            sql(
                "candidate",
                "SELECT pg_catalog.graph_adj_debug_compact("
                "'gs070.edges_adj'::regclass,100,true,0);",
            ).strip()
        )
        if not sealed_resume["resumed_sealed"] or sealed_resume["base_output_items"] != len(edges):
            raise RuntimeError(f"sealed generation was not resumable: {sealed_resume}")

        # A complete but unpublished Base is an orphan, not a visible second copy.
        edges = golden["base_build_edges"]
        sql("candidate", f"INSERT INTO gs070.edges VALUES {values_sql(edges)};")
        expected_ids.extend(edge[0] for edge in edges)
        faults.append(
            crash_at(
                out,
                "base-built",
                "base_built",
                200,
                "SELECT pg_catalog.graph_adj_debug_compact("
                "'gs070.edges_adj'::regclass,200,true,0);",
            )
        )
        base_built_crash = assert_visible(expected_ids, "base build crash")
        if base_built_crash["unreferenced_segments"] < 1:
            raise RuntimeError("unpublished Base was not accounted as orphan debt")
        base_resume = json.loads(
            sql(
                "candidate",
                "SELECT pg_catalog.graph_adj_debug_compact("
                "'gs070.edges_adj'::regclass,200,true,0);",
            ).strip()
        )
        if not base_resume["resumed_sealed"] or base_resume["base_output_items"] != len(edges):
            raise RuntimeError(f"Base build retry did not resume sealed input: {base_resume}")

        # Descriptor publication itself is a single WAL-protected linearization point.
        edges = golden["base_publish_edges"]
        sql("candidate", f"INSERT INTO gs070.edges VALUES {values_sql(edges)};")
        expected_ids.extend(edge[0] for edge in edges)
        faults.append(
            crash_at(
                out,
                "base-published",
                "base_published",
                300,
                "SELECT pg_catalog.graph_adj_debug_compact("
                "'gs070.edges_adj'::regclass,300,true,0);",
            )
        )
        base_published_crash = assert_visible(expected_ids, "Base publish crash")
        if base_published_crash["base_entries"] < len(edges):
            raise RuntimeError("published Base was not authoritative after recovery")

        # Build a 180-entry Base, then publish an equivalent replacement.  At
        # this size each Base occupies its own physical page, so the old one
        # is a true whole-page orphan rather than a hole on a shared page.
        # This is the required precondition for testing the whole-page-only
        # free list introduced by GS-050.
        stress_ids = list(range(1000, 1180))
        sql(
            "candidate",
            "INSERT INTO gs070.edges "
            "SELECT 1000+g,400,20 FROM generate_series(0,179) AS g;",
        )
        expected_ids.extend(stress_ids)
        first_large_base = json.loads(
            sql(
                "candidate",
                "SELECT pg_catalog.graph_adj_debug_compact("
                "'gs070.edges_adj'::regclass,400,true,0);",
            ).strip()
        )
        second_large_base = json.loads(
            sql(
                "candidate",
                "SELECT pg_catalog.graph_adj_debug_compact("
                "'gs070.edges_adj'::regclass,400,true,0);",
            ).strip()
        )
        before_free_fault = assert_visible(expected_ids, "large Base replacement")
        if first_large_base["base_output_items"] != 180 or second_large_base["base_output_items"] != 180:
            raise RuntimeError("large Base replacement did not preserve all parallel edges")
        if before_free_fault["maintenance_debt_bytes"] <= 0:
            raise RuntimeError("large Base replacement did not create maintenance debt")

        # Crash after free-list push proves Meta.free_head and page identity
        # replay together.  A later VACUUM must finish idempotently.
        faults.append(
            crash_at(
                out,
                "free-page-published",
                "free_page_published",
                0,
                "VACUUM gs070.edges;",
            )
        )
        free_crash = assert_visible(expected_ids, "free-list crash")
        if free_crash["free_pages"] < 1:
            raise RuntimeError("free-list WAL record was not replayed")
        sql("candidate", "VACUUM gs070.edges;")
        after_reclaim = assert_visible(expected_ids, "reclaim retry")
        sql("candidate", "DELETE FROM gs070.edges WHERE edge_id >= 1000;")
        sql("candidate", "VACUUM gs070.edges;")
        expected_ids = [value for value in expected_ids if value < 1000]
        after_stress_cleanup = assert_visible(expected_ids, "large Base cleanup")

        # Deliberately WAL-log a bad persistent opposite ref.  Observability
        # must expose it and scans must ERROR instead of key fallback.
        changed = sql(
            "candidate",
            "SELECT pg_catalog.graph_adj_debug_fault("
            "'gs070.edges_adj'::regclass,10,'corrupt_out_ref');",
        ).strip()
        if changed != "t":
            raise RuntimeError("persistent reference corruption was not injected")
        corrupt_info = info()
        if corrupt_info["opposite_refs_valid"]:
            raise RuntimeError("index observability hid persistent reference corruption")
        corruption_error = ""
        try:
            sql(
                "candidate",
                "SET enable_seqscan=off; SET enable_bitmapscan=off; "
                "SELECT edge_id FROM gs070.edges WHERE src=10;",
            )
        except RuntimeError as error:
            corruption_error = str(error)
        if "conflicting persistent VertexRef" not in corruption_error:
            raise RuntimeError(f"scan hid persistent corruption: {corruption_error}")
        sql("candidate", "REINDEX INDEX gs070.edges_adj;")
        final_state = assert_visible(expected_ids, "REINDEX repair")
        if not final_state["opposite_refs_valid"] or not final_state["canonical_mapping_valid"]:
            raise RuntimeError("REINDEX did not restore a valid graph_adj index")

        expected_final = ",".join(str(value) for value in golden["expected_final_ids"])
        if edge_ids() != expected_final:
            raise RuntimeError("final committed edge multiset differs from golden")

        results = {
            "task_id": TASK_ID,
            "finished": now(),
            "golden": str(GOLDEN.relative_to(PACKAGE)),
            "catalog_refresh": catalog_refresh,
            "faults": faults,
            "slot_creation": {
                "before": before_slot,
                "after_crash": after_slot_crash,
                "recovered_ref": slot_ref,
            },
            "one_directory": {
                "before": before_directory,
                "after_crash": after_directory_crash,
            },
            "shared_segment": {"after_crash": shared_crash, "after_vacuum": shared_vacuum},
            "half_direction": {"after_crash": half_crash, "after_vacuum": half_vacuum},
            "sealed": {"after_crash": sealed_crash, "resume": sealed_resume},
            "base_build": {"after_crash": base_built_crash, "resume": base_resume},
            "base_publish": base_published_crash,
            "reclaim": {
                "first_large_base": first_large_base,
                "second_large_base": second_large_base,
                "before_fault": before_free_fault,
                "after_crash": free_crash,
                "after_retry": after_reclaim,
                "after_stress_cleanup": after_stress_cleanup,
            },
            "persistent_corruption": {
                "index_info": corrupt_info,
                "scan_error": corruption_error,
            },
            "snapshots": snapshots,
            "final": {"edge_ids": edge_ids(), "index": final_state},
            "commands": commands,
        }
        write(out / "results.json", results)
    finally:
        run_logged(stop, out, "post-stop", cwd=WORKSPACE, timeout=120)

    record = {
        "task_id": TASK_ID,
        "status": "PASS",
        "assertions_passed": [
            "half_direction_crash",
            "seal_publish_crash",
            "recovery_complete",
            "no_ghosts",
            "slot_creation_ref_generation_faults",
            "shared_segment_growth_recovery",
            "persistent_ref_corruption_not_hidden",
        ],
        "evidence_files": [
            evidence(out / "results.json"),
            evidence(out / "build.log"),
            *[evidence(PACKAGE / fault["log"]) for fault in faults],
        ],
    }
    print(json.dumps(record, ensure_ascii=False, sort_keys=True))


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError, ValueError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
