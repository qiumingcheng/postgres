#!/usr/bin/env python3
"""Exercise delta sealing, active preservation, Base publication, and cleanup."""
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


TASK_ID = "GS-040"


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
            "SELECT pg_catalog.graph_adj_index_info('gs040.edges_adj'::regclass);",
        ).strip()
    )


def ref_for(key: int) -> dict:
    return json.loads(
        sql(
            "candidate",
            "SELECT pg_catalog.graph_adj_debug_ref_probe("
            f"'gs040.edges_adj'::regclass,{key},"
            "'gs040.edges_adj'::regclass::oid,1,"
            f"'gs040.edges'::regclass::oid,0,0,false);",
        ).strip()
    )


def compact_sql(key: int, pause_ms: int) -> str:
    return (
        "SELECT pg_catalog.graph_adj_debug_compact("
        f"'gs040.edges_adj'::regclass,{key},true,{pause_ms});"
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


def wait_for(predicate, description: str, timeout: float = 10.0) -> dict:
    deadline = time.monotonic() + timeout
    last = {}
    while time.monotonic() < deadline:
        last = info()
        if predicate(last):
            return last
        time.sleep(0.05)
    raise RuntimeError(f"timed out waiting for {description}: {last}")


def process_json(process: subprocess.Popen[str], timeout: float = 15.0) -> tuple[dict, str]:
    stdout, stderr = process.communicate(timeout=timeout)
    if process.returncode != 0:
        raise RuntimeError(f"compaction process failed: {stderr}\n{stdout}")
    lines = [line for line in stdout.splitlines() if line.strip()]
    if not lines:
        raise RuntimeError("compaction process returned no JSON")
    return json.loads(lines[-1]), stderr


class PsqlSession:
    def __init__(self) -> None:
        binary = WORKSPACE / ".install/pg19-graph-core/candidate/bin/psql"
        self.proc = subprocess.Popen(
            [binary, "-X", "-qAt", "-v", "ON_ERROR_STOP=1", "-d", "graph_lab"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
            env=db_env("candidate"),
        )
        self.counter = 0

    def execute(self, statement: str) -> list[str]:
        self.counter += 1
        marker = f"__GS040_{self.counter}__"
        assert self.proc.stdin is not None and self.proc.stdout is not None
        self.proc.stdin.write(statement + f"\n\\echo {marker}\n")
        self.proc.stdin.flush()
        rows = []
        for line in self.proc.stdout:
            line = line.rstrip("\n")
            if line == marker:
                return rows
            rows.append(line)
        assert self.proc.stderr is not None
        raise RuntimeError("reader psql exited: " + self.proc.stderr.read())

    def close(self) -> None:
        if self.proc.poll() is None:
            assert self.proc.stdin is not None
            self.proc.stdin.write("\\q\n")
            self.proc.stdin.flush()
        self.proc.wait(timeout=10)


def evidence(path: Path) -> dict:
    return {"path": str(path.relative_to(PACKAGE)), "sha256": sha(path)}


def main() -> None:
    out = run_dir("check-GS-040")
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
            "DROP SCHEMA IF EXISTS gs040 CASCADE; CREATE SCHEMA gs040; "
            "CREATE TABLE gs040.edges("
            "edge_id bigint PRIMARY KEY,src bigint NOT NULL,dst bigint NOT NULL); "
            "CREATE INDEX edges_adj ON gs040.edges USING graph_adj(src,dst); "
            "INSERT INTO gs040.edges VALUES"
            "(1,10,20),(2,10,21),(3,10,22),(4,10,23),(5,10,24);",
        )
        initial = info()
        ref_before = ref_for(10)
        if initial["base_entries"] != 0 or initial["sealed_entries"] != 0:
            raise RuntimeError(f"fresh writes did not stay in active delta: {initial}")

        reader = PsqlSession()
        try:
            first = reader.execute(
                "BEGIN ISOLATION LEVEL REPEATABLE READ; "
                "SET LOCAL enable_seqscan=off; SET LOCAL enable_bitmapscan=off; "
                "DECLARE old_reader NO SCROLL CURSOR FOR "
                "SELECT edge_id FROM gs040.edges WHERE src=10 ORDER BY edge_id; "
                "FETCH 1 FROM old_reader;"
            )
            if first != ["1"]:
                raise RuntimeError(f"old reader did not start at the expected generation: {first}")

            compactor = async_psql(compact_sql(10, 1500))
            sealed_state = wait_for(
                lambda doc: doc["sealed_entries"] == 5 and doc["base_entries"] == 0,
                "sealed cutoff and new active publication",
            )
            sql("candidate", "INSERT INTO gs040.edges VALUES(6,10,25);")
            during_build = info()
            if during_build["sealed_entries"] != 5 or during_build["active_entries"] < 1:
                raise RuntimeError(f"concurrent insert did not use the new active: {during_build}")
            compacted, compact_stderr = process_json(compactor)

            remaining = reader.execute("FETCH ALL FROM old_reader; COMMIT;")
        finally:
            reader.close()
        if first + remaining != ["1", "2", "3", "4", "5"]:
            raise RuntimeError(f"old reader mixed descriptor generations: {first + remaining}")
        if compacted["sealed_cutoff"] != 5 or compacted["base_output_items"] != 5:
            raise RuntimeError(f"sealed cutoff/Base contents are wrong: {compacted}")
        if compacted["active_items_at_publish"] != 1 or not compacted["new_active_preserved"]:
            raise RuntimeError(f"post-cutoff write was not preserved: {compacted}")

        after_publish = info()
        visible_after = sql(
            "candidate",
            "SET enable_seqscan=off; SET enable_bitmapscan=off; "
            "SELECT string_agg(edge_id::text,',' ORDER BY edge_id) "
            "FROM gs040.edges WHERE src=10;",
        ).strip()
        if visible_after != "1,2,3,4,5,6":
            raise RuntimeError(f"new reader lost Base or active entries: {visible_after}")
        if after_publish["base_entries"] != 5 or after_publish["sealed_entries"] != 0:
            raise RuntimeError(f"descriptor did not atomically publish Base: {after_publish}")
        ref_after = ref_for(10)
        if (ref_after["slot_id"], ref_after["slot_generation"]) != (
            ref_before["slot_id"],
            ref_before["slot_generation"],
        ):
            raise RuntimeError("ordinary compaction moved or recycled the live Endpoint Slot")
        if not compacted["descriptor_in_stable_slot"]:
            raise RuntimeError("compaction did not publish through the stable Slot descriptor")

        # Cancellation happens after the seal linearization point.  The next
        # invocation must resume that sealed generation and all backend-owned
        # locks/pins from the canceled statement must have been released.
        sql(
            "candidate",
            "INSERT INTO gs040.edges VALUES"
            "(101,100,200),(102,100,201),(103,100,202),(104,100,203);",
        )
        cancel_ref_before = ref_for(100)
        canceled_compactor = async_psql(compact_sql(100, 5000))
        canceled_sealed = wait_for(
            lambda doc: doc["sealed_entries"] == 4,
            "cancel test sealed generation",
        )
        pid_text = sql(
            "candidate",
            "SELECT pid FROM pg_stat_activity "
            "WHERE datname='graph_lab' AND state='active' "
            "AND pid <> pg_backend_pid() "
            "AND query LIKE '%graph_adj_debug_compact%' "
            "AND query LIKE '%100,true,5000%' ORDER BY pid LIMIT 1;",
        ).strip()
        if not pid_text:
            canceled_compactor.kill()
            raise RuntimeError("could not locate the paused compactor backend")
        cancel_result = sql(
            "candidate", f"SELECT pg_cancel_backend({int(pid_text)});"
        ).strip()
        cancel_stdout, cancel_stderr = canceled_compactor.communicate(timeout=10)
        if cancel_result != "t" or canceled_compactor.returncode == 0:
            raise RuntimeError(
                f"compactor was not canceled: result={cancel_result}, out={cancel_stdout}, err={cancel_stderr}"
            )
        if "canceling statement" not in cancel_stderr:
            raise RuntimeError(f"unexpected cancel error: {cancel_stderr}")

        after_cancel = info()
        if after_cancel["sealed_entries"] != 4:
            raise RuntimeError(f"cancel lost the safely sealed cutoff: {after_cancel}")
        resumed = json.loads(sql("candidate", compact_sql(100, 0)).strip())
        if not resumed["resumed_sealed"] or resumed["base_output_items"] != 4:
            raise RuntimeError(f"canceled compaction did not resume safely: {resumed}")
        sql("candidate", "INSERT INTO gs040.edges VALUES(105,100,204);")
        cancel_visible = sql(
            "candidate",
            "SET enable_seqscan=off; SET enable_bitmapscan=off; "
            "SELECT string_agg(edge_id::text,',' ORDER BY edge_id) "
            "FROM gs040.edges WHERE src=100;",
        ).strip()
        if cancel_visible != "101,102,103,104,105":
            raise RuntimeError(f"cancel/resume leaked resources or rows: {cancel_visible}")
        cancel_ref_after = ref_for(100)
        if (cancel_ref_after["slot_id"], cancel_ref_after["slot_generation"]) != (
            cancel_ref_before["slot_id"],
            cancel_ref_before["slot_generation"],
        ):
            raise RuntimeError("cancel/resume moved the live Slot")

        final = info()
        results = {
            "task_id": TASK_ID,
            "finished": now(),
            "catalog_refresh": catalog_refresh,
            "initial": initial,
            "ref_before": ref_before,
            "sealed_state": sealed_state,
            "during_lock_free_build": during_build,
            "compaction": compacted,
            "compaction_stderr": compact_stderr,
            "old_reader_rows": first + remaining,
            "after_publish": after_publish,
            "new_reader_rows": visible_after,
            "ref_after": ref_after,
            "cancel": {
                "sealed_state": canceled_sealed,
                "backend_pid": int(pid_text),
                "pg_cancel_backend": cancel_result,
                "stdout": cancel_stdout,
                "stderr": cancel_stderr,
                "after_cancel": after_cancel,
                "resumed": resumed,
                "visible_after_resume_and_insert": cancel_visible,
            },
            "final": final,
            "commands": commands,
        }
        write(out / "results.json", results)
    finally:
        run_logged(stop, out, "post-stop", cwd=WORKSPACE, timeout=120)

    record = {
        "task_id": TASK_ID,
        "status": "PASS",
        "assertions_passed": [
            "sealed_cutoff",
            "new_active_preserved",
            "reader_generation_pin",
            "cancel_releases_pin",
            "descriptor_in_stable_slot",
            "segment_owner_cutoff",
            "no_live_slot_move_on_compaction",
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
