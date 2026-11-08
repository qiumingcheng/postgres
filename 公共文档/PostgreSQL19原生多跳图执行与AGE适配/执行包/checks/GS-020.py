#!/usr/bin/env python3
"""Execute the GS-020 dual-direction append and transaction boundary contract."""
from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys

SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS))

from common import PACKAGE, WORKSPACE, now, run_dir, run_logged, sha, write  # noqa: E402
from lab import sql  # noqa: E402


TASK_ID = "GS-020"


def index_info(name: str) -> dict:
    return json.loads(
        sql(
            "candidate",
            f"SELECT pg_catalog.graph_adj_index_info('{name}'::regclass);",
        ).strip()
    )


def require_balanced(info: dict, visible: int | None = None) -> None:
    if not info["dual_direction_physical_balanced"]:
        raise RuntimeError("OUT and IN physical publication diverged")
    if info["out_physical_entries"] != info["in_physical_entries"]:
        raise RuntimeError("OUT/IN physical counts differ")
    if info["out_visible_entries"] != info["in_visible_entries"]:
        raise RuntimeError("OUT/IN visible counts differ")
    if visible is not None and info["out_visible_entries"] != visible:
        raise RuntimeError(
            f"expected {visible} visible edges, got {info['out_visible_entries']}"
        )
    if not info["canonical_mapping_valid"] or not info["opposite_refs_valid"]:
        raise RuntimeError("canonical Slot or opposite VertexRef validation failed")


def evidence(path: Path) -> dict:
    return {"path": str(path.relative_to(PACKAGE)), "sha256": sha(path)}


