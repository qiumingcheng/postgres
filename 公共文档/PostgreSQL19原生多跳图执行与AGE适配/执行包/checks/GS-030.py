#!/usr/bin/env python3
"""Execute GS-030 ordinary OUT/IN scans and heap-owned MVCC/HOT checks."""
from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys

SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS))

from common import PACKAGE, WORKSPACE, now, run_dir, run_logged, sha, write  # noqa: E402
from lab import db_env, sql  # noqa: E402


TASK_ID = "GS-030"


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
            "SELECT pg_catalog.graph_adj_index_info('gs030.edges_adj'::regclass);",
        ).strip()
    )


def ids(predicate: str, force_index: bool = True) -> str:
    settings = (
        "SET enable_seqscan=off; SET enable_bitmapscan=off; "
        if force_index
        else "SET enable_indexscan=off; SET enable_bitmapscan=off; "
    )
    return sql(
        "candidate",
        settings
        + "SELECT coalesce(string_agg(edge_id::text,',' ORDER BY edge_id),'') "
        + "FROM gs030.edges WHERE "
        + predicate
        + ";",
    ).strip()


def explain(predicate: str, projection: str = "edge_id") -> dict:
    raw = sql(
        "candidate",
        "SET enable_seqscan=off; SET enable_bitmapscan=off; "
        f"EXPLAIN (ANALYZE,BUFFERS,FORMAT JSON) SELECT {projection} "
        f"FROM gs030.edges WHERE {predicate};",
    ).strip()
    return json.loads(raw)[0]


def find_index_scan(node: dict) -> dict | None:
    if node.get("Node Type") == "Index Scan" and node.get("Index Name") == "edges_adj":
        return node
    for child in node.get("Plans", []):
        found = find_index_scan(child)
        if found:
            return found
    return None


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
        marker = f"__GS030_{self.counter}__"
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
        raise RuntimeError("long-snapshot psql exited: " + self.proc.stderr.read())

    def close(self) -> None:
        if self.proc.poll() is None:
            assert self.proc.stdin is not None
            self.proc.stdin.write("\\q\n")
            self.proc.stdin.flush()
        self.proc.wait(timeout=10)


def evidence(path: Path) -> dict:
    return {"path": str(path.relative_to(PACKAGE)), "sha256": sha(path)}


