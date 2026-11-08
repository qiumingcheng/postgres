#!/usr/bin/env python3
"""Deterministic graph vertex VM/VACUUM race, against an already running server.

Requires Linux, GDB, psycopg2, matching pg_config/server debug symbols and a
superuser DSN. Starts no server and changes no server source. It creates/drops
an isolated schema and compiles a temporary helper in --output. Run both frozen
old and fixed servers with --expect old/fixed; old must reproduce wrong rows.
GDB only observes and stops the reader at the real VM/heap boundary. The writer
uses ordinary VACUUM, with no page editing, fake snapshot or forced xmin.

Example:
  python3 vertex_vm_race.py --dsn 'port=55439 dbname=postgres' \
    --pg-config /matching/install/bin/pg_config --expect fixed --output /evidence/r1
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import time
import uuid

import psycopg2
from psycopg2 import sql

HERE = Path(__file__).resolve().parent


def query(conn, command, params=None):
    with conn.cursor() as cursor:
        cursor.execute(command, params)
        return cursor.fetchall() if cursor.description else []


def write(path, value):
    path.write_text(json.dumps(value, indent=2, default=str) + "\n")


def wait_file(path, debugger, future=None):
    deadline = time.monotonic() + 30
    while not path.exists():
        error = path.parent / "gdb_error.json"
        if error.exists():
            raise AssertionError(error.read_text())
        if debugger.poll() is not None:
            raise AssertionError("debugger exited: " + (path.parent / "gdb.log").read_text())
        if future and future.done():
            raise AssertionError("reader completed without pause: " + repr(future.result()))
        if time.monotonic() > deadline:
            raise AssertionError("timed out waiting for " + str(path))
        time.sleep(.02)
    return json.loads(path.read_text())


def plan_nodes(value):
    if isinstance(value, dict):
        yield value
        for child in value.values():
            yield from plan_nodes(child)
    elif isinstance(value, list):
        for child in value:
            yield from plan_nodes(child)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dsn", required=True)
    parser.add_argument("--pg-config", type=Path, required=True)
    parser.add_argument("--expect", choices=("old", "fixed"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    report = dict(status="RUNNING", expected=args.expect, cases=[])
    write(output / "summary.json", report)

    def config(option):
        return subprocess.check_output([str(args.pg_config), option], text=True).strip()

    module = output / "vertex_vm_race_helper.so"
    compile_command = ["cc", *shlex.split(config("--cflags")), "-fPIC", "-shared",
                       "-Werror=implicit-function-declaration", "-I" + config("--includedir-server"),
                       str(HERE / "vertex_vm_race_helper.c"), "-o", str(module)]
    result = subprocess.run(compile_command, capture_output=True, text=True)
    (output / "compile.log").write_text(result.stdout + result.stderr)
    result.check_returncode()
    report["compile_command"] = compile_command
    report["inputs"] = {name: hashlib.sha256((HERE / name).read_bytes()).hexdigest()
                        for name in ("vertex_vm_race.py", "vertex_vm_race_gdb.py", "vertex_vm_race_helper.c")}
    report["configure"] = config("--configure")

    def connect():
        conn = psycopg2.connect(args.dsn)
        conn.autocommit = True
        query(conn, "SET statement_timeout='60s'; SET max_parallel_maintenance_workers=0; SET jit=off")
        for name, signature, returns in (("allow_debugger", "integer", "void"),
                                         ("status", "regclass,bigint", "boolean")):
            query(conn, sql.SQL("CREATE FUNCTION pg_temp.vm_" + name + "(" + signature + ") RETURNS " +
                               returns + " AS {},{} LANGUAGE C STRICT").format(
                                   sql.Literal(str(module)), sql.Literal("graph_vertex_vm_" + name)))
        return conn

    admin = connect()
    report["server"] = query(admin, "SELECT version(),pg_postmaster_start_time()")
    # First three hit the formerly unsafe scalar branch. The two full batches
    # already held pins before the fix. A filtered stage must visit the heap.
    cases = [("singleton", 1, False, False, 1, True),
             ("array_tail", 33, False, False, 33, True),
             ("range_tail", 33, True, False, 33, True),
             ("array_head", 33, False, False, 1, False),
             ("range_head", 33, True, False, 1, False),
             ("filtered_heap", 1, False, True, 1, False)]
    try:
        for name, count, ranges, filtered, stop_after, formerly_unsafe in cases:
            directory = output / name
            directory.mkdir()
            schema = "vertex_vm_" + uuid.uuid4().hex[:12]
            reader = connect()
            writer = connect()
            debugger = None
            pool = ThreadPoolExecutor(max_workers=2)
            try:
                query(admin, sql.SQL("CREATE SCHEMA {}").format(sql.Identifier(schema)))
                for conn in (admin, reader, writer):
                    query(conn, sql.SQL("SET search_path={},public").format(sql.Identifier(schema)))
                query(admin, "CREATE TABLE v(id bigint PRIMARY KEY,age int,pad text STORAGE PLAIN DEFAULT repeat('x',3000)) WITH (autovacuum_enabled=false)")
                query(admin, "CREATE TABLE e(id bigint PRIMARY KEY,src bigint NOT NULL,dst bigint NOT NULL) WITH (autovacuum_enabled=false)")
                query(admin, "INSERT INTO v(id,age) VALUES(1,20),(-1,20)")
                query(admin, "INSERT INTO v(id,age) SELECT i,20 FROM generate_series(2,%s) i UNION ALL SELECT 100+i,20 FROM generate_series(2,%s) i", (count+1, count+1))
                query(admin, "INSERT INTO e SELECT i,1,i FROM generate_series(2,%s) i UNION ALL SELECT 100+i,i,100+i FROM generate_series(2,%s) i", (count+1, count+1))
                query(admin, "CREATE INDEX e_src ON e(src) INCLUDE(dst,id)")
                query(admin, "CREATE PROPERTY GRAPH g VERTEX TABLES(v KEY(id)) EDGE TABLES(e KEY(id) SOURCE KEY(src) REFERENCES v(id) DESTINATION KEY(dst) REFERENCES v(id))")
                query(admin, "VACUUM (ANALYZE) v")
                query(admin, "VACUUM (ANALYZE) e")
                for conn in (reader,):
                    query(conn, "LOAD 'test_graph_core'")
                    # Bitmap anchor releases index pins and fetches only its own
                    # heap page; seqscan read-ahead can pin the target page.
                    query(conn, "SET enable_seqscan=off; SET enable_indexscan=off; SET enable_indexonlyscan=off")
                    query(conn, "SET test_graph_core.vertex_batch_size=32; SET test_graph_core.endpoint_batch_size=8")
                    query(conn, "SET test_graph_core.vertex_range_probe=" + ("on" if ranges else "off"))
                target = count+1 if stop_after == 33 else 2
                statement = "/*+ GraphRoute(btree) */ SELECT DISTINCT endpoint FROM GRAPH_TABLE(g MATCH (a IS v WHERE a.id=1::bigint)-[x IS e]->(b IS v" + (" WHERE b.age>=0" if filtered else "") + ")-[y IS e]->(c IS v) COLUMNS(c.id AS endpoint)) gt ORDER BY endpoint"
                plan = query(reader, "EXPLAIN (FORMAT JSON) " + statement)[0][0]
                fixed = [node for node in plan_nodes(plan) if node.get("Custom Plan Provider") == "GraphExpandFixed"]
                assert len(fixed) == 1, plan
                if not filtered:
                    assert "endpoint sets" in fixed[0].get("Graph Prefix Representation", ""), plan
                query(admin, "DELETE FROM v WHERE id=%s", (target,))
                relation = query(admin, "SELECT 'v'::regclass::oid")[0][0]
                log = open(directory / "gdb.log", "w")
                debugger = subprocess.Popen(["gdb", "--quiet", "--nx", "--batch", "-x", str(HERE / "vertex_vm_race_gdb.py")],
                                            stdout=log, stderr=subprocess.STDOUT,
                                            env={**os.environ, "GGC_VM_CONTROL": str(directory)})
                query(reader, "SELECT pg_temp.vm_allow_debugger(%s)", (debugger.pid,))
                write(directory / "attach.json", dict(pid=reader.get_backend_pid(), relation=relation,
                      symbol="index_fetch_heap" if filtered else "visibilitymap_get_status", stop_after=stop_after))
                wait_file(directory / "armed.json", debugger)
                read_future = pool.submit(query, reader, statement)
                pause = wait_file(directory / "paused.json", debugger, read_future)
                assert pause["number_of_keys"] == (2 if ranges and stop_after == 1 else 1), pause
                should_block = not filtered and (args.expect == "fixed" or not formerly_unsafe)
                assert pause["xs_want_itup"] == should_block, pause
                assert pause["drop_pin"] != should_block, pause
                assert (pause["leaf_buffer"] != 0) == should_block, pause
                vacuum_future = pool.submit(query, writer, "VACUUM (VERBOSE, INDEX_CLEANUP ON, DISABLE_PAGE_SKIPPING ON, TRUNCATE OFF) v")
                activity = []
                if should_block:
                    deadline = time.monotonic() + 15
                    while True:
                        activity = query(admin, "SELECT state,wait_event_type,wait_event FROM pg_stat_activity WHERE pid=%s", (writer.get_backend_pid(),))
                        if activity and activity[0][2] in ("BufferPin", "BufferCleanup"):
                            break
                        assert not vacuum_future.done(), ("VACUUM unexpectedly completed", vacuum_future.result())
                        assert time.monotonic() < deadline, activity
                        time.sleep(.02)
                else:
                    vacuum_future.result(timeout=15)
                vm = query(admin, "SELECT pg_temp.vm_status('v',%s)", (pause["heap_block"],))[0][0]
                write(directory / "vacuum_debug.json", dict(notices=writer.notices, activity=query(admin,"SELECT pid, state, backend_xmin FROM pg_stat_activity"), vm=vm))
                assert vm != should_block, (pause, vm, activity)
                checkpoint = dict(name=name, pause=pause, vacuum_blocked=should_block,
                                  vacuum_activity=activity, vm_while_paused=vm, plan=plan)
                write(directory / "before_resume.json", checkpoint)
                (directory / "release").touch()
                rows = read_future.result(timeout=15)
                vacuum_future.result(timeout=15)
                assert debugger.wait(timeout=15) == 0, (directory / "gdb.log").read_text()
                assert (directory / "done.json").exists()
                oracle = query(admin, "SELECT DISTINCT c.id FROM v a JOIN e x ON x.src=a.id JOIN v b ON b.id=x.dst JOIN e y ON y.src=b.id JOIN v c ON c.id=y.dst WHERE a.id=1 ORDER BY c.id")
                extra = [(100+target,)] if args.expect == "old" and formerly_unsafe else []
                assert rows == sorted(oracle + extra), (name, rows, oracle, extra)
                checkpoint.update(rows=rows, oracle=oracle, reproduced_wrong_result=bool(extra), status="PASS")
                report["cases"].append(checkpoint)
                write(output / "summary.json", report)
                print(name + ": PASS" + (" (old wrong result reproduced)" if extra else ""), flush=True)
            finally:
                (directory / "release").touch()
                if debugger:
                    try:
                        debugger.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        debugger.terminate()
                        debugger.wait(timeout=10)
                for conn in (reader, writer):
                    try:
                        conn.cancel()
                    except psycopg2.Error:
                        pass
                pool.shutdown(wait=True)
                reader.close()
                writer.close()
                query(admin, sql.SQL("DROP SCHEMA IF EXISTS {} CASCADE").format(sql.Identifier(schema)))
        report["status"] = "PASS"
    except BaseException as exc:
        report.update(status="FAIL", error=repr(exc))
        raise
    finally:
        admin.close()
        write(output / "summary.json", report)


if __name__ == "__main__":
    main()