def main() -> None:
    out = run_dir("check-GS-020")
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
    commands.append(
        run_logged(
            [python, lab, "start", "--variant=candidate"],
            out,
            "start",
            cwd=WORKSPACE,
            timeout=120,
        )
    )
    try:
        sql(
            "candidate",
            "DROP SCHEMA IF EXISTS gs020 CASCADE; CREATE SCHEMA gs020; "
            "CREATE TABLE gs020.edges("
            "edge_id bigint PRIMARY KEY, src bigint NOT NULL, dst bigint NOT NULL); "
            "CREATE INDEX edges_adj ON gs020.edges USING graph_adj(src,dst);",
        )
        empty = index_info("gs020.edges_adj")
        require_balanced(empty, 0)
        if empty["slot_count"] != 0 or empty["data_pages"] != 0:
            raise RuntimeError("empty index allocated Slot/data contents")

        wal_before = sql("candidate", "SELECT pg_current_wal_insert_lsn();").strip()
        sql("candidate", "INSERT INTO gs020.edges VALUES(1,10,20);")
        first = index_info("gs020.edges_adj")
        require_balanced(first, 1)
        expected_first = {
            "slot_count": 2,
            "out_directory_items": 2,
            "in_directory_items": 2,
            "out_physical_entries": 1,
            "in_physical_entries": 1,
            "data_pages": 2,
            "empty_direction_heads": 2,
        }
        for key, value in expected_first.items():
            if first[key] != value:
                raise RuntimeError(f"first append {key}: {first[key]} != {value}")

        sql("candidate", "INSERT INTO gs020.edges VALUES(2,10,30);")
        second = index_info("gs020.edges_adj")
        require_balanced(second, 2)
        if second["slot_count"] != 3 or second["data_pages"] != 3:
            raise RuntimeError("existing source Slot/OUT segment was not reused")
        if second["empty_direction_heads"] != 3:
            raise RuntimeError("empty directions allocated data pages eagerly")

        inside_abort = json.loads(
            sql(
                "candidate",
                "BEGIN; INSERT INTO gs020.edges VALUES(3,40,50); "
                "SELECT pg_catalog.graph_adj_index_info("
                "'gs020.edges_adj'::regclass); ROLLBACK;",
            ).strip()
        )
        require_balanced(inside_abort, 3)
        after_abort = index_info("gs020.edges_adj")
        require_balanced(after_abort, 2)
        if after_abort["out_physical_entries"] < 3:
            raise RuntimeError("abort test did not leave an MVCC-filtered physical candidate")

        before_speculative = after_abort["out_visible_entries"]
        returned = sql(
            "candidate",
            "INSERT INTO gs020.edges VALUES(1,60,70) "
            "ON CONFLICT(edge_id) DO NOTHING RETURNING edge_id;",
        ).strip()
        if returned:
            raise RuntimeError("speculative conflict unexpectedly inserted a heap row")
        after_speculative = index_info("gs020.edges_adj")
        require_balanced(after_speculative, before_speculative)

        sql("candidate", "INSERT INTO gs020.edges VALUES(3,40,50);")
        committed = index_info("gs020.edges_adj")
        require_balanced(committed, 3)
        if committed["out_physical_entries"] <= committed["out_visible_entries"]:
            raise RuntimeError("aborted candidates were not distinguished from visible edges")
        if not committed["all_mutable_pages_have_lsn"]:
            raise RuntimeError("a published mutable page has no WAL LSN")
        wal_after = sql("candidate", "SELECT pg_current_wal_insert_lsn();").strip()
        wal_advanced = sql(
            "candidate",
            f"SELECT pg_wal_lsn_diff('{wal_after}','{wal_before}') > 0;",
        ).strip()
        if wal_advanced != "t":
            raise RuntimeError("WAL insert position did not advance across graph_adj writes")

        sql(
            "candidate",
            "CREATE TABLE gs020.prebuilt("
            "edge_id bigint,src bigint NOT NULL,dst bigint NOT NULL); "
            "INSERT INTO gs020.prebuilt VALUES(1,1,2),(2,2,3),(3,3,3); "
            "CREATE INDEX prebuilt_adj ON gs020.prebuilt USING graph_adj(src,dst);",
        )
        prebuilt = index_info("gs020.prebuilt_adj")
        require_balanced(prebuilt, 3)
        if prebuilt["slot_count"] != 3:
            raise RuntimeError("build over existing rows did not canonicalize endpoints")

        sql(
            "candidate",
            "CREATE TABLE gs020.copied("
            "edge_id bigint,src bigint NOT NULL,dst bigint NOT NULL); "
            "CREATE INDEX copied_adj ON gs020.copied USING graph_adj(src,dst); "
            "COPY gs020.copied FROM STDIN WITH (FORMAT csv);\n"
            "1,100,200\n2,100,300\n3,300,100\n\\.\n",
        )
        copied = index_info("gs020.copied_adj")
        require_balanced(copied, 3)

        source = (
            WORKSPACE
            / ".worktrees/pg19-graph-core/postgres19/src/backend/access/graphadj/graphadj.c"
        ).read_text()
        if source.count("GenericXLogStart(index)") < 2:
            raise RuntimeError("Slot/directory and data publication do not both use Generic WAL")
        if "Complete entry first; this published boundary" not in source:
            raise RuntimeError("entry-before-published boundary is not explicit")

        results = {
            "task_id": TASK_ID,
            "finished": now(),
            "empty": empty,
            "first_committed": first,
            "second_committed": second,
            "inside_aborting_transaction": inside_abort,
            "after_abort": after_abort,
            "after_speculative_conflict": after_speculative,
            "after_reusing_aborted_slots": committed,
            "prebuilt_index": prebuilt,
            "copy_into_index": copied,
            "wal": {
                "before": wal_before,
                "after": wal_after,
                "advanced": True,
                "all_mutable_pages_have_lsn": True,
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
            "out_in_before_return",
            "abort_speculative_visibility",
            "wal_before_data",
            "canonical_slot_unique_out_in",
            "opposite_ref_persisted_both_directions",
            "empty_direction_lazy_allocate",
        ],
        "evidence_files": [evidence(out / "results.json"), evidence(out / "build.log")],
    }
    print(json.dumps(record, ensure_ascii=False, sort_keys=True))


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError, ValueError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