def main() -> None:
    out = run_dir("check-GS-030")
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
            "DROP SCHEMA IF EXISTS gs030 CASCADE; CREATE SCHEMA gs030; "
            "CREATE TABLE gs030.edges("
            "edge_id bigint PRIMARY KEY,src bigint NOT NULL,dst bigint NOT NULL,"
            "payload text) WITH (fillfactor=70); "
            "CREATE INDEX edges_adj ON gs030.edges USING graph_adj(src,dst); "
            "INSERT INTO gs030.edges VALUES"
            "(1,10,20,'p1'),(2,10,20,'p2'),(3,30,30,'self'),(4,40,20,'p4'); "
            "ANALYZE gs030.edges;",
        )

        predicates = {
            "out": ("src=10", "1,2"),
            "in": ("dst=20", "1,2,4"),
            "both": ("src=10 AND dst=20", "1,2"),
            "self": ("src=30 AND dst=30", "3"),
        }
        plans = {}
        for name, (predicate, expected) in predicates.items():
            indexed = ids(predicate, True)
            sequential = ids(predicate, False)
            if indexed != expected or sequential != expected or indexed != sequential:
                raise RuntimeError(
                    f"{name} result mismatch: index={indexed}, seq={sequential}, expected={expected}"
                )
            plan = explain(predicate, "edge_id,payload")
            scan = find_index_scan(plan["Plan"])
            if scan is None:
                raise RuntimeError(f"{name} did not execute edges_adj: {json.dumps(plan)}")
            if "Index Cond" not in scan:
                raise RuntimeError(f"{name} scan did not retain an index condition")
            plans[name] = plan

        initial = info()
        if initial["out_physical_entries"] != 4 or initial["out_visible_entries"] != 4:
            raise RuntimeError("initial physical/logical edge counts are wrong")

        sql("candidate", "SELECT pg_stat_reset(); UPDATE gs030.edges SET payload='hot-1' WHERE edge_id=1; SELECT pg_stat_force_next_flush();")
        hot = info()
        if hot["out_physical_entries"] != initial["out_physical_entries"]:
            raise RuntimeError("payload-only update unexpectedly added graph_adj entries")
        hot_rows = sql(
            "candidate",
            "SET enable_seqscan=off; SET enable_bitmapscan=off; "
            "SELECT payload FROM gs030.edges WHERE src=10 AND edge_id=1;",
        ).strip()
        hot_updates = int(
            sql(
                "candidate",
                "SELECT pg_stat_get_tuples_hot_updated('gs030.edges'::regclass);",
            ).strip()
        )
        if hot_rows != "hot-1" or hot_updates < 1:
            raise RuntimeError(f"HOT chain was not exercised: payload={hot_rows}, count={hot_updates}")

        session = PsqlSession()
        try:
            first_view = session.execute(
                "BEGIN ISOLATION LEVEL REPEATABLE READ; "
                "SET LOCAL enable_seqscan=off; SET LOCAL enable_bitmapscan=off; "
                "SELECT payload FROM gs030.edges WHERE src=10 AND edge_id=1;"
            )
            sql("candidate", "UPDATE gs030.edges SET payload='hot-2' WHERE edge_id=1;")
            old_view = session.execute(
                "SELECT payload FROM gs030.edges WHERE src=10 AND edge_id=1;"
            )
            new_view = session.execute(
                "COMMIT; SET enable_seqscan=off; SET enable_bitmapscan=off; "
                "SELECT payload FROM gs030.edges WHERE src=10 AND edge_id=1;"
            )
        finally:
            session.close()
        if first_view != ["hot-1"] or old_view != ["hot-1"] or new_view != ["hot-2"]:
            raise RuntimeError(
                f"long snapshot changed versions: {first_view}, {old_view}, {new_view}"
            )

        before_nonhot = info()
        sql("candidate", "UPDATE gs030.edges SET src=11 WHERE edge_id=1;")
        after_nonhot = info()
        if after_nonhot["out_physical_entries"] != before_nonhot["out_physical_entries"] + 1:
            raise RuntimeError("endpoint-changing non-HOT update did not append a new edge version")
        if ids("src=10") != "2" or ids("src=11") != "1":
            raise RuntimeError("old/new endpoint visibility after non-HOT update is wrong")

        sql("candidate", "DELETE FROM gs030.edges WHERE edge_id=2;")
        final = info()
        if ids("src=10") != "":
            raise RuntimeError("deleted parallel edge remained logically visible")
        if final["out_physical_entries"] != 5 or final["out_visible_entries"] != 3:
            raise RuntimeError(
                "physical candidates and logical visible edges were not counted separately"
            )
        final_plan = explain("src=10", "edge_id,payload")
        final_scan = find_index_scan(final_plan["Plan"])
        if final_scan is None or final_scan.get("Actual Rows") != 0:
            raise RuntimeError("noncovering heap recheck did not filter dead physical candidates")
        if final_scan.get("Node Type") == "Index Only Scan":
            raise RuntimeError("prototype incorrectly advertised index-only capability")

        results = {
            "task_id": TASK_ID,
            "finished": now(),
            "catalog_refresh": catalog_refresh,
            "direction_and_parallel_edge_plans": plans,
            "initial_counts": initial,
            "hot_counts": hot,
            "hot_updates": hot_updates,
            "long_snapshot": {
                "first_view": first_view,
                "after_concurrent_commit": old_view,
                "after_own_commit": new_view,
            },
            "before_nonhot": before_nonhot,
            "after_nonhot": after_nonhot,
            "final_counts_after_delete": final,
            "dead_candidate_plan": final_plan,
            "commands": commands,
        }
        write(out / "results.json", results)
    finally:
        run_logged(stop, out, "post-stop", cwd=WORKSPACE, timeout=120)

    record = {
        "task_id": TASK_ID,
        "status": "PASS",
        "assertions_passed": [
            "out_only_in_only_both",
            "hot_chain",
            "long_snapshot_versions",
            "no_parallel_edge_dedup",
            "logical_physical_fetch_counts_separate",
            "noncovering_heap_fetch_contract",
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
