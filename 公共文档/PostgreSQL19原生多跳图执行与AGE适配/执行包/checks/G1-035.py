#!/usr/bin/env python3
"""Verify public bounded candidate batching over the real B-tree provider."""
from __future__ import annotations

from collections import Counter
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time

SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS))
from common import PACKAGE, WORKSPACE, now, read, run_dir, run_logged, sha, write  # noqa: E402
from lab import db_env  # noqa: E402

TASK_ID = "G1-035"
SOURCE = WORKSPACE / ".worktrees/pg19-graph-core/postgres19"
INSTALL = WORKSPACE / ".install/pg19-graph-core/candidate"
MODULE = SOURCE / "src/test/modules/test_graph_core"
GOLDEN = PACKAGE / "golden/G1-035/contracts.json"


class Session:
    def __init__(self, name: str) -> None:
        self.name = name
        self.calls = 0
        self.transcript: list[dict] = []
        self.process = subprocess.Popen(
            [INSTALL / "bin/psql", "-X", "-qAt", "-d", "graph_lab"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, bufsize=1, env=db_env("candidate"),
        )
        self.execute(r"\set VERBOSITY verbose")
        self.execute("SET statement_timeout='30s'; SET enable_indexscan=off; SET enable_bitmapscan=off;")
        self.pid = int(self.value("SELECT pg_backend_pid();"))

    def send(self, sql: str) -> tuple[str, str]:
        self.calls += 1
        token = f"__G1035_{self.name}_{self.calls}__"
        assert self.process.stdin
        self.process.stdin.write(sql + f"\n\\echo {token}\n")
        self.process.stdin.flush()
        return token, sql

    def finish(self, pending: tuple[str, str], error: str | None = None) -> list[str]:
        token, sql = pending
        rows: list[str] = []
        assert self.process.stdout
        for line in self.process.stdout:
            line = line.rstrip("\n")
            if line == token:
                self.transcript.append({"sql": sql, "output": rows})
                errors = [row for row in rows if row.startswith("ERROR:")]
                if error and not any(error in row for row in errors):
                    raise RuntimeError(f"{self.name}: expected {error}, got {rows}")
                if not error and errors:
                    raise RuntimeError(f"{self.name}: unexpected SQL error: {rows}")
                return rows
            rows.append(line)
        raise RuntimeError(self.name + ": psql exited before completion marker")

    def execute(self, sql: str, error: str | None = None) -> list[str]:
        return self.finish(self.send(sql), error)

    def value(self, sql: str) -> str:
        rows = [row for row in self.execute(sql) if row and not row.startswith("NOTICE:")]
        if len(rows) != 1:
            raise RuntimeError(f"{self.name}: expected one value, got {rows}")
        return rows[0]

    def close(self) -> None:
        if self.process.poll() is None:
            assert self.process.stdin
            self.process.stdin.write("\\q\n")
            self.process.stdin.flush()
        self.process.wait(timeout=10)


def call(keys: list[int], ordinals: list[int] | None = None, *,
         max_endpoints: int = 2, max_candidates: int = 8, max_pages: int = 4,
         candidate_bytes: int = 1472, restore_bytes: int = 1048576,
         first_row: bool = False, small_threshold: int = 0,
         small_limit: int = 1, prefetch: bool = True, mode: int = 0,
         pause_ms: int = 0) -> str:
    ordinals = ordinals or list(range(1, len(keys) + 1))
    key_array = "ARRAY[" + ",".join(map(str, keys)) + "]::bigint[]"
    ordinal_array = "ARRAY[" + ",".join(map(str, ordinals)) + "]::bigint[]"
    return (
        "SELECT test_graph_batch('g1035.edge','g1035.edge_out',1,"
        f"{key_array},{ordinal_array},{max_endpoints},{max_candidates},{max_pages},"
        f"{candidate_bytes},{restore_bytes},{str(first_row).lower()},"
        f"{small_threshold},{small_limit},{str(prefetch).lower()},{mode},{pause_ms});"
    )


def cleanup_state(session: Session) -> dict:
    return json.loads(session.value("SELECT test_graph_batch_cleanup_state();"))


def wait_paused(observer: Session, pid: int) -> None:
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        if observer.value(
            f"SELECT coalesce((SELECT wait_event_type='Extension' AND state='active' "
            f"FROM pg_stat_activity WHERE pid={pid}),false);"
        ) == "t":
            return
        time.sleep(0.02)
    raise RuntimeError("batch reader did not reach its cancellable provider wait")


def visible_oracle(session: Session, keys: list[int]) -> Counter:
    array = "ARRAY[" + ",".join(map(str, keys)) + "]::bigint[]"
    rows = json.loads(session.value(
        "SELECT coalesce(json_agg(json_build_array(999+k.ord,e.id,e.dst)), '[]'::json) "
        f"FROM unnest({array}) WITH ORDINALITY k(key,ord) JOIN g1035.edge e ON e.src=k.key;"
    ))
    return Counter(tuple(row) for row in rows)


def actual_bag(result: dict, phase: int = 0) -> Counter:
    return Counter((row["parent_id"], row["edge_id"], row["opposite"])
                   for row in result["rows"] if row["phase"] == phase)


def evidence(path: Path) -> dict:
    return {"path": str(path.relative_to(PACKAGE)), "sha256": sha(path)}


def main() -> None:
    golden = read(GOLDEN)
    limits = golden["limits"]
    out = run_dir("check-G1-035")
    lab = PACKAGE / "scripts/lab.py"
    commands: list[dict] = []
    sessions: list[Session] = []
    results: dict = {}
    started = False
    try:
        commands.append(run_logged([sys.executable, lab, "stop", "--variant=candidate"],
                                   out, "pre-stop", timeout=120))
        commands.append(run_logged([sys.executable, lab, "build", "--variant=candidate", "--jobs=4"],
                                   out, "build", timeout=1800))
        make = ["make", "-C", MODULE, "USE_PGXS=1",
                f"PG_CONFIG={INSTALL / 'bin/pg_config'}"]
        for label, args in (("clean", ["clean"]), ("build", ["-j4"]), ("install", ["install"])):
            commands.append(run_logged(make + args, out, "module-" + label, timeout=180))
        commands.append(run_logged([sys.executable, lab, "start", "--variant=candidate"],
                                   out, "start", timeout=180))
        started = True
        commands.append(run_logged(make + ["installcheck"], out, "native-regression",
                                   env=db_env("candidate"), timeout=180))
        shutil.copyfile(MODULE / "results/graph_batch.out", out / "native-results.out")

        writer, reader, observer = (Session(name) for name in ("writer", "reader", "observer"))
        sessions.extend((writer, reader, observer))
        writer.execute(
            "SET client_min_messages=warning; DROP EXTENSION IF EXISTS test_graph_core CASCADE; "
            "CREATE EXTENSION test_graph_core; DROP SCHEMA IF EXISTS g1035 CASCADE; CREATE SCHEMA g1035; "
            "CREATE TABLE g1035.edge(id bigint PRIMARY KEY,src bigint NOT NULL,dst bigint NOT NULL,payload text) WITH(fillfactor=40); "
            "CREATE INDEX edge_out ON g1035.edge(src); "
            "INSERT INTO g1035.edge VALUES " +
            ",".join(f"({edge},{src},{dst},'before')" for edge, src, dst in golden["fixture"]["edges"]) + "; "
            "INSERT INTO g1035.edge SELECT 10000+i,50,100+(i%17),"
            "(SELECT string_agg(md5((i*100+j)::text),'') FROM generate_series(1,32) j) "
            f"FROM generate_series(1,{golden['fixture']['bulk_edges']}) i; ANALYZE g1035.edge;"
        )

        # One logical probe per equal endpoint, bounded endpoint/candidate slices.
        endpoint = json.loads(reader.value(call(
            [10, 20, 30, 40, 999, 998], max_endpoints=limits["max_endpoints"],
            max_candidates=2, candidate_bytes=2 * limits["candidate_item_bytes"])))
        inst = endpoint["instrumentation"]
        if inst["endpoint_requests"] != 6 or inst["endpoint_batches"] != 3:
            raise RuntimeError("endpoint request slicing did not honor max_endpoints")
        if inst["peak_candidate_count"] > 2 or inst["candidate_batches"] < 3:
            raise RuntimeError("candidate count batching was absent or unbounded")
        results["endpoint_candidate_batches"] = endpoint

        # A complete, small batch proves duplicate-parent bag restoration and order.
        parent_keys = [10, 10, 20, 999]
        parent = json.loads(reader.value(call(
            parent_keys, [30, 10, 20, 40], max_endpoints=8,
            max_candidates=32, candidate_bytes=32 * limits["candidate_item_bytes"])))
        if actual_bag(parent) != visible_oracle(reader, parent_keys):
            raise RuntimeError("restored parent/edge bag differs from an independent SQL join")
        rows = parent["rows"]
        ordering = [(row["parent_ordinal"], row["candidate_ordinal"]) for row in rows]
        if ordering != sorted(ordering):
            raise RuntimeError("promised parent/candidate order was not restored")
        if parent["instrumentation"]["shared_parent_requests"] != 1:
            raise RuntimeError("equal endpoints did not share one physical request")
        if parent["qual_calls"] != parent["row_count"]:
            raise RuntimeError("parent-correlated predicate was not called per binding")
        for row in rows:
            expected_qual = (((row["edge_id"] + row["parent_id"]) & 1) == 0)
            if row["qual_pass"] != expected_qual:
                raise RuntimeError("parent-correlated result was reused across bindings")
        duplicate = [row for row in rows if row["edge_id"] == 1]
        if len({row["qual_pass"] for row in duplicate}) != 2:
            raise RuntimeError("fixture did not prove different quals over one shared candidate")
        results["parent_bag_order_quals"] = parent

        # HOT is followed by public Table AM; physical grouping stays within all limits.
        writer.execute("UPDATE g1035.edge SET payload='hot' WHERE id=1;")
        hot = json.loads(reader.value(call([10], max_candidates=8,
                                           candidate_bytes=8 * limits["candidate_item_bytes"])))
        if not any(row["edge_id"] == 1 and row["root_tid"] != row["visible_tid"]
                   for row in hot["rows"]):
            raise RuntimeError("HOT fixture did not expose root versus visible TID")
        if hot["instrumentation"]["hot_redirects"] < 1:
            raise RuntimeError("batch instrumentation missed a HOT redirect")
        results["hot"] = hot

        bulk = json.loads(reader.value(call(
            [50], max_candidates=limits["max_candidates"],
            max_pages=limits["max_inflight_pages"],
            candidate_bytes=limits["max_candidates"] * limits["candidate_item_bytes"],
            restore_bytes=limits["max_restore_bytes"], prefetch=True)))
        inst = bulk["instrumentation"]
        if bulk["row_count"] != golden["fixture"]["bulk_edges"]:
            raise RuntimeError("multi-page batch lost or duplicated visible edges")
        if inst["tableam_fetches"] != golden["fixture"]["bulk_edges"]:
            raise RuntimeError("visible candidates did not pass through Table AM")
        if inst["heap_page_groups"] <= limits["max_inflight_pages"] or inst["prefetch_submitted"] == 0:
            raise RuntimeError("fixture did not exercise multiple bounded page groups and prefetch")
        if inst["peak_inflight_pages"] > limits["max_inflight_pages"] or inst["peak_pins"] > limits["max_pins"]:
            raise RuntimeError("page or pin peak exceeded the registered limit")
        if inst["peak_candidate_count"] > limits["max_candidates"]:
            raise RuntimeError("candidate count peak exceeded its limit")
        if inst["peak_candidate_bytes"] > limits["max_candidates"] * limits["candidate_item_bytes"]:
            raise RuntimeError("candidate byte peak exceeded its limit")
        if inst["peak_restore_bytes"] > limits["max_restore_bytes"]:
            raise RuntimeError("restore byte peak exceeded its limit")
        results["multi_page_bounded"] = bulk

        byte_limited = json.loads(reader.value(call(
            [10], max_candidates=32,
            candidate_bytes=2 * limits["candidate_item_bytes"])))
        if byte_limited["instrumentation"]["peak_candidate_count"] != 2 or \
                byte_limited["instrumentation"]["candidate_batches"] != 2:
            raise RuntimeError("candidate-byte limit did not shrink the provider batch")
        results["candidate_byte_limit"] = byte_limited

        # First-row and small-frontier policies use the same executor path and explain why.
        first = json.loads(reader.value(call(
            [10], max_candidates=32,
            candidate_bytes=32 * limits["candidate_item_bytes"], first_row=True)))
        if first["instrumentation"]["first_row_batches"] != 1 or \
                first["instrumentation"]["peak_candidate_count"] > 2 or \
                first["instrumentation"]["first_row_us"] <= 0:
            raise RuntimeError("first-row mode was not observable or bounded")
        small = json.loads(reader.value(call(
            [10], max_candidates=32,
            candidate_bytes=32 * limits["candidate_item_bytes"],
            small_threshold=4, small_limit=1)))
        if small["instrumentation"]["peak_candidate_count"] != 1 or \
                small["instrumentation"]["small_batches"] != small["instrumentation"]["candidate_batches"]:
            raise RuntimeError("small-request reason did not drive one-candidate batches")
        results["first_row"] = first
        results["small_request"] = small

        # Rescan repeats the same complete bag without retaining old page/tuple state.
        rescan = json.loads(reader.value(call(
            [10, 20], max_candidates=2,
            candidate_bytes=2 * limits["candidate_item_bytes"], mode=3)))
        if rescan["instrumentation"]["rescan_count"] != 1:
            raise RuntimeError("public batch rescan was not executed")
        oracle = visible_oracle(reader, [10, 20])
        if actual_bag(rescan, 0) != oracle or actual_bag(rescan, 1) != oracle:
            raise RuntimeError("rescan changed the visible parent/edge bag")
        results["rescan"] = rescan

        # Injected provider ERROR occurs inside GgcBatchNext and must call end exactly once.
        cleanup_before = cleanup_state(reader)
        reader.execute(call([10], mode=1), golden["error_sqlstate"])
        cleanup_error = cleanup_state(reader)
        if cleanup_error["active"] != 0 or cleanup_error["end_calls"] != cleanup_before["end_calls"] + 1:
            raise RuntimeError("provider error left a batch/provider scan active")

        # Cancellation is delivered while getnext is live; the same catch path must clean it.
        pending = reader.send(call([50], mode=2, pause_ms=10000))
        wait_paused(observer, reader.pid)
        if observer.value(f"SELECT pg_cancel_backend({reader.pid});") != "t":
            raise RuntimeError("failed to cancel the live graph batch")
        reader.finish(pending, golden["cancel_sqlstate"])
        cleanup_cancel = cleanup_state(reader)
        if cleanup_cancel["active"] != 0 or cleanup_cancel["end_calls"] != cleanup_error["end_calls"] + 1:
            raise RuntimeError("cancel left a batch/provider scan active")
        writer.execute("VACUUM g1035.edge; REINDEX INDEX g1035.edge_out;")
        reusable = json.loads(reader.value(call([10])))
        if actual_bag(reusable) != visible_oracle(reader, [10]):
            raise RuntimeError("backend or index was not reusable after error/cancel cleanup")
        results["cleanup"] = {"before": cleanup_before, "error": cleanup_error,
                              "cancel": cleanup_cancel, "after": reusable}

        source_manifest = {}
        for path in (SOURCE / "src/backend/executor/graph_batch.c",
                     SOURCE / "src/include/executor/graph_batch.h",
                     MODULE / "test_graph_batch.c"):
            source_manifest[str(path.relative_to(WORKSPACE))] = sha(path)
        write(out / "source-manifest.json", source_manifest)

        for session in sessions:
            session.close()
            write(out / (session.name + ".json"), session.transcript)
            if any("WARNING:" in row for entry in session.transcript for row in entry["output"]):
                raise RuntimeError("resource cleanup emitted a warning")
        sessions.clear()
        commands.append(run_logged([sys.executable, lab, "stop", "--variant=candidate"],
                                   out, "post-stop", timeout=120))
        started = False
        results["finished"] = now()
        results["postgres_sha256"] = sha(INSTALL / "bin/postgres")
        results["module_sha256"] = sha(INSTALL / "lib/postgresql/test_graph_core.so")
        results["golden_sha256"] = sha(GOLDEN)
        write(out / "results.json", results)
        write(out / "summary.json", {"status": "PASS", "task_id": TASK_ID,
                                     "finished": now(), "results_sha256": sha(out / "results.json"),
                                     "commands": commands})
        print(json.dumps({
            "task_id": TASK_ID, "status": "PASS",
            "assertions_passed": [
                "endpoint_and_candidate_batches", "heap_group_prefetch_tableam_hot",
                "parent_bag_and_order_restored", "correlated_quals_not_shared",
                "bounded_bytes_pages_pins", "small_batch_reason_and_first_row",
                "rescan_cancel_error_cleanup",
            ],
            "evidence_files": [evidence(path) for path in (
                GOLDEN, out / "results.json", out / "source-manifest.json",
                out / "writer.json", out / "reader.json", out / "observer.json",
                out / "native-results.out", out / "native-regression.log")],
        }, ensure_ascii=False, sort_keys=True))
    finally:
        for session in sessions:
            if session.process.poll() is None:
                session.process.terminate()
            session.process.wait(timeout=10)
            write(out / (session.name + ".json"), session.transcript)
        if started:
            run_logged([sys.executable, lab, "stop", "--variant=candidate"],
                       out, "failure-stop", timeout=120)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
