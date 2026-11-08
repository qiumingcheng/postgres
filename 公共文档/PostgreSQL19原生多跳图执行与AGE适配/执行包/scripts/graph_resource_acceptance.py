#!/usr/bin/env python3
"""Isolated T1/T2 resource, cancellation and real Rescan checks (not a benchmark)."""
import argparse
import json
from pathlib import Path
import shutil
import threading
import time
import traceback

import psycopg2

from common import PACKAGE, WORKSPACE, now, run_dir, run_logged, sha, write


def nodes(plan):
    yield plan
    for child in plan.get("Plans", []):
        yield from nodes(child)


def query(hops, condition="v0.id=1::bigint"):
    pattern = f"(v0 IS v WHERE {condition})"
    for i in range(1, hops + 1):
        pattern += f"-[e{i} IS e]->(v{i} IS v)"
    return ("SELECT DISTINCT endpoint FROM GRAPH_TABLE(g MATCH " + pattern +
            f" COLUMNS(v{hops}.id AS endpoint)) gt")


def parse_args():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--port", required=True, type=int)
    ap.add_argument("--binary", required=True, type=Path)
    ap.add_argument("--user", help="database role; defaults to libpq's current-user behavior")
    ap.add_argument("--prior-log", action="append", default=[], type=Path)
    return ap.parse_args()


