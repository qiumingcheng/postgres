#!/usr/bin/env python3
"""Exercise packed multi-owner segments, growth, WAL, and crash recovery."""
from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import time

SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS))

from common import PACKAGE, WORKSPACE, now, run_dir, run_logged, sha, write  # noqa: E402
from lab import db_env, sql  # noqa: E402


TASK_ID = "GS-045"


def refresh_catalog_cluster(out: Path) -> tuple[Path, dict]:
    runtime = WORKSPACE / ".runtime/pg19-graph-core/candidate"
    data = runtime / "data"
    marker = runtime / ".catalog-bki.sha256"
    bki = WORKSPACE / ".install/pg19-graph-core/candidate/share/postgresql/postgres.bki"
    digest = sha(bki)
    previous = marker.read_text().strip() if marker.exists() else None
    archived = None
    if previous != digest and data.exists():
        archive = runtime / "catalog-archives"
        archive.mkdir(parents=True, exist_ok=True)
        archived = archive / ("pre-" + out.name)
        data.rename(archived)
    return marker, {
        "installed_postgres_bki_sha256": digest,
        "previous_postgres_bki_sha256": previous,
        "archived_data": str(archived) if archived else None,
    }


def info(index: str = "gs045.edges_adj") -> dict:
    return json.loads(
        sql(
            "candidate",
            f"SELECT pg_catalog.graph_adj_index_info('{index}'::regclass);",
        ).strip()
    )


def ref_for(key: int) -> dict:
    return json.loads(
        sql(
            "candidate",
            "SELECT pg_catalog.graph_adj_debug_ref_probe("
            f"'gs045.edges_adj'::regclass,{key},"
            "'gs045.edges_adj'::regclass::oid,1,"
            f"'gs045.edges'::regclass::oid,0,0,false);",
        ).strip()
    )


def ids(predicate: str) -> str:
    return sql(
        "candidate",
        "SET enable_seqscan=off; SET enable_bitmapscan=off; "
        "SELECT coalesce(string_agg(edge_id::text,',' ORDER BY edge_id),'') "
        f"FROM gs045.edges WHERE {predicate};",
    ).strip()


def concurrent_inserts(rows: list[tuple[int, int, int]]) -> dict:
    binary = WORKSPACE / ".install/pg19-graph-core/candidate/bin/psql"
    processes = []
    started = time.monotonic_ns()
    for edge_id, src, dst in rows:
        statement = f"INSERT INTO gs045.edges VALUES({edge_id},{src},{dst});"
        processes.append(
            subprocess.Popen(
                [binary, "-X", "-qAt", "-v", "ON_ERROR_STOP=1", "-d", "graph_lab", "-c", statement],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                env=db_env("candidate"),
            )
        )
    results = []
    for process in processes:
        stdout, stderr = process.communicate(timeout=20)
        results.append(
            {"returncode": process.returncode, "stdout": stdout, "stderr": stderr}
        )
    elapsed_ms = (time.monotonic_ns() - started) / 1_000_000
    if any(result["returncode"] != 0 for result in results):
        raise RuntimeError(f"concurrent shared-page insert failed: {results}")
    return {"elapsed_ms": elapsed_ms, "sessions": results}


def evidence(path: Path) -> dict:
    return {"path": str(path.relative_to(PACKAGE)), "sha256": sha(path)}


