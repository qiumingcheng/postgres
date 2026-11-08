#!/usr/bin/env python3
"""Exercise callback-authoritative VACUUM, whole-page reclaim, and reuse."""
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


TASK_ID = "GS-050"
SCHEMA = "gs050"


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


def info() -> dict:
    return json.loads(
        sql(
            "candidate",
            "SELECT pg_catalog.graph_adj_index_info('gs050.edges_adj'::regclass);",
        ).strip()
    )


def ref_for(key: int) -> dict:
    return json.loads(
        sql(
            "candidate",
            "SELECT pg_catalog.graph_adj_debug_ref_probe("
            f"'gs050.edges_adj'::regclass,{key},"
            "'gs050.edges_adj'::regclass::oid,1,"
            "'gs050.edges'::regclass::oid,0,0,false);",
        ).strip()
    )


def edge_ids(predicate: str = "true") -> str:
    return sql(
        "candidate",
        "SET enable_seqscan=off; SET enable_bitmapscan=off; "
        "SELECT coalesce(string_agg(edge_id::text,',' ORDER BY edge_id),'') "
        f"FROM gs050.edges WHERE {predicate};",
    ).strip()


def relation_blocks() -> int:
    return int(
        sql(
            "candidate",
            "SELECT pg_relation_size('gs050.edges_adj') / current_setting('block_size')::int;",
        ).strip()
    )


def async_psql(statement: str) -> subprocess.Popen[str]:
    binary = WORKSPACE / ".install/pg19-graph-core/candidate/bin/psql"
    return subprocess.Popen(
        [binary, "-X", "-qAt", "-v", "ON_ERROR_STOP=1", "-d", "graph_lab", "-c", statement],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=db_env("candidate"),
    )


def finish_process(process: subprocess.Popen[str], label: str, timeout: float = 20.0) -> dict:
    stdout, stderr = process.communicate(timeout=timeout)
    if process.returncode != 0:
        raise RuntimeError(f"{label} failed: stdout={stdout!r}, stderr={stderr!r}")
    return {"returncode": process.returncode, "stdout": stdout, "stderr": stderr}


def wait_for_sealed(timeout: float = 10.0) -> dict:
    deadline = time.monotonic() + timeout
    last = {}
    while time.monotonic() < deadline:
        last = info()
        if last["sealed_entries"] >= 1:
            return last
        time.sleep(0.05)
    raise RuntimeError(f"timed out waiting for sealed maintenance input: {last}")


def vacuum_logged(out: Path, label: str, commands: list[dict]) -> None:
    binary = WORKSPACE / ".install/pg19-graph-core/candidate/bin/psql"
    commands.append(
        run_logged(
            [binary, "-X", "-v", "ON_ERROR_STOP=1", "-d", "graph_lab", "-c", "VACUUM (VERBOSE) gs050.edges"],
            out,
            label,
            cwd=WORKSPACE,
            env=db_env("candidate"),
            timeout=120,
        )
    )


def evidence(path: Path) -> dict:
    return {"path": str(path.relative_to(PACKAGE)), "sha256": sha(path)}