def run_acceptance(args, out, result, owned):
    source = WORKSPACE / ".worktrees/pg19-graph-core/postgres19"
    test = source / "src/test/modules/test_graph_core/sql/graph_resources.sql"
    write(out / "identity.json", {
        "started": now(), "binary": str(args.binary), "binary_sha256": sha(args.binary),
        "sql": str(test), "sql_sha256": sha(test),
        "driver_sha256": sha(__file__), "port": args.port, "host": args.host,
        "user": args.user,
        "scope": "isolated correctness/resource acceptance; not performance; binary identity recorded above",
        "source_at_test": {str(p.relative_to(source)): sha(p) for p in [
            source / "src/backend/executor/graph_fixed.c",
            source / "src/include/executor/graph_fixed.h",
            source / "src/backend/executor/graph_pgq.c"]},
    })
    shutil.copy2(test, out / "graph_resources.sql")
    shutil.copy2(__file__, out / "graph_resource_acceptance.py")
    for log in args.prior_log:
        shutil.copy2(log, out / log.name)
    database = "resource_" + str(time.time_ns())
    connection = {"host": args.host, "port": args.port}
    if args.user:
        connection["user"] = args.user
    admin = owned["admin"] = psycopg2.connect(**connection, dbname="postgres")
    admin.autocommit = True
    with admin.cursor() as cur:
        cur.execute('CREATE DATABASE "' + database + '"')
    result["database"] = database
    command = [args.binary.parent / "psql", "-X", "-h", args.host, "-p", args.port,
               "-d", database, "-v", "ON_ERROR_STOP=1", "-f", test]
    if args.user:
        command += ["-U", args.user]
    run_logged(command, out, "resource-sql", timeout=120)
    conn = owned["connection"] = psycopg2.connect(**connection, dbname=database)
    conn.autocommit = True
    cur = owned["cursor"] = conn.cursor()
    statements = [
        "LOAD 'test_graph_core'", "SET jit=off", "SET max_parallel_workers_per_gather=0",
        "SET work_mem='4MB'", "SET statement_timeout='30s'",
        "CREATE SCHEMA life", "SET search_path=life,public",
        "CREATE TABLE v(id bigint PRIMARY KEY)",
        "CREATE TABLE e(id bigint PRIMARY KEY,src bigint NOT NULL,dst bigint NOT NULL)",
        "CREATE INDEX e_out ON e(src)",
        "INSERT INTO v SELECT i FROM generate_series(0,4099) i",
        "INSERT INTO e SELECT i,1,i+1 FROM generate_series(1,4096) i",
        "INSERT INTO e SELECT 4096+i,i+1,i+1 FROM generate_series(1,4096) i",
        "INSERT INTO e VALUES(90000,0,4098),(90001,0,4099)",
        "CREATE PROPERTY GRAPH g VERTEX TABLES(v KEY(id)) EDGE TABLES(e KEY(id) "
        "SOURCE KEY(src) REFERENCES v(id) DESTINATION KEY(dst) REFERENCES v(id))",
        "ANALYZE v", "ANALYZE e",
    ]
    owned["sql"]["setup"] = statements
    for sql in statements:
        cur.execute(sql)
    queries = {}
    owned["sql"]["queries"] = queries

    def explain(name, sql):
        queries[name] = sql
        cur.execute("EXPLAIN(ANALYZE,FORMAT JSON,TIMING OFF) " + sql)
        plan = cur.fetchone()[0]
        write(out / (name + "-plan.json"), plan)
        return plan

    plan = explain("wide-64", "/*+ GraphRoute(btree) */ " + query(64))
    graph = next(n for n in nodes(plan[0]["Plan"])
                 if n.get("Custom Plan Provider") == "GraphExpandFixed")
    assert graph.get("Graph Prefix Representation") == "exact-k WALK endpoint sets (unordered)"
    assert int(graph["Actual Rows"]) == 4096
    assert graph["Factorized Peak Bytes"] <= 4 * 1024 * 1024
    result["tests"]["wide-64"] = {
        "rows": graph["Actual Rows"], "peak_bytes": graph["Factorized Peak Bytes"],
        "expanded_edges": graph.get("Expanded Edges"), "passed": True}

    plan = explain("formal-multiple-anchors", "/*+ GraphRoute(btree) */ " +
                   query(2, "v0.id IN (0::bigint,1::bigint)"))
    graphs = [n for n in nodes(plan[0]["Plan"])
              if n.get("Custom Plan Provider") == "GraphExpandFixed"]
    result["tests"]["formal-multiple-anchors"] = {
        "rows": plan[0]["Plan"]["Actual Rows"],
        "endpoint_set": bool(graphs and graphs[0].get("Graph Prefix Representation") ==
                             "exact-k WALK endpoint sets (unordered)"),
    }
    assert int(plan[0]["Plan"]["Actual Rows"]) == 4096

    # Warm a parameter-free prepared plan before cancelling its second run.
    # Thus cancellation does not merely prove parser/planner cancellation.
    sql = "/*+ GraphRoute(btree) */ " + query(256)
    queries["cancel-prepared"] = sql
    cur.execute("PREPARE resource_cancel AS " + sql)
    cur.execute("EXECUTE resource_cancel")
    assert len(cur.fetchall()) == 4096
    pid = conn.get_backend_pid()
    cancelled = {}

    def execute_cancel():
        try:
            cur.execute("EXECUTE resource_cancel")
            cancelled["rows"] = len(cur.fetchall())
        except psycopg2.Error as error:
            cancelled["sqlstate"] = error.pgcode
            cancelled["message"] = str(error)

    worker = owned["worker"] = threading.Thread(target=execute_cancel, daemon=True)
    worker.start()
    active = False
    deadline = time.monotonic() + 5
    with admin.cursor() as control:
        while worker.is_alive() and time.monotonic() < deadline:
            control.execute("SELECT state,query FROM pg_stat_activity WHERE pid=%s", (pid,))
            state = control.fetchone()
            if state and state[0] == "active" and state[1] == "EXECUTE resource_cancel":
                active = True
                break
            time.sleep(.01)
        if active:
            time.sleep(.02)
            control.execute("SELECT pg_cancel_backend(%s)", (pid,))
            cancelled["signal_sent"] = control.fetchone()[0]
    worker.join(timeout=10)
    assert not worker.is_alive(), "cancelled backend did not return"
    assert cancelled.get("sqlstate") == "57014", cancelled
    cur.execute("EXECUTE resource_cancel")
    assert len(cur.fetchall()) == 4096
    cancelled["same_connection_recovery_rows"] = 4096
    result["tests"]["prepared-cancellation"] = cancelled
    cur.execute("DEALLOCATE resource_cancel")

    # Exercise endpoint-set rescan directly through the public fixed-runtime
    # test bridge, independently of the frontend's narrow DISTINCT admission.
    setup = [
        "CREATE TABLE requests(anchor bigint PRIMARY KEY)",
        "INSERT INTO requests VALUES(0),(1)",
        "CREATE FUNCTION pg_temp.cfg(regclass,regclass[],regclass[],integer[],integer,integer,integer,integer) "
        "RETURNS integer AS '$libdir/test_graph_core','test_graph_fixed_config' LANGUAGE C STRICT",
        "CREATE FUNCTION pg_temp.tune(integer,integer,integer,bigint,boolean,boolean) "
        "RETURNS boolean AS '$libdir/test_graph_core','test_graph_fixed_tuning' LANGUAGE C STRICT",
        "CREATE FUNCTION pg_temp.factor(bigint) RETURNS bigint AS '$libdir/test_graph_core',"
        "'test_graph_fixed_factorize' LANGUAGE C STRICT",
        "CREATE FUNCTION pg_temp.endpoint(boolean) RETURNS boolean AS '$libdir/test_graph_core',"
        "'test_graph_fixed_endpoint_set' LANGUAGE C STRICT",
        "SELECT pg_temp.cfg('requests',array_fill('e'::regclass,ARRAY[2]),"
        "array_fill('e_out'::regclass,ARRAY[2]),ARRAY[1,1],0,1,128,1000000)",
        "SELECT pg_temp.tune(128,128,4,1048576,true,false)",
        "SELECT pg_temp.factor(4194304)", "SELECT pg_temp.endpoint(true)",
        "SET enable_seqscan=off", "SET enable_bitmapscan=off",
        "SET enable_memoize=off", "SET enable_material=off",
    ]
    statements.extend(setup)
    for sql in setup:
        cur.execute(sql)
    plan = explain("api-rescan", "SELECT r.* FROM (VALUES (0::bigint),(1),(0),(1)) s(a) "
                   "CROSS JOIN LATERAL (SELECT anchor FROM requests WHERE anchor=s.a OFFSET 0) r")
    graph = next(n for n in nodes(plan[0]["Plan"])
                 if n.get("Custom Plan Provider") == "GraphExpandFixed")
    assert graph.get("Graph Prefix Representation") == "exact-k WALK endpoint sets (unordered)"
    assert int(plan[0]["Plan"]["Actual Rows"]) == 8192
    assert graph["Graph Rescans"] >= 3
    result["tests"]["api-rescan"] = {
        "rows": plan[0]["Plan"]["Actual Rows"], "rescans": graph["Graph Rescans"],
        "scope": "public fixed-runtime diagnostic bridge, not formal PGQ LATERAL endpoint admission"}
    result["all_checks_passed"] = True
    result["limitations"] = ["no spill implementation", "no performance acceptance",
                              "assert-enabled binary must be validated separately"]


