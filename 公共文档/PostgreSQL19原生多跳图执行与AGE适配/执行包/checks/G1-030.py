#!/usr/bin/env python3
"""Exercise the real non-covering B-tree provider against independent SQL."""
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

TASK_ID = "G1-030"
SOURCE = WORKSPACE / ".worktrees/pg19-graph-core/postgres19"
INSTALL = WORKSPACE / ".install/pg19-graph-core/candidate"
MODULE = SOURCE / "src/test/modules/test_graph_core"
GOLDEN = PACKAGE / "golden/G1-030/contracts.json"


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
        self.execute("SET statement_timeout='20s'; SET enable_indexscan=off; SET enable_bitmapscan=off;")
        self.pid = int(self.value("SELECT pg_backend_pid();"))

    def send(self, sql: str) -> tuple[str, str]:
        self.calls += 1
        token = f"__G1030_{self.name}_{self.calls}__"
        assert self.process.stdin
        self.process.stdin.write(sql + f"\n\\echo {token}\n")
        self.process.stdin.flush()
        return token, sql

    def finish(self, pending: tuple[str, str], error: str | None = None) -> list[str]:
        token, sql = pending
        rows = []
        assert self.process.stdout
        for line in self.process.stdout:
            line = line.rstrip("\n")
            if line == token:
                self.transcript.append({"sql": sql, "output": rows})
                errors = [row for row in rows if row.startswith("ERROR:")]
                if error:
                    if not any(error in row for row in errors):
                        raise RuntimeError(f"{self.name}: expected {error}, got {rows}")
                elif errors:
                    raise RuntimeError(f"{self.name}: unexpected SQL error: {rows}")
                return rows
            rows.append(line)
        raise RuntimeError(self.name + ": psql exited before the completion marker")

    def execute(self, sql: str, error: str | None = None) -> list[str]:
        return self.finish(self.send(sql), error)

    def value(self, sql: str) -> str:
        rows = [r for r in self.execute(sql) if r and not r.startswith("NOTICE:")]
        if len(rows) != 1:
            raise RuntimeError(f"{self.name}: expected one value: {rows}")
        return rows[0]

    def close(self) -> None:
        if self.process.poll() is None:
            assert self.process.stdin
            self.process.stdin.write("\\q\n")
            self.process.stdin.flush()
        self.process.wait(timeout=10)


def call(keys: list[int], direction: int = 1, capacity: int = 2, mode: int = 0,
         pause: int = 0, byte_limit: int = 0, count_limit: int = 0,
         table: str = "edge", index: str | None = None, rescan: bool = False) -> str:
    index = index or ("edge_out" if direction == 1 else "edge_in")
    array = "ARRAY[" + ",".join(map(str, keys)) + "]::bigint[]"
    rescan_index = "'g1030.edge_in'::regclass::oid" if rescan else "0::oid"
    return (
        f"SELECT test_graph_btree_provider('g1030.{table}','g1030.{index}',{direction},"
        f"{array},{capacity},{mode},{pause},{byte_limit},{count_limit},{rescan_index});"
    )


def compare(session: Session, result: dict, keys: list[int], direction: int = 1,
            phase: int = 0) -> list[int]:
    if not result.get("accepted") or result.get("snapshot_borrowed") is not True:
        raise RuntimeError("provider rejected a supported shape or registered its own snapshot")
    array = "ARRAY[" + ",".join(map(str, keys)) + "]::bigint[]"
    endpoint, opposite = ("src", "dst") if direction == 1 else ("dst", "src")
    oracle = json.loads(session.value(
        "SELECT coalesce(json_agg(json_build_array(k.ord,e.id,e."
        f"{opposite},e.ctid::text) ORDER BY k.ord,e.id),'[]'::json) "
        f"FROM unnest({array}) WITH ORDINALITY k(key,ord) JOIN g1030.edge e ON e.{endpoint}=k.key;"
    ))
    actual = [
        [r["request"], r["id"], r["opposite"], r["visible_tid"]]
        for r in result["rows"] if r["phase"] == phase and r["visible"]
    ]
    if Counter(map(tuple, actual)) != Counter(map(tuple, oracle)):
        raise RuntimeError(f"complete visible candidate multiset differs: {actual} != {oracle}")
    raw = [r for r in result["rows"] if r["phase"] == phase]
    if [r["ordinal"] for r in raw] != list(range(len(raw))):
        raise RuntimeError("pause/resume repeated or skipped a candidate ordinal")
    if any(r["opposite_available"] for r in result["rows"]):
        raise RuntimeError("non-covering B-tree claimed to have the opposite key")
    if sum(result["batches"]) != result["raw_count"]:
        raise RuntimeError("batch accounting does not match physical candidates")
    return sorted(r["id"] for r in result["rows"] if r["phase"] == phase and r["visible"])