def main() -> None:
    out = run_dir("check-GS-050")
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
            "DROP SCHEMA IF EXISTS gs050 CASCADE; CREATE SCHEMA gs050; "
            "CREATE TABLE gs050.edges("
            "edge_id bigint PRIMARY KEY,src bigint NOT NULL,dst bigint NOT NULL); "
            "CREATE INDEX edges_adj ON gs050.edges USING graph_adj(src,dst); "
            "INSERT INTO gs050.edges VALUES"
            "(1,10,100),(2,10,101),(3,20,200),(4,30,300),"
            "(5,40,400),(6,50,500);",
        )
        initial = info()
        ref10 = ref_for(10)
        ref20 = ref_for(20)
        if initial["out_physical_entries"] != 6 or initial["in_physical_entries"] != 6:
            raise RuntimeError(f"initial twin adjacency copies are unbalanced: {initial}")
        if initial["free_pages"] != 0 or initial["maintenance_debt_bytes"] != 0:
            raise RuntimeError(f"fresh index unexpectedly starts with maintenance debt: {initial}")
        if initial["shared_page_reuse_enabled"] or not initial["whole_page_reuse_enabled"]:
            raise RuntimeError("reclaim granularity is not explicitly whole-page only")
        if initial["slot_reuse_enabled"]:
            raise RuntimeError("GS-050 must not advertise Slot reuse")

        compacted_one = json.loads(
            sql(
                "candidate",
                "SELECT pg_catalog.graph_adj_debug_compact("
                "'gs050.edges_adj'::regclass,10,true,0);",
            ).strip()
        )
        shared_orphan = info()
        if shared_orphan["unreferenced_segments"] < 1:
            raise RuntimeError("replaced segment was not accounted as retired debt")
        if shared_orphan["maintenance_debt_bytes"] != shared_orphan["retired_segment_bytes"]:
            raise RuntimeError("maintenance debt and retired bytes disagree")
        if shared_orphan["maintenance_debt_bytes"] <= 0:
            raise RuntimeError("retired shared-page bytes were not measured")
        if edge_ids() != "1,2,3,4,5,6":
            raise RuntimeError("single-owner compaction damaged another shared-page owner")

        # No tuple is dead yet.  Cleanup may rediscover whole-page orphans, but
        # it must not free the shared Delta page while other owners reference it.
        vacuum_logged(out, "shared-owner-vacuum", commands)
        shared_owner_guard = info()
        if shared_owner_guard["free_pages"] != shared_orphan["free_pages"]:
            raise RuntimeError("VACUUM reclaimed a page that still had a live shared owner")
        if shared_owner_guard["maintenance_debt_bytes"] <= 0:
            raise RuntimeError("whole-page policy silently reused an individual segment hole")

        sql("candidate", "DELETE FROM gs050.edges WHERE edge_id <> 2;")
        vacuum_logged(out, "dead-tid-vacuum", commands)
        after_delete_vacuum = info()
        if edge_ids() != "2" or edge_ids("src=10") != "2":
            raise RuntimeError("VACUUM resurrected or lost a heap edge")
        if edge_ids("src IN (20,30,40,50)") != "":
            raise RuntimeError("dead root TIDs remained reachable from OUT adjacency")
        if edge_ids("dst IN (100,200,300,400,500)") != "":
            raise RuntimeError("dead root TIDs remained reachable from IN adjacency")
        if after_delete_vacuum["out_physical_entries"] != 1 or after_delete_vacuum["in_physical_entries"] != 1:
            raise RuntimeError(f"callback filtering did not remove both physical directions: {after_delete_vacuum}")
        if after_delete_vacuum["free_pages"] <= 0:
            raise RuntimeError("published orphan pages were not moved to the free list")
        expected_debt = (
            after_delete_vacuum["allocated_segment_reserved_bytes"]
            - after_delete_vacuum["segment_reserved_bytes"]
        )
        if expected_debt != after_delete_vacuum["maintenance_debt_bytes"]:
            raise RuntimeError("post-VACUUM byte debt is not exact")

        # Endpoint Slots and their generations are stable even when the last
        # incident edge disappears.  New identities must receive new Slots.
        ref20_after_delete = ref_for(20)
        if (ref20_after_delete["slot_id"], ref20_after_delete["slot_generation"]) != (
            ref20["slot_id"],
            ref20["slot_generation"],
        ):
            raise RuntimeError("VACUUM recycled a Slot before a proven ABA horizon")

        free_before_reuse = after_delete_vacuum["free_pages"]
        blocks_before_reuse = relation_blocks()
        sql("candidate", "INSERT INTO gs050.edges VALUES(1000,60,600);")
        after_reuse = info()
        ref60 = ref_for(60)
        if relation_blocks() != blocks_before_reuse:
            raise RuntimeError("new Delta allocation extended the relation instead of using free pages")
        if after_reuse["free_pages"] > free_before_reuse - 2:
            raise RuntimeError("OUT/IN allocation did not consume two whole free pages")
        if after_reuse["reused_pages"] < 2:
            raise RuntimeError("allocation generation did not record physical page reuse")
        if ref60["slot_id"] == ref20["slot_id"]:
            raise RuntimeError("new endpoint aliased a retired endpoint Slot")
        if ref_for(20)["slot_id"] != ref20["slot_id"]:
            raise RuntimeError("old VertexRef changed after unrelated page reuse")

        # A paused compactor owns the endpoint maintenance token and an
        # AccessShare relation lock.  VACUUM's conservative reuse horizon must
        # wait for it before callback compaction and page recycling.
        compactor = async_psql(
            "SELECT pg_catalog.graph_adj_debug_compact("
            "'gs050.edges_adj'::regclass,60,true,1200);"
        )
        sealed_during_compaction = wait_for_sealed()
        sql("candidate", "DELETE FROM gs050.edges WHERE edge_id=1000;")
        vacuum_started = time.monotonic()
        concurrent_vacuum = async_psql("VACUUM (VERBOSE) gs050.edges;")
        time.sleep(0.20)
        if concurrent_vacuum.poll() is not None:
            raise RuntimeError("VACUUM did not wait for the active compactor/reuse horizon")
        compactor_result = finish_process(compactor, "paused compactor")
        vacuum_result = finish_process(concurrent_vacuum, "concurrent VACUUM")
        vacuum_elapsed_ms = (time.monotonic() - vacuum_started) * 1000
        if vacuum_elapsed_ms < 700:
            raise RuntimeError(f"maintenance serialization was not observed: {vacuum_elapsed_ms:.1f} ms")
        after_serialized_vacuum = info()
        if edge_ids("src=60") != "" or after_serialized_vacuum["out_visible_entries"] != 1:
            raise RuntimeError("serialized VACUUM made the deleted concurrent-test TID visible")
        if after_serialized_vacuum["out_physical_entries"] < 1:
            raise RuntimeError("serialized VACUUM corrupted physical entry accounting")

        # This first VACUUM chose its global cleanup cutoff while the older
        # compactor transaction still existed.  It must not guess that the TID
        # is globally removable.  Once that transaction has ended, a new
        # VACUUM receives an advanced callback horizon and may remove both
        # directions physically.
        vacuum_logged(out, "post-horizon-vacuum", commands)
        after_horizon_vacuum = info()
        if after_horizon_vacuum["out_physical_entries"] != 1 or after_horizon_vacuum["in_physical_entries"] != 1:
            raise RuntimeError("advanced VACUUM horizon did not remove both physical TID copies")
        if edge_ids() != "2":
            raise RuntimeError("advanced VACUUM horizon changed the surviving logical edge")

        before_recovery_rows = edge_ids()
        before_recovery = after_horizon_vacuum
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
        recovered = info()
        after_recovery_rows = edge_ids()
        if after_recovery_rows != before_recovery_rows or after_recovery_rows != "2":
            raise RuntimeError("crash recovery resurrected a callback-removed root TID")
        if recovered["free_pages"] != before_recovery["free_pages"]:
            raise RuntimeError("WAL recovery changed the free-list population")

        recovery_blocks = relation_blocks()
        recovery_free = recovered["free_pages"]
        sql("candidate", "INSERT INTO gs050.edges VALUES(1001,70,700);")
        after_recovery_reuse = info()
        if recovery_free >= 2 and relation_blocks() != recovery_blocks:
            raise RuntimeError("recovered free list was not reusable without relation growth")
        if recovery_free >= 2 and after_recovery_reuse["free_pages"] > recovery_free - 2:
            raise RuntimeError("recovered free-list head was not consumed")
        if edge_ids() != "2,1001":
            raise RuntimeError("post-recovery reuse changed logical edge results")

        results = {
            "task_id": TASK_ID,
            "finished": now(),
            "catalog_refresh": catalog_refresh,
            "initial": initial,
            "stable_refs": {"key10": ref10, "key20": ref20},
            "single_owner_compaction": compacted_one,
            "retired_debt_on_shared_page": shared_orphan,
            "shared_owner_reclaim_guard": shared_owner_guard,
            "after_callback_vacuum": after_delete_vacuum,
            "slot_after_last_edge_delete": ref20_after_delete,
            "whole_page_reuse": {
                "free_pages_before": free_before_reuse,
                "blocks_before": blocks_before_reuse,
                "after": after_reuse,
                "new_ref": ref60,
            },
            "maintenance_serialization": {
                "sealed_state": sealed_during_compaction,
                "compactor": compactor_result,
                "vacuum": vacuum_result,
                "vacuum_elapsed_ms": vacuum_elapsed_ms,
                "after_cutoff_chosen_with_old_compactor": after_serialized_vacuum,
                "after_advanced_horizon": after_horizon_vacuum,
            },
            "crash_recovery": {
                "rows_before": before_recovery_rows,
                "before": before_recovery,
                "rows_after": after_recovery_rows,
                "after": recovered,
                "after_reuse": after_recovery_reuse,
            },
            "commands": commands,
        }
        write(out / "results.json", results)
    finally:
        run_logged(stop, out, "post-stop", cwd=WORKSPACE, timeout=120)

    record = {
        "task_id": TASK_ID,
        "status": "PASS",
        "assertions_passed": [
            "no_tid_resurrection",
            "vacuum_compaction_order",
            "orphan_page_reclaim",
            "retired_debt_accounted",
            "slot_aba_reuse_horizon",
            "all_shared_owners_before_page_reuse",
        ],
        "evidence_files": [
            evidence(out / "results.json"),
            evidence(out / "build.log"),
            evidence(out / "shared-owner-vacuum.log"),
            evidence(out / "dead-tid-vacuum.log"),
            evidence(out / "post-horizon-vacuum.log"),
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