def main() -> None:
    out = run_dir("check-GS-045")
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

    try:
        sql(
            "candidate",
            "DROP SCHEMA IF EXISTS gs045 CASCADE; CREATE SCHEMA gs045; "
            "CREATE TABLE gs045.empty_edges("
            "edge_id bigint,src bigint NOT NULL,dst bigint NOT NULL); "
            "CREATE INDEX empty_adj ON gs045.empty_edges USING graph_adj(src,dst); "
            "CREATE TABLE gs045.edges("
            "edge_id bigint PRIMARY KEY,src bigint NOT NULL,dst bigint NOT NULL); "
            "CREATE INDEX edges_adj ON gs045.edges USING graph_adj(src,dst);",
        )
        empty = info("gs045.empty_adj")
        if empty["data_pages"] != 0 or empty["referenced_segments"] != 0:
            raise RuntimeError(f"empty directions allocated data storage: {empty}")
        sql("candidate", "INSERT INTO gs045.empty_edges VALUES(1,1,2);")
        one_edge = info("gs045.empty_adj")
        if one_edge["data_pages"] != 2 or one_edge["referenced_segments"] != 2:
            raise RuntimeError(f"one edge did not allocate only OUT/IN segments: {one_edge}")
        if one_edge["empty_direction_heads"] != 2:
            raise RuntimeError("one edge eagerly allocated empty endpoint directions")

        sql(
            "candidate",
            "INSERT INTO gs045.edges VALUES"
            "(1,10,100),(2,20,200),(3,30,300);",
        )
        packed = info()
        expected_packed = {
            "slot_count": 6,
            "referenced_segments": 6,
            "data_pages": 2,
            "shared_data_pages": 2,
            "empty_direction_heads": 6,
            "small_segment_capacity": 8,
            "packed_small_segments": True,
        }
        for key, expected in expected_packed.items():
            if packed[key] != expected:
                raise RuntimeError(f"packed layout {key}: {packed[key]} != {expected}")
        if packed["data_pages"] >= packed["referenced_segments"]:
            raise RuntimeError("small adjacency still consumes one page per segment")
        if ids("src=10") != "1" or ids("src=20") != "2" or ids("dst=200") != "2":
            raise RuntimeError("multi-owner page scan crossed owner segment boundaries")

        ref10_before = ref_for(10)
        ref20_before = ref_for(20)
        compact10 = json.loads(
            sql(
                "candidate",
                "SELECT pg_catalog.graph_adj_debug_compact("
                "'gs045.edges_adj'::regclass,10,true,0);",
            ).strip()
        )
        after_one_owner = info()
        if ids("src=20") != "2" or ref_for(20)["slot_id"] != ref20_before["slot_id"]:
            raise RuntimeError("retiring one shared-page segment damaged another owner")
        if after_one_owner["unreferenced_segments"] < 1:
            raise RuntimeError("replaced shared segment was not accounted as unreferenced")
        if after_one_owner["shared_page_reuse_enabled"]:
            raise RuntimeError("prototype unsafely advertises shared-page reuse")

        # The active segment holds eight entries.  Nine further writes force
        # synchronous seal/build/publish and continue in a new active segment.
        sql(
            "candidate",
            "INSERT INTO gs045.edges "
            "SELECT 10+g,10,100+g FROM generate_series(1,9) g;",
        )
        grown = info()
        ref10_after = ref_for(10)
        if ids("src=10") != "1,11,12,13,14,15,16,17,18,19":
            raise RuntimeError("segment growth lost or duplicated edges")
        if (ref10_before["slot_id"], ref10_before["slot_generation"]) != (
            ref10_after["slot_id"],
            ref10_after["slot_generation"],
        ):
            raise RuntimeError("segment growth moved the live Endpoint Slot")
        if grown["base_entries"] < 9 or grown["active_entries"] < 1:
            raise RuntimeError(f"growth did not publish Base plus new active: {grown}")

        wal_before = sql("candidate", "SELECT pg_current_wal_insert_lsn();").strip()
        contention = concurrent_inserts(
            [(1001, 20, 100), (1002, 30, 200), (1003, 20, 300), (1004, 30, 100)]
        )
        wal_after = sql("candidate", "SELECT pg_current_wal_insert_lsn();").strip()
        wal_bytes = int(
            sql(
                "candidate",
                f"SELECT pg_wal_lsn_diff('{wal_after}','{wal_before}')::bigint;",
            ).strip()
        )
        measured = info()
        if wal_bytes <= 0 or contention["elapsed_ms"] <= 0:
            raise RuntimeError("WAL or shared-page contention measurement is missing")
        if not measured["all_mutable_pages_have_lsn"]:
            raise RuntimeError("a packed page lacks a WAL LSN")
        if measured["segment_reserved_bytes"] <= measured["entry_payload_bytes"]:
            raise RuntimeError("layout accounting omitted segment headers/reservation")
        if measured["reserved_occupancy_basis_points"] <= 0:
            raise RuntimeError("packed layout occupancy was not measured")

        before_recovery_rows = {
            "src10": ids("src=10"),
            "src20": ids("src=20"),
            "src30": ids("src=30"),
        }
        sql("candidate", "CHECKPOINT;")
        pg_ctl = WORKSPACE / ".install/pg19-graph-core/candidate/bin/pg_ctl"
        data = WORKSPACE / ".runtime/pg19-graph-core/candidate/data"
        commands.append(
            run_logged(
                [pg_ctl, "-D", data, "-m", "immediate", "-w", "stop"],
                out,
                "immediate-stop",
                cwd=WORKSPACE,
                env=db_env("candidate"),
                timeout=120,
            )
        )
        commands.append(
            run_logged(
                [python, lab, "start", "--variant=candidate"],
                out,
                "recovery-start",
                cwd=WORKSPACE,
                timeout=120,
            )
        )
        after_recovery_rows = {
            "src10": ids("src=10"),
            "src20": ids("src=20"),
            "src30": ids("src=30"),
        }
        recovered = info()
        if after_recovery_rows != before_recovery_rows:
            raise RuntimeError(
                f"crash recovery changed packed owner results: {before_recovery_rows} -> {after_recovery_rows}"
            )
        if not recovered["canonical_mapping_valid"] or not recovered["opposite_refs_valid"]:
            raise RuntimeError("crash recovery broke Slot/ref ownership")

        results = {
            "task_id": TASK_ID,
            "finished": now(),
            "catalog_refresh": catalog_refresh,
            "empty_index": empty,
            "one_edge_lazy_allocation": one_edge,
            "packed_multi_owner": packed,
            "compacted_owner_10": compact10,
            "after_one_owner_replaced": after_one_owner,
            "grown_layout": grown,
            "stable_ref_before": ref10_before,
            "stable_ref_after": ref10_after,
            "contention": contention,
            "wal": {"before": wal_before, "after": wal_after, "bytes": wal_bytes},
            "occupancy": {
                key: measured[key]
                for key in (
                    "data_pages",
                    "shared_data_pages",
                    "allocated_segments",
                    "referenced_segments",
                    "unreferenced_segments",
                    "data_usable_bytes",
                    "segment_reserved_bytes",
                    "entry_payload_bytes",
                    "payload_occupancy_basis_points",
                    "reserved_occupancy_basis_points",
                )
            },
            "before_recovery_rows": before_recovery_rows,
            "after_recovery_rows": after_recovery_rows,
            "recovered_layout": recovered,
            "commands": commands,
        }
        write(out / "results.json", results)
    finally:
        run_logged(stop, out, "post-stop", cwd=WORKSPACE, timeout=120)

    record = {
        "task_id": TASK_ID,
        "status": "PASS",
        "assertions_passed": [
            "packed_slots_and_small_segments",
            "empty_heads_no_data_page",
            "multi_owner_segment_isolation",
            "growth_publish_stable_slot",
            "shared_page_retirement_safe",
            "occupancy_bytes_wal_contention_measured",
        ],
        "evidence_files": [
            evidence(out / "results.json"),
            evidence(out / "build.log"),
            evidence(out / "immediate-stop.log"),
            evidence(out / "recovery-start.log"),
        ],
    }
    print(json.dumps(record, ensure_ascii=False, sort_keys=True))


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError, ValueError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
