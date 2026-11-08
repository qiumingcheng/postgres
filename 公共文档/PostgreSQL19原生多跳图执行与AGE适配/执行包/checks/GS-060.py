#!/usr/bin/env python3
"""Exercise conservative SSI protection for empty graph adjacency reads."""
from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys

SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS))

from common import PACKAGE, WORKSPACE, now, read, run_dir, run_logged, sha, write  # noqa: E402
from lab import db_env, sql  # noqa: E402


TASK_ID = "GS-060"
SCHEMA = "gs060"
GOLDEN = PACKAGE / "golden/GS-060/ssi_interleaving.json"


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


def index_info() -> dict:
    return json.loads(
        sql(
            "candidate",
            "SELECT pg_catalog.graph_adj_index_info('gs060.edges_adj'::regclass);",
        ).strip()
    )


class PsqlSession:
    """Persistent psql session which keeps running after an expected SQL error."""

    def __init__(self, label: str) -> None:
        binary = WORKSPACE / ".install/pg19-graph-core/candidate/bin/psql"
        self.label = label
        self.proc = subprocess.Popen(
            [binary, "-X", "-qAt", "-v", "ON_ERROR_STOP=0", "-d", "graph_lab"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
            env=db_env("candidate"),
        )
        self.counter = 0
        self.transcript: list[dict] = []
        self.execute(r"\set VERBOSITY verbose")

    def execute(self, statement: str) -> list[str]:
        self.counter += 1
        marker = f"__GS060_{self.label}_{self.counter}__"
        if self.proc.poll() is not None:
            raise RuntimeError(f"{self.label} psql exited unexpectedly")
        assert self.proc.stdin is not None and self.proc.stdout is not None
        self.proc.stdin.write(statement + f"\n\\echo {marker}\n")
        self.proc.stdin.flush()
        rows: list[str] = []
        for line in self.proc.stdout:
            line = line.rstrip("\n")
            if line == marker:
                self.transcript.append({"sql": statement, "output": rows})
                return rows
            rows.append(line)
        raise RuntimeError(f"{self.label} psql exited before marker: {rows}")

    def close(self) -> None:
        if self.proc.poll() is None:
            assert self.proc.stdin is not None
            self.proc.stdin.write("\\q\n")
            self.proc.stdin.flush()
        self.proc.wait(timeout=10)


def only_value(rows: list[str], label: str) -> str:
    values = [row for row in rows if row and not row.startswith(("ERROR:", "LOCATION:"))]
    if len(values) != 1:
        raise RuntimeError(f"{label} returned unexpected output: {rows}")
    return values[0]


def predicate_locks(pids: list[int]) -> list[dict]:
    pid_list = ",".join(str(pid) for pid in pids)
    raw = sql(
        "candidate",
        "SELECT coalesce(json_agg(x ORDER BY pid),'[]'::json) FROM ("
        "SELECT pid,locktype,mode,relation::regclass::text AS relation_name,"
        "page,tuple,granted FROM pg_locks "
        "WHERE pid IN (" + pid_list + ") "
        "AND relation='gs060.edges_adj'::regclass AND mode='SIReadLock') x;",
    ).strip()
    return json.loads(raw)


def assert_relation_sireads(locks: list[dict], pids: list[int], phase: str) -> None:
    by_pid = {entry["pid"]: entry for entry in locks}
    if set(by_pid) != set(pids):
        raise RuntimeError(f"{phase}: missing relation predicate lock: {locks}")
    for entry in locks:
        if (
            entry["locktype"] != "relation"
            or entry["mode"] != "SIReadLock"
            or entry["page"] is not None
            or entry["tuple"] is not None
            or not entry["granted"]
        ):
            raise RuntimeError(f"{phase}: predicate lock is not relation-wide: {entry}")


def evidence(path: Path) -> dict:
    return {"path": str(path.relative_to(PACKAGE)), "sha256": sha(path)}


def main() -> None:
    out = run_dir("check-GS-060")
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

    reader_a: PsqlSession | None = None
    reader_b: PsqlSession | None = None
    retry: PsqlSession | None = None
    try:
        values = ",".join(
            "(" + ",".join(str(value) for value in edge) + ")"
            for edge in golden["initial_edges"]
        )
        sql(
            "candidate",
            "DROP SCHEMA IF EXISTS gs060 CASCADE; CREATE SCHEMA gs060; "
            "CREATE TABLE gs060.edges("
            "edge_id bigint PRIMARY KEY,src bigint NOT NULL,dst bigint NOT NULL); "
            "CREATE INDEX edges_adj ON gs060.edges USING graph_adj(src,dst); "
            f"INSERT INTO gs060.edges VALUES {values};",
        )
        initial = index_info()
        expected = golden["initial_endpoints_per_direction"]
        if initial["out_directory_items"] != expected or initial["in_directory_items"] != expected:
            raise RuntimeError(f"initial directory did not reach the split boundary: {initial}")
        if initial["directory_split_observed"]:
            raise RuntimeError("directory split happened before the scheduled interleaving")

        reader_a = PsqlSession("batch")
        reader_b = PsqlSession("ordinary")
        reader_a.execute("BEGIN ISOLATION LEVEL SERIALIZABLE;")
        reader_b.execute("BEGIN ISOLATION LEVEL SERIALIZABLE;")
        pid_a = int(only_value(reader_a.execute("SELECT pg_backend_pid();"), "batch pid"))
        pid_b = int(only_value(reader_b.execute("SELECT pg_backend_pid();"), "ordinary pid"))

        batch_walk_text = only_value(
            reader_a.execute(
                "SELECT pg_catalog.graph_adj_debug_walk("
                f"'gs060.edges_adj'::regclass,{golden['batch_empty_endpoint']},1);"
            ),
            "batch empty walk",
        )
        batch_walk = json.loads(batch_walk_text)

        plan_rows = reader_b.execute(
            "SET LOCAL enable_seqscan=off; SET LOCAL enable_bitmapscan=off; "
            "EXPLAIN (COSTS OFF) SELECT edge_id FROM gs060.edges "
            f"WHERE src={golden['ordinary_empty_endpoint']};"
        )
        plan = "\n".join(plan_rows)
        if "Index Scan using edges_adj" not in plan:
            raise RuntimeError(f"ordinary empty read did not use graph_adj: {plan_rows}")
        empty_count = only_value(
            reader_b.execute(
                "SELECT count(*) FROM gs060.edges "
                f"WHERE src={golden['ordinary_empty_endpoint']};"
            ),
            "ordinary empty scan",
        )
        if empty_count != "0":
            raise RuntimeError(f"ordinary endpoint was not empty: {empty_count}")

        locks_before_split = predicate_locks([pid_a, pid_b])
        assert_relation_sireads(locks_before_split, [pid_a, pid_b], "before split")

        split_edge = golden["split_writer_edge"]
        split_write = reader_a.execute(
            "INSERT INTO gs060.edges VALUES "
            f"({split_edge[0]},{split_edge[1]},{split_edge[2]}); COMMIT;"
        )
        if any("ERROR:" in row for row in split_write):
            raise RuntimeError(f"first writer unexpectedly lost the SSI race: {split_write}")
        after_split = index_info()
        if not after_split["directory_split_observed"]:
            raise RuntimeError(f"scheduled write did not publish a directory split: {after_split}")
        locks_after_split = predicate_locks([pid_a, pid_b])
        assert_relation_sireads(locks_after_split, [pid_a, pid_b], "after split")

        retry_edge = golden["retry_writer_edge"]
        failed_write = reader_b.execute(
            "INSERT INTO gs060.edges VALUES "
            f"({retry_edge[0]},{retry_edge[1]},{retry_edge[2]}); COMMIT;"
        )
        serialization_errors = [row for row in failed_write if "40001" in row]
        if len(serialization_errors) != golden["expected_serialization_failures"]:
            raise RuntimeError(f"expected one SQLSTATE 40001 serialization failure: {failed_write}")
        reader_b.execute("ROLLBACK;")

        retry = PsqlSession("retry")
        retry.execute("BEGIN ISOLATION LEVEL SERIALIZABLE;")
        retry_output = retry.execute(
            "INSERT INTO gs060.edges VALUES "
            f"({retry_edge[0]},{retry_edge[1]},{retry_edge[2]}); COMMIT;"
        )
        if any("ERROR:" in row for row in retry_output):
            raise RuntimeError(f"business retry did not commit: {retry_output}")

        attempts = 2
        retries = attempts - 1
        final_rows = int(sql("candidate", "SELECT count(*) FROM gs060.edges;").strip())
        final_ids = sql(
            "candidate",
            "SELECT string_agg(edge_id::text,',' ORDER BY edge_id) FROM gs060.edges;",
        ).strip()
        expected_ids = [edge[0] for edge in golden["initial_edges"]] + [
            split_edge[0],
            retry_edge[0],
        ]
        if final_rows != golden["expected_final_rows"]:
            raise RuntimeError(f"retry accounting changed final row count: {final_rows}")
        if final_ids != ",".join(str(value) for value in sorted(expected_ids)):
            raise RuntimeError(f"committed business edges differ from schedule: {final_ids}")
        if retries != golden["expected_business_retries"]:
            raise RuntimeError(f"business retry count is wrong: {retries}")

        final_info = index_info()
        if final_info["out_visible_entries"] != final_rows or final_info["in_visible_entries"] != final_rows:
            raise RuntimeError(f"visible adjacency copies are not balanced: {final_info}")

        results = {
            "task_id": TASK_ID,
            "finished": now(),
            "golden": str(GOLDEN.relative_to(PACKAGE)),
            "catalog_refresh": catalog_refresh,
            "initial": initial,
            "sessions": {"batch_pid": pid_a, "ordinary_pid": pid_b},
            "batch_empty_walk": batch_walk,
            "ordinary_empty_plan": plan_rows,
            "predicate_locks_before_split": locks_before_split,
            "split_writer_output": split_write,
            "after_split": after_split,
            "predicate_locks_after_split": locks_after_split,
            "serialization_failure_output": failed_write,
            "business_retry": {
                "attempts": attempts,
                "serialization_failures": len(serialization_errors),
                "retries": retries,
                "successful_commits": 1,
                "retry_output": retry_output,
            },
            "final": {"row_count": final_rows, "edge_ids": final_ids, "index": final_info},
            "transcripts": {
                "batch": reader_a.transcript,
                "ordinary": reader_b.transcript,
                "retry": retry.transcript,
            },
            "commands": commands,
        }
        write(out / "results.json", results)
    finally:
        for session in (retry, reader_b, reader_a):
            if session is not None:
                session.close()
        run_logged(stop, out, "post-stop", cwd=WORKSPACE, timeout=120)

    record = {
        "task_id": TASK_ID,
        "status": "PASS",
        "assertions_passed": [
            "batch_predicate_lock",
            "empty_range_conflict",
            "business_retries_accounted",
        ],
        "evidence_files": [
            evidence(out / "results.json"),
            evidence(out / "build.log"),
            evidence(out / "start.log"),
        ],
    }
    print(json.dumps(record, ensure_ascii=False, sort_keys=True))


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError, ValueError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