def wait_paused(observer: Session, pid: int) -> None:
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if observer.value(
            f"SELECT coalesce((SELECT wait_event_type='Extension' AND state='active' "
            f"FROM pg_stat_activity WHERE pid={pid}),false);"
        ) == "t":
            return
        time.sleep(0.02)
    raise RuntimeError("reader did not pause after its first real candidate batch")


def evidence(path: Path) -> dict:
    return {"path": str(path.relative_to(PACKAGE)), "sha256": sha(path)}


def main() -> None:
    out = run_dir("check-G1-030")
    golden = read(GOLDEN)
    lab = PACKAGE / "scripts/lab.py"
    commands = []
    sessions = []
    results = {}
    started = False
    try:
        commands.append(run_logged([sys.executable, lab, "stop", "--variant=candidate"], out, "pre-stop", timeout=120))
        commands.append(run_logged([sys.executable, lab, "build", "--variant=candidate", "--jobs=4"], out, "build", timeout=1800))
        make = ["make", "-C", MODULE, "USE_PGXS=1", f"PG_CONFIG={INSTALL / 'bin/pg_config'}"]
        for label, args in (("clean", ["clean"]), ("build", ["-j4"]), ("install", ["install"])):
            commands.append(run_logged(make + args, out, "module-" + label, timeout=120))
        commands.append(run_logged([sys.executable, lab, "start", "--variant=candidate"], out, "start", timeout=180))
        started = True
        commands.append(run_logged(make + ["installcheck"], out, "native-regression",
                                   env=db_env("candidate"), timeout=120))
        shutil.copyfile(MODULE / "results/graph_btree.out", out / "native-results.out")
        writer, reader, observer = (Session(n) for n in ("writer", "reader", "observer"))
        sessions.extend((writer, reader, observer))
        writer.execute(
            "SET client_min_messages=warning; DROP EXTENSION IF EXISTS test_graph_core CASCADE; "
            "CREATE EXTENSION test_graph_core; DROP SCHEMA IF EXISTS g1030 CASCADE; CREATE SCHEMA g1030; "
            "CREATE TABLE g1030.edge(id bigint PRIMARY KEY,src bigint NOT NULL,dst bigint NOT NULL,payload text) WITH(fillfactor=40); "
            "CREATE INDEX edge_out ON g1030.edge(src); CREATE INDEX edge_in ON g1030.edge(dst); "
            "CREATE INDEX edge_partial ON g1030.edge(src) WHERE id>0; "
            "CREATE INDEX edge_expr ON g1030.edge((src+0)); "
            "CREATE INDEX edge_multi ON g1030.edge(src,id); "
            "CREATE INDEX edge_cover ON g1030.edge(src) INCLUDE(dst); "
            "INSERT INTO g1030.edge VALUES " + ",".join(f"({i},{s},{d},'before')" for i, s, d in golden["fixture"]["edges"]) + "; "
            "INSERT INTO g1030.edge SELECT 1000+i,50,100+i%13,'bulk' FROM generate_series(1,1800) i; "
            "ANALYZE g1030.edge;"
        )
        narrow = json.loads(writer.value(
            "SELECT json_agg(json_build_array(indnatts,indnkeyatts)) FROM pg_index "
            "WHERE indexrelid IN ('g1030.edge_out'::regclass,'g1030.edge_in'::regclass);"
        ))
        if narrow != [[1, 1], [1, 1]]:
            raise RuntimeError("baseline index unexpectedly contains covering columns")
        results["narrow_indexes"] = narrow
        reader.execute("BEGIN ISOLATION LEVEL REPEATABLE READ;")
        for name, keys, direction, capacity in (
            ("out", [10], 1, 2), ("in", [20], 2, 1),
            ("empty", [999], 1, 3), ("bindings", [10, 10, 999, 20], 1, 3),
            ("multi_leaf", [50], 1, 127), ("large_batch", [50, 10], 1, 1024),
        ):
            result = json.loads(reader.value(call(keys, direction, capacity)))
            ids = compare(reader, result, keys, direction)
            if name == "out" and ids != golden["out_10"] or name == "in" and ids != golden["in_20"]:
                raise RuntimeError("manual small-graph oracle differs")
            if name == "multi_leaf" and (len(ids) != 1800 or len(result["batches"]) < 10):
                raise RuntimeError("multi-page endpoint was not resumed across bounded batches")
            results[name] = result
        reader.execute("COMMIT;")

        # A payload-only HOT update must keep the index root, not visible ctid.
        writer.execute("UPDATE g1030.edge SET payload='hot' WHERE id=101;")
        reader.execute("BEGIN ISOLATION LEVEL REPEATABLE READ;")
        hot = json.loads(reader.value(call([10])))
        compare(reader, hot, [10])
        row = next(r for r in hot["rows"] if r.get("id") == golden["hot_id"])
        if row["root_tid"] == row["visible_tid"]:
            raise RuntimeError("HOT fixture did not prove distinct root and visible TIDs")
        results["hot"] = hot
        results["hot_update_counter"] = writer.value("SELECT n_tup_hot_upd FROM pg_stat_all_tables WHERE relid='g1030.edge'::regclass;")

        # Old RR snapshot survives non-HOT endpoint move, delete, insert and VACUUM.
        writer.execute(
            "UPDATE g1030.edge SET src=90 WHERE id=102; DELETE FROM g1030.edge WHERE id=103; "
            "INSERT INTO g1030.edge VALUES(109,10,99,'new'); VACUUM g1030.edge;"
        )
        old = json.loads(reader.value(call([10], capacity=1)))
        if compare(reader, old, [10]) != golden["old_snapshot_out_10"]:
            raise RuntimeError("old snapshot lost an index candidate")
        results["old_snapshot"] = old
        reader.execute("COMMIT; BEGIN ISOLATION LEVEL REPEATABLE READ;")
        current = json.loads(reader.value(call([10], capacity=2)))
        if compare(reader, current, [10]) != golden["current_out_10"]:
            raise RuntimeError("new snapshot visible rows differ")
        results["current"] = current
        for name, args, keys in (
            ("request_copy", dict(mode=2), [10]),
            ("byte_shrink", dict(capacity=8, byte_limit=1), [50]),
            ("count_shrink", dict(capacity=8, count_limit=2), [50]),
            ("cover_optional", dict(index="edge_cover"), [10]),
            ("multi_optional", dict(index="edge_multi"), [10]),
        ):
            result = json.loads(reader.value(call(keys, **args)))
            compare(reader, result, keys)
            if name == "byte_shrink" and max(result["batches"]) != 1:
                raise RuntimeError("candidate-byte budget did not shrink batches")
            if name == "count_shrink" and max(result["batches"]) != 2:
                raise RuntimeError("candidate-count budget did not shrink batches")
            results[name] = result
        rescan = json.loads(reader.value(call([10], capacity=1, mode=1, rescan=True)))
        compare(reader, rescan, [20], 2, phase=1)
        if len([r for r in rescan["rows"] if r["phase"] == 0]) != 1:
            raise RuntimeError("rescan was not issued while the old cursor was paused")
        results["rescan"] = rescan
        reader.execute("COMMIT;")

        # Concurrent writes while a real cursor is paused cannot change its RR view.
        reader.execute("BEGIN ISOLATION LEVEL REPEATABLE READ;")
        before = json.loads(reader.value(call([50], capacity=200)))
        pending = reader.send(call([50], capacity=7, pause=1200))
        wait_paused(observer, reader.pid)
        writer.execute("UPDATE g1030.edge SET src=91 WHERE id=1001; DELETE FROM g1030.edge WHERE id=1002; INSERT INTO g1030.edge VALUES(9999,50,444,'mid-scan');")
        concurrent = json.loads(reader.finish(pending)[0])
        if compare(reader, concurrent, [50]) != compare(reader, before, [50]):
            raise RuntimeError("mid-scan writer changed the same snapshot's visible candidates")
        results["concurrent_pause"] = concurrent
        reader.execute("COMMIT;")

        writer.execute("BEGIN; INSERT INTO g1030.edge VALUES(110,10,77,'own');")
        own = json.loads(writer.value(call([10])))
        if 110 not in compare(writer, own, [10]):
            raise RuntimeError("own prior-command write is missing")
        results["own_command"] = own
        writer.execute("ROLLBACK;")

        for mode, expected in golden["rejection_reasons"].items():
            probe = json.loads(reader.value(call([10, 20], mode=int(mode))))
            if probe != {"accepted": False, "reason": expected}:
                raise RuntimeError(f"validation mode {mode} did not fail closed: {probe}")
            results["reject_" + mode] = probe
        for index in ("edge_partial", "edge_expr", "edge_in"):
            probe = json.loads(reader.value(call([10], index=index)))
            if probe != {"accepted": False, "reason": 3}:
                raise RuntimeError("unsupported index shape accepted: " + index)
            results["reject_" + index] = probe
        writer.execute("ALTER TABLE g1030.edge ENABLE ROW LEVEL SECURITY;")
        rls = json.loads(reader.value(call([10])))
        if rls != {"accepted": False, "reason": 4}:
            raise RuntimeError("RLS relation accepted without a frontend proof")
        results["reject_rls"] = rls
        writer.execute("ALTER TABLE g1030.edge DISABLE ROW LEVEL SECURITY;")
        writer.execute(
            "CREATE TABLE g1030.wrong_type(id bigint,src bigint NOT NULL,dst integer NOT NULL); "
            "CREATE INDEX wrong_type_out ON g1030.wrong_type(src); "
            "CREATE TABLE g1030.nullable(id bigint,src bigint NOT NULL,dst bigint); "
            "CREATE INDEX nullable_out ON g1030.nullable(src); "
            "CREATE INDEX edge_hash ON g1030.edge USING hash(src);"
        )
        for table in ("wrong_type", "nullable"):
            probe = json.loads(reader.value(call([10], table=table, index=table + "_out")))
            if probe != {"accepted": False, "reason": 2}:
                raise RuntimeError("unsupported endpoint type/nullability accepted: " + table)
            results["reject_" + table] = probe
        probe = json.loads(reader.value(call([10], index="edge_hash")))
        if probe != {"accepted": False, "reason": 3}:
            raise RuntimeError("a non-B-tree AM was accepted")
        results["reject_hash"] = probe
        # Provider registration and a graph catalog do not confer heap SELECT.
        writer.execute("DROP ROLE IF EXISTS g1030_untrusted; CREATE ROLE g1030_untrusted; GRANT USAGE ON SCHEMA g1030 TO g1030_untrusted;")
        reader.execute("SET ROLE g1030_untrusted;")
        no_permission = json.loads(reader.value(call([10])))
        if no_permission != {"accepted": False, "reason": 4}:
            raise RuntimeError("provider bypassed SELECT permissions")
        results["reject_acl"] = no_permission
        writer.execute("GRANT SELECT ON g1030.edge TO g1030_untrusted;")
        permitted = json.loads(reader.value(call([10])))
        compare(reader, permitted, [10])
        results["permitted_role"] = permitted
        reader.execute("RESET ROLE;")
        writer.execute("REVOKE SELECT ON g1030.edge FROM g1030_untrusted; REVOKE USAGE ON SCHEMA g1030 FROM g1030_untrusted; DROP ROLE g1030_untrusted;")
        reader.execute("SELECT test_graph_btree_provider('g1030.edge','g1030.edge_out',1,ARRAY[NULL]::bigint[],1);", "22004")

        # Errors/cancel happen with a live AM cursor; standard query cleanup must work.
        for mode, sqlstate in golden["error_sqlstates"].items():
            reader.execute(call([50], mode=int(mode)), sqlstate)
            repeat = json.loads(reader.value(call([10])))
            compare(reader, repeat, [10])
        pending = reader.send(call([50], pause=10000))
        wait_paused(observer, reader.pid)
        if observer.value(f"SELECT pg_cancel_backend({reader.pid});") != "t":
            raise RuntimeError("failed to cancel a paused provider")
        reader.finish(pending, "57014")
        writer.execute("VACUUM g1030.edge; REINDEX INDEX g1030.edge_out;")
        after_cleanup = json.loads(reader.value(call([10])))
        compare(reader, after_cleanup, [10])
        results["after_cancel_vacuum_reindex"] = after_cleanup

        # Real SSI uses the public B-tree access path's predicate locks.
        reader.execute("BEGIN ISOLATION LEVEL SERIALIZABLE;")
        ssi_a = json.loads(reader.value(call([10])))
        writer.execute("BEGIN ISOLATION LEVEL SERIALIZABLE;")
        ssi_b = json.loads(writer.value(call([40])))
        results["ssi_locks"] = json.loads(observer.value(
            f"SELECT coalesce(json_agg(json_build_array(pid,locktype,mode)),'[]'::json) FROM pg_locks WHERE pid IN ({reader.pid},{writer.pid}) AND mode='SIReadLock';"
        ))
        if not results["ssi_locks"]:
            raise RuntimeError("public B-tree path acquired no SSI predicate lock")
        reader.execute("INSERT INTO g1030.edge VALUES(120,40,123,'ssi_a');")
        writer.execute("INSERT INTO g1030.edge VALUES(121,10,124,'ssi_b');")
        reader.execute("COMMIT;")
        writer.execute("COMMIT;", "40001")
        results["ssi_reads"] = [ssi_a, ssi_b]

        for session in sessions:
            session.close()
            write(out / (session.name + ".json"), session.transcript)
            if any("WARNING:" in row for entry in session.transcript for row in entry["output"]):
                raise RuntimeError("resource cleanup emitted a warning")
        sessions.clear()
        commands.append(run_logged([sys.executable, lab, "stop", "--variant=candidate"], out, "post-stop", timeout=120))
        started = False
        results["finished"] = now()
        results["postgres_sha256"] = sha(INSTALL / "bin/postgres")
        results["module_sha256"] = sha(INSTALL / "lib/postgresql/test_graph_core.so")
        results["golden_sha256"] = sha(GOLDEN)
        write(out / "results.json", results)
        write(out / "summary.json", {"status": "PASS", "task_id": TASK_ID, "finished": now(),
                                     "results_sha256": sha(out / "results.json"), "commands": commands})
        print(json.dumps({
            "task_id": TASK_ID, "status": "PASS",
            "assertions_passed": ["root_tid_hot", "complete_snapshot_candidates", "out_in_rescan",
                                  "no_covering_requirement", "bounded_batch_pause_resume",
                                  "opposite_key_from_tableam_if_absent"],
            "evidence_files": [evidence(p) for p in (GOLDEN, out / "results.json", out / "writer.json",
                                                       out / "reader.json", out / "observer.json",
                                                       out / "native-results.out", out / "native-regression.log")],
        }, ensure_ascii=False, sort_keys=True))
    finally:
        for session in sessions:
            if session.process.poll() is None:
                session.process.terminate()
            session.process.wait(timeout=10)
            write(out / (session.name + ".json"), session.transcript)
        if started:
            run_logged([sys.executable, lab, "stop", "--variant=candidate"], out, "failure-stop", timeout=120)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