def main():
    args = parse_args()
    out = run_dir("G1-080-T1-T2-resource-acceptance")
    owned = {"sql": {"setup": [], "queries": {}}}
    result = {"started": now(), "tests": {}, "all_checks_passed": False}
    failure = None
    try:
        run_acceptance(args, out, result, owned)
    except Exception as error:
        failure = error
        result["error"] = {"type": type(error).__name__, "message": str(error),
                           "traceback": traceback.format_exc()}
    finally:
        errors = []
        worker = owned.get("worker")
        conn = owned.get("connection")
        if worker is not None and worker.is_alive():
            try:
                # Only our own connection/query is cancelled.  Never stop the
                # supplied postmaster or cancel another session's backend.
                conn.cancel()
                worker.join(timeout=10)
                if worker.is_alive():
                    errors.append("own cancellation worker did not exit within 10 seconds")
            except Exception as error:
                errors.append("cancel own worker: " + str(error))
        for name in ("cursor", "connection", "admin"):
            if name != "admin" and worker is not None and worker.is_alive():
                # Do not race cursor/libpq destruction against a live worker.
                # Failure is recorded above; daemon/process teardown will
                # close this script's socket if cancellation itself is broken.
                continue
            resource = owned.get(name)
            if resource is not None:
                try:
                    resource.close()
                except Exception as error:
                    errors.append("close " + name + ": " + str(error))
        if errors:
            result["cleanup_errors"] = errors
            failure = failure or RuntimeError("resource cleanup failed")
        result["finished"] = now()
        result["status"] = "failed" if failure else "passed"
        if failure:
            result["all_checks_passed"] = False
        result["postmaster_lifecycle_changed"] = False
        result["test_database_retained"] = result.get("database")
        write(out / "lifecycle-sql.json", owned["sql"])
        write(out / "summary.json", result)
    print(json.dumps({"run_dir": str(out), "result": result}, ensure_ascii=False, indent=2))
    if failure:
        raise failure


if __name__ == "__main__":
    main()
