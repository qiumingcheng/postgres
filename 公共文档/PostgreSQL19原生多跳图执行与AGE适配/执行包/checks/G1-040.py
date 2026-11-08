#!/usr/bin/env python3
"""Verify the selected generic fixed-N graph executor and its batch lifecycle."""
from __future__ import annotations

from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS))
from common import PACKAGE, WORKSPACE, now, read, run_dir, run_logged, sha, write  # noqa: E402
from lab import db_env  # noqa: E402

TASK_ID = "G1-040"
SOURCE = WORKSPACE / ".worktrees/pg19-graph-core/postgres19"
INSTALL = WORKSPACE / ".install/pg19-graph-core/candidate"
MODULE = SOURCE / "src/test/modules/test_graph_core"
GOLDEN = PACKAGE / "golden/G1-040/contracts.json"


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
        self.execute("SET statement_timeout='30s'; SET max_parallel_workers_per_gather=0;")
        self.pid = int(self.value("SELECT pg_backend_pid();"))

    def send(self, sql: str) -> tuple[str, str]:
        self.calls += 1
        token = f"__G1040_{self.name}_{self.calls}__"
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
        rows = [row for row in self.execute(sql)
                if row and not row.startswith(("NOTICE:", "LOG:"))]
        if len(rows) != 1:
            raise RuntimeError(f"{self.name}: expected one value, got {rows}")
        return rows[0]

    def close(self) -> None:
        if self.process.poll() is None:
            assert self.process.stdin
            self.process.stdin.write("\\q\n")
            self.process.stdin.flush()
        self.process.wait(timeout=10)


def payload(eid: int, fragments: int) -> str:
    return "".join(hashlib.md5(str(eid * 1000 + i).encode()).hexdigest()
                   for i in range(1, fragments + 1))


def enumerate_fixed(edges: list[list[int]], anchors: list[int], directions: list[int],
                    fragments: int) -> list[list]:
    payload_hash = {eid: hashlib.md5(payload(eid, fragments).encode()).hexdigest()
                    for eid, _, _ in edges}
    bindings: list[tuple[int, list[int], list[int], list[str]]] = [
        (anchor, [anchor], [], []) for anchor in anchors
    ]
    for direction in directions:
        next_bindings: list[tuple[int, list[int], list[int], list[str]]] = []
        for anchor, vertices, path_edges, hashes in bindings:
            endpoint = vertices[-1]
            for eid, src, dst in edges:
                if direction == 1 and src == endpoint:
                    opposite = dst
                elif direction == 2 and dst == endpoint:
                    opposite = src
                else:
                    continue
                next_bindings.append((anchor, vertices + [opposite],
                                      path_edges + [eid], hashes + [payload_hash[eid]]))
        bindings = next_bindings
    return [[anchor, vertices, path_edges, hashes]
            for anchor, vertices, path_edges, hashes in bindings]


def bag(rows: list[list]) -> Counter:
    return Counter(json.dumps(row, separators=(",", ":"), sort_keys=False)
                   for row in rows)


def config_sql(stage_count: int, directions: list[int], *, pause_ms: int,
               anchor_batch: int, parent_batch: int, max_bindings: int) -> str:
    if len(directions) != stage_count:
        raise ValueError("stage/direction mismatch")
    direction_array = "ARRAY[" + ",".join(map(str, directions)) + "]"
    index_array = "ARRAY[" + ",".join(
        f"'g1040.edge_{'out' if direction == 1 else 'in'}'::regclass"
        for direction in directions
    ) + "]::regclass[]"
    return (
        "SELECT test_graph_fixed_config('g1040.request',"
        f"array_fill('g1040.edge'::regclass,ARRAY[{stage_count}]),"
        f"{index_array},"
        f"{direction_array},{pause_ms},{anchor_batch},{parent_batch},{max_bindings});"
    )


def actual_sql() -> str:
    return (
        "SELECT coalesce(json_agg(json_build_array(anchor,vertices,edges,"
        "ARRAY(SELECT md5(p) FROM unnest(payloads) AS u(p))) "
        "ORDER BY anchor,edges,vertices),'[]'::json) FROM g1040.request;"
    )


def explain_json(session: Session, analyze: bool = True) -> list:
    options = "ANALYZE, BUFFERS, COSTS OFF, SUMMARY OFF, FORMAT JSON" if analyze \
        else "COSTS OFF, FORMAT JSON"
    rows = session.execute(
        f"EXPLAIN ({options}) SELECT anchor,vertices,edges,payloads FROM g1040.request;"
    )
    return json.loads("\n".join(rows))


def find_custom(plan: dict) -> dict | None:
    if plan.get("Node Type") == "Custom Scan" and \
            plan.get("Custom Plan Provider") == "GraphExpandFixed":
        return plan
    for child in plan.get("Plans", []):
        found = find_custom(child)
        if found:
            return found
    return None


def stage_summary(plan: dict, stage: int) -> dict[str, int]:
    text = plan.get(f"Stage {stage} Batch Summary", "")
    match = re.fullmatch(
        r"executions=(\d+) input=(\d+) requests=(\d+) candidates=(\d+) pages=(\d+) output=(\d+)",
        text,
    )
    if not match:
        raise RuntimeError(f"missing stage {stage} batch summary: {text!r}")
    keys = ("executions", "input", "requests", "candidates", "pages", "output")
    return dict(zip(keys, map(int, match.groups())))


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
    raise RuntimeError("fixed-N reader did not reach its cancellable provider wait")


def evidence(path: Path) -> dict:
    return {"path": str(path.relative_to(PACKAGE)), "sha256": sha(path)}


def main() -> None:
    golden = read(GOLDEN)
    fixture = golden["fixture"]
    limits = golden["limits"]
    out = run_dir("check-G1-040")
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
        commands.append(run_logged(make + ["clean"], out, "module-clean", timeout=180))
        commands.append(run_logged(make + ["-j4"], out, "module-build", timeout=180))
        commands.append(run_logged(make + ["install"], out, "module-install", timeout=180))
        commands.append(run_logged([sys.executable, lab, "start", "--variant=candidate"],
                                   out, "start", timeout=180))
        started = True
        commands.append(run_logged(make + ["installcheck"], out, "native-regression",
                                   env=db_env("candidate"), timeout=180))
        shutil.copyfile(MODULE / "results/graph_fixed.out", out / "native-results.out")

        writer = Session("writer")
        sessions.append(writer)
        edge_values = ",".join(f"({eid},{src},{dst})"
                               for eid, src, dst in fixture["edges"])
        writer.execute(
            "SET client_min_messages=warning; DROP EXTENSION IF EXISTS test_graph_core CASCADE; "
            "CREATE EXTENSION test_graph_core; DROP SCHEMA IF EXISTS g1040 CASCADE; CREATE SCHEMA g1040; "
            "CREATE TABLE g1040.edge(id bigint PRIMARY KEY,src bigint NOT NULL,dst bigint NOT NULL,payload text) "
            "WITH(fillfactor=40); CREATE INDEX edge_out ON g1040.edge(src); "
            "CREATE INDEX edge_in ON g1040.edge(dst); "
            f"INSERT INTO g1040.edge(id,src,dst) VALUES {edge_values}; "
            "INSERT INTO g1040.edge(id,src,dst) SELECT 1000+i,99+i,100+i FROM generate_series(1,9) i; "
            "UPDATE g1040.edge e SET payload=(SELECT string_agg(md5((e.id*1000+g)::text),'' ORDER BY g) "
            f"FROM generate_series(1,{fixture['payload_md5_fragments']}) g); "
            "ALTER TABLE g1040.edge ALTER COLUMN payload SET NOT NULL; "
            "CREATE TABLE g1040.request(anchor bigint NOT NULL,vertices bigint[],edges bigint[],payloads text[]); "
            "INSERT INTO g1040.request(anchor) VALUES " +
            ",".join(f"({anchor})" for anchor in fixture["anchors"]) + "; ANALYZE g1040.edge;"
        )

        reader, observer = Session("reader"), Session("observer")
        sessions.extend((reader, observer))
        reader.value(config_sql(
            len(fixture["directions"]), fixture["directions"], pause_ms=0,
            anchor_batch=limits["anchor_batch_size"],
            parent_batch=limits["max_parent_batch"],
            max_bindings=limits["max_path_bindings"],
        ))
        expected = enumerate_fixed(fixture["edges"], fixture["anchors"],
                                   fixture["directions"],
                                   fixture["payload_md5_fragments"])
        actual = json.loads(reader.value(actual_sql()))
        if bag(actual) != bag(expected):
            raise RuntimeError("fixed-N ALL_BINDINGS bag differs from independent enumeration")
        if not actual or any(len(row[3]) != len(fixture["directions"]) for row in actual):
            raise RuntimeError("copied payload array did not survive every stage")
        results["all_bindings"] = {"expected": expected, "actual": actual}

        plan_doc = explain_json(reader, analyze=True)
        write(out / "selected-plan.json", plan_doc)
        custom = find_custom(plan_doc[0]["Plan"])
        if not custom:
            raise RuntimeError("GraphExpandFixed was not selected")
        if custom.get("Graph Stages") != len(fixture["directions"]) or \
                custom.get("Graph Stage Model") != "generic fixed-N loop" or \
                custom.get("Graph Batch Pipeline") != "endpoint + candidate/heap-page":
            raise RuntimeError("selected plan does not expose the generic batched stage model")
        summaries = [stage_summary(custom, i + 1)
                     for i in range(len(fixture["directions"]))]
        if custom.get("Output Bindings") != len(expected):
            raise RuntimeError("plan output instrumentation differs from the oracle bag")
        if not all(item["candidates"] > 0 and item["pages"] > 0 for item in summaries):
            raise RuntimeError("fixed executor bypassed candidate/page batching")
        if summaries[0]["input"] <= summaries[0]["requests"] or \
                summaries[1]["input"] <= summaries[1]["requests"]:
            raise RuntimeError("frontier bindings were treated as scalar index probes")
        results["selected_plan"] = {"custom": custom, "summaries": summaries}

        # Nine hops are a probe beyond both the original 2-4 and proposed 2-8 test envelopes.
        writer.execute("TRUNCATE g1040.request; INSERT INTO g1040.request(anchor) VALUES (100);")
        probe_hops = fixture["generic_probe_hops"]
        reader.value(config_sql(
            probe_hops, [1] * probe_hops, pause_ms=0,
            anchor_batch=limits["anchor_batch_size"],
            parent_batch=limits["max_parent_batch"],
            max_bindings=limits["max_path_bindings"],
        ))
        probe_actual = json.loads(reader.value(actual_sql()))
        probe_edges = [[1000 + i, 99 + i, 100 + i]
                       for i in range(1, probe_hops + 1)]
        probe_expected = enumerate_fixed(probe_edges, [100], [1] * probe_hops,
                                         fixture["payload_md5_fragments"])
        if bag(probe_actual) != bag(probe_expected):
            raise RuntimeError("generic nine-hop probe failed")
        probe_plan = explain_json(reader, analyze=False)
        probe_custom = find_custom(probe_plan[0]["Plan"])
        if not probe_custom or probe_custom.get("Graph Stages") != probe_hops:
            raise RuntimeError("nine-hop plan was capped or fell back")
        results["generic_nine_hop"] = {"expected": probe_expected,
                                        "actual": probe_actual, "plan": probe_plan}

        # Cancel while the provider is live, then prove exact cleanup and backend/index reuse.
        writer.execute("TRUNCATE g1040.request; INSERT INTO g1040.request(anchor) VALUES (1);")
        reader.value(config_sql(
            len(fixture["directions"]), fixture["directions"],
            pause_ms=limits["cancel_pause_ms"],
            anchor_batch=limits["anchor_batch_size"],
            parent_batch=limits["max_parent_batch"],
            max_bindings=limits["max_path_bindings"],
        ))
        cleanup_before = cleanup_state(reader)
        pending = reader.send(
            "SELECT coalesce(sum(anchor+cardinality(vertices)+cardinality(edges)+cardinality(payloads)),0) "
            "FROM g1040.request;"
        )
        wait_paused(observer, reader.pid)
        if observer.value(f"SELECT pg_cancel_backend({reader.pid});") != "t":
            raise RuntimeError("failed to cancel the live fixed-N executor")
        reader.finish(pending, golden["cancel_sqlstate"])
        cleanup_after = cleanup_state(reader)
        if cleanup_after["active"] != 0 or \
                cleanup_after["end_calls"] != cleanup_before["end_calls"] + 1:
            raise RuntimeError("cancel left a provider scan active or ended it more than once")
        if int(reader.value("SELECT pg_backend_pid();")) != reader.pid:
            raise RuntimeError("cancel destroyed the reader backend")
        writer.execute("VACUUM g1040.edge; REINDEX INDEX g1040.edge_out; REINDEX INDEX g1040.edge_in;")
        reader.value(config_sql(
            len(fixture["directions"]), fixture["directions"], pause_ms=0,
            anchor_batch=limits["anchor_batch_size"],
            parent_batch=limits["max_parent_batch"],
            max_bindings=limits["max_path_bindings"],
        ))
        reusable = json.loads(reader.value(actual_sql()))
        reusable_expected = enumerate_fixed(fixture["edges"], [1],
                                            fixture["directions"],
                                            fixture["payload_md5_fragments"])
        if bag(reusable) != bag(reusable_expected):
            raise RuntimeError("backend or index was not reusable after cancel cleanup")
        results["cancel_cleanup"] = {"before": cleanup_before, "after": cleanup_after,
                                      "reusable_rows": len(reusable)}

        source_manifest = {}
        for path in (SOURCE / "src/backend/executor/graph_fixed.c",
                     SOURCE / "src/include/executor/graph_fixed.h",
                     MODULE / "test_graph_fixed.c", MODULE / "sql/graph_fixed.sql",
                     MODULE / "expected/graph_fixed.out"):
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
        files = (GOLDEN, out / "results.json", out / "source-manifest.json",
                 out / "writer.json", out / "reader.json", out / "observer.json",
                 out / "selected-plan.json", out / "native-results.out",
                 out / "native-regression.log")
        plan_evidence = evidence(out / "selected-plan.json")
        print(json.dumps({
            "task_id": TASK_ID,
            "status": "PASS",
            "assertions_passed": [
                "all_bindings", "datum_lifetime", "cancel_cleanup",
                "selected_graph_node", "batch_pipeline_not_scalar_wrapper",
                "frontier_bindings_requests_separate",
            ],
            "selected_feature_plan_evidence": plan_evidence,
            "evidence_files": [evidence(path) for path in files],
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
