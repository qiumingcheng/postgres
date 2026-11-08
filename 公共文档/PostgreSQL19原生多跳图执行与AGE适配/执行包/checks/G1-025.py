#!/usr/bin/env python3
"""Verify explicit, bounded single-hop graph statistics collection."""
from __future__ import annotations

from collections import Counter
import hashlib
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

TASK_ID = "G1-025"
INSTALL = WORKSPACE / ".install/pg19-graph-core/candidate"
RUNTIME = WORKSPACE / ".runtime/pg19-graph-core/candidate"
MODULE = WORKSPACE / ".worktrees/pg19-graph-core/postgres19/src/test/modules/test_graph_core"
GOLDEN = PACKAGE / "golden/G1-025/contracts.json"


def literal(value: object) -> str:
    return "'" + json.dumps(value, sort_keys=True).replace("'", "''") + "'::jsonb"


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
        self.execute("SET statement_timeout='20s';")
        self.pid = int(self.value("SELECT pg_backend_pid();"))

    def send(self, sql: str) -> tuple[str, str]:
        self.calls += 1
        token = f"__G1025_{self.name}_{self.calls}__"
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
        rows = [r for r in self.execute(sql) if r and not r.startswith("NOTICE:")]
        if len(rows) != 1:
            raise RuntimeError(f"{self.name}: expected one value, got {rows}")
        return rows[0]

    def close(self) -> None:
        if self.process.poll() is None:
            assert self.process.stdin
            self.process.stdin.write("\\q\n")
            self.process.stdin.flush()
        self.process.wait(timeout=10)


def options(**changes: object) -> dict:
    result = {
        "mode": "full", "seed": 20260921, "vertex_sample_target": 8,
        "strata": 3, "heavy_hitter_limit": 8, "max_scan_pages": 1000,
        "max_examined_entries": 100000, "max_bindings": 100,
        "max_entries_per_seed": 1000, "max_memory_bytes": 64 * 1024 * 1024,
        "max_payload_bytes": 1024 * 1024, "max_profiles": 64,
        "deadline_ms": 10000, "lock_timeout_ms": 1000, "throttle_delay_ms": 0,
    }
    result.update(changes)
    return result


def collect(session: Session, stat: int, opts: dict) -> dict:
    return json.loads(session.value(
        f"SELECT pg_graph_statistic_collect({stat},{literal(opts)});"
    ))


def read_stats(session: Session, stat: int, scope: dict) -> dict:
    return json.loads(session.value(
        f"SELECT pg_graph_statistic_read({stat},{literal(scope)});"
    ))


def metric(document: dict, metric_id: int) -> dict:
    matches = [p for p in document["profiles"] if p["metric_kind"] == metric_id]
    if len(matches) != 1:
        raise RuntimeError(f"metric {metric_id} count differs: {len(matches)}")
    return matches[0]


def generations(session: Session, stat: int) -> tuple[int, int]:
    row = session.value(
        f"SELECT gstgeneration||'|'||(SELECT count(*) FROM pg_graph_statistic_data "
        f"WHERE gstdstatid={stat} AND gstdgeneration=gstgeneration) "
        f"FROM pg_graph_statistic WHERE oid={stat};"
    )
    return tuple(map(int, row.split("|")))  # type: ignore[return-value]


def sql_degrees(session: Session, direction: int) -> dict[int, int]:
    endpoint = "src" if direction == 1 else "dst"
    rows = json.loads(session.value(
        "SELECT json_agg(json_build_array(v.id,coalesce(d.degree,0)) ORDER BY v.id) "
        "FROM g1025.vertex v LEFT JOIN (SELECT " + endpoint +
        " endpoint,count(*) degree FROM g1025.edge GROUP BY 1) d ON d.endpoint=v.id;"
    ))
    return {int(key): int(degree) for key, degree in rows}


def profile_truth(document: dict) -> dict:
    return {
        "vertices": metric(document, 100)["estimate"]["expected"],
        "edges": metric(document, 101)["estimate"]["expected"],
        "mean": metric(document, 102)["estimate"]["expected"],
        "zero": metric(document, 103)["estimate"]["expected"],
        "p50": metric(document, 104)["estimate"]["expected"],
        "p95": metric(document, 105)["estimate"]["expected"],
        "p99": metric(document, 106)["estimate"]["expected"],
        "maximum": metric(document, 107)["estimate"]["expected"],
        "self_loops": metric(document, 108)["estimate"]["expected"],
        "parallel_extra": metric(document, 109)["estimate"]["expected"],
    }


def scope(graph: int, edge: int, role: int, direction: int) -> dict:
    return {
        "catalog_provider_oid": 0, "graph_oid": graph, "edge_element_oid": edge,
        "index_relid": 0, "effective_role": role,
        "endpoint_domain": 76 + direction, "scope_kind": 1,
        "direction": direction, "path_mode": 1, "result_mode": 1,
        "catalog_generation": 7, "security_generation": 11,
        "build_generation": 0, "semantic_fingerprint": 12345 + direction,
    }


def wait_throttle(observer: Session, pid: int) -> None:
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        state = observer.value(
            "SELECT coalesce((SELECT state='active' AND wait_event_type='Extension' "
            f"AND wait_event='GraphStatsThrottle' FROM pg_stat_activity WHERE pid={pid}),false);"
        )
        if state == "t":
            return
        time.sleep(0.02)
    raise RuntimeError("collector did not reach its bounded throttle checkpoint")


def evidence(path: Path) -> dict:
    return {"path": str(path.relative_to(PACKAGE)), "sha256": sha(path)}


def main() -> None:
    out = run_dir("check-G1-025")
    golden = read(GOLDEN)
    lab = PACKAGE / "scripts/lab.py"
    sessions: list[Session] = []
    commands = []
    results: dict = {}
    started = False
    try:
        commands.append(run_logged([sys.executable, lab, "stop", "--variant=candidate"], out, "pre-stop", timeout=120))
        commands.append(run_logged([sys.executable, lab, "build", "--variant=candidate", "--jobs=4"], out, "build", timeout=1800))
        make = ["make", "-C", MODULE, "USE_PGXS=1", f"PG_CONFIG={INSTALL / 'bin/pg_config'}"]
        for label, args in (("clean", ["clean"]), ("build", ["-j4"]), ("install", ["install"])):
            commands.append(run_logged(make + args, out, "module-" + label, timeout=120))
        digest = hashlib.sha256((INSTALL / "share/postgresql/postgres.bki").read_bytes()).hexdigest()
        marker = RUNTIME / ".g1-025-catalog.sha256"
        previous = marker.read_text().strip() if marker.exists() else None
        if previous != digest and (RUNTIME / "data").exists():
            archive = RUNTIME / "catalog-archives" / ("pre-" + out.name)
            archive.parent.mkdir(exist_ok=True)
            if archive.exists():
                raise RuntimeError("refusing to overwrite candidate catalog archive")
            (RUNTIME / "data").rename(archive)
        commands.append(run_logged([sys.executable, lab, "start", "--variant=candidate"], out, "start", timeout=180))
        started = True
        marker.write_text(digest + "\n")
        commands.append(run_logged(make + ["installcheck"], out, "native-regression",
                                   env=db_env("candidate"), timeout=120))
        shutil.copyfile(MODULE / "results/graph_stats_collect.out", out / "native-results.out")

        writer, collector, observer = (Session(n) for n in ("writer", "collector", "observer"))
        sessions.extend((writer, collector, observer))
        vertices = golden["fixture"]["vertices"]
        edges = golden["fixture"]["edges"]
        writer.execute(
            "SET client_min_messages=warning; DROP SCHEMA IF EXISTS g1025 CASCADE; CREATE SCHEMA g1025; "
            "CREATE TABLE g1025.vertex(id bigint PRIMARY KEY,payload text); "
            "CREATE TABLE g1025.edge(id bigint PRIMARY KEY,src bigint NOT NULL,dst bigint NOT NULL,payload text) WITH(fillfactor=50); "
            "INSERT INTO g1025.vertex VALUES " + ",".join(f"({v},'v')" for v in vertices) + "; "
            "INSERT INTO g1025.edge VALUES " + ",".join(f"({i},{s},{d},'e')" for i, s, d in edges) + "; "
            "CREATE INDEX edge_adj ON g1025.edge USING graph_adj(src,dst); "
            "CREATE PROPERTY GRAPH g1025.graph VERTEX TABLES(g1025.vertex KEY(id)) "
            "EDGE TABLES(g1025.edge KEY(id) SOURCE KEY(src) REFERENCES vertex(id) "
            "DESTINATION KEY(dst) REFERENCES vertex(id));"
        )
        graph = int(writer.value("SELECT 'g1025.graph'::regclass::oid;"))
        edge_element = int(writer.value(
            f"SELECT oid FROM pg_propgraph_element WHERE pgepgid={graph} AND pgekind='e';"
        ))
        role = int(writer.value("SELECT oid FROM pg_roles WHERE rolname=current_user;"))
        scopes = {d: scope(graph, edge_element, role, d) for d in (1, 2)}
        stats = {d: int(writer.value(
            f"SELECT pg_graph_statistic_create({literal(scopes[d])},'{{}}'::jsonb);"
        )) for d in (1, 2)}
        index_info = json.loads(writer.value(
            "SELECT pg_catalog.graph_adj_index_info('g1025.edge_adj'::regclass);"
        ))
        if index_info["out_physical_entries"] != 9 or index_info["in_physical_entries"] != 9:
            raise RuntimeError("dual-direction graph_adj fixture was not physically populated")

        full: dict[int, dict] = {}
        sql_oracle: dict[int, dict] = {}
        for direction in (1, 2):
            diagnostic = collect(collector, stats[direction], options())
            document = read_stats(observer, stats[direction], scopes[direction])
            degrees = sql_degrees(observer, direction)
            ordered = sorted(degrees.values())
            truth = {
                "vertices": len(degrees), "edges": sum(ordered),
                "mean": sum(ordered) / len(ordered),
                "zero": sum(x == 0 for x in ordered) / len(ordered),
                "p50": ordered[3], "p95": ordered[-1], "p99": ordered[-1],
                "maximum": ordered[-1], "self_loops": 1, "parallel_extra": 1,
            }
            if diagnostic["population"] != 8 or diagnostic["visible_edges"] != 9:
                raise RuntimeError("logical vertex/edge population differs")
            if profile_truth(document) != truth:
                raise RuntimeError(f"full SQL oracle differs for direction {direction}: {profile_truth(document)} != {truth}")
            if Counter((x["key"], x["degree"]) for x in diagnostic["samples"]) != Counter(degrees.items()):
                raise RuntimeError("full collection sample differs from SQL LEFT JOIN, including isolates")
            full[direction] = {"diagnostic": diagnostic, "document": document}
            sql_oracle[direction] = {"degrees": degrees, "truth": truth}
        if metric(full[1]["document"], 101)["estimate"]["expected"] != 9:
            raise RuntimeError("collector counted graph_adj OUT/IN copies instead of logical edges")

        sampled_diag = collect(collector, stats[1], options(
            mode="sample", vertex_sample_target=6
        ))
        sampled_doc = read_stats(observer, stats[1], scopes[1])
        sampled_degrees = sql_degrees(observer, 1)
        strata_profiles = [p for p in sampled_doc["profiles"] if p["metric_kind"] == 110]
        if len(strata_profiles) != 3 or sampled_diag["sample_count"] != 6:
            raise RuntimeError("stratified reservoir did not allocate its exact target")
        expected_weights = {
            p["profile_key"]["stratum"]:
                p["profile_key"]["stratum_population"] / p["profile_key"]["stratum_samples"]
            for p in strata_profiles
        }
        if any(abs(p["estimate"]["expected"] - expected_weights[p["profile_key"]["stratum"]]) > 1e-12
               for p in strata_profiles):
            raise RuntimeError("published survey weight is not N_h / s_h")
        weighted_sum = sum(
            sampled_degrees[row["key"]] * expected_weights[row["stratum"]]
            for row in sampled_diag["samples"]
        )
        sampled_mean = metric(sampled_doc, 102)["estimate"]["expected"]
        if abs(sampled_mean - weighted_sum / 8) > 1e-12:
            raise RuntimeError("sample mean did not apply stratified survey weights")

        heavy = [p for p in sampled_doc["profiles"] if p["metric_kind"] == 111]
        if not heavy or any(
            p["profile_key"]["population"] != "discovered_heavy_hitters" or
            p["estimate"]["expected"] != sampled_degrees[p["profile_key"]["endpoint"]]
            for p in heavy
        ):
            raise RuntimeError("discovered heavy hitters were not separately exact-rechecked")
        if metric(sampled_doc, 100)["profile_key"]["population"] != "uniform_vertices":
            raise RuntimeError("heavy discovery contaminated the uniform vertex population")

        before_prefix = generations(observer, stats[1])
        prefix = collect(collector, stats[1], options(max_examined_entries=7))
        if prefix["status"] != "PARTIAL" or prefix["published"] or generations(observer, stats[1]) != before_prefix:
            raise RuntimeError("prefix collection was published as a full sample")
        censored = collect(collector, stats[1], options(max_entries_per_seed=1))
        censored_doc = read_stats(observer, stats[1], scopes[1])
        for metric_id in (102, 104, 105, 106, 107):
            estimate = metric(censored_doc, metric_id)["estimate"]
            if not estimate["censored"] or estimate["kind"] != 2 or estimate["state"] != 2:
                raise RuntimeError(f"censored degree metric {metric_id} claimed a complete estimate")
        if not censored["censored"] or metric(censored_doc, 103)["estimate"]["expected"] != 0.375:
            raise RuntimeError("degree cap censoring or exact zero-degree fraction differs")

        # A conflicting DDL-class lock must stop at the explicit collector budget.
        writer.execute("BEGIN; LOCK TABLE g1025.edge IN ACCESS EXCLUSIVE MODE;")
        lock_before = generations(observer, stats[1])
        lock_result = collect(collector, stats[1], options(lock_timeout_ms=30))
        writer.execute("ROLLBACK;")
        if lock_result["reason"] != "lock_unavailable" or generations(observer, stats[1]) != lock_before:
            raise RuntimeError("lock budget did not fail closed without publication")

        # Publication is ordinary MVCC state and must disappear on transaction abort.
        rollback_before = generations(observer, stats[1])
        collector.execute("BEGIN;")
        rollback_result = collect(collector, stats[1], options())
        collector.execute("ROLLBACK;")
        if not rollback_result["published"] or generations(observer, stats[1]) != rollback_before:
            raise RuntimeError("aborted explicit collection leaked a generation")

        # A real long scan is cancelled at an instrumented checkpoint.  The same
        # backend must remain reusable and VACUUM plus a new collection must work.
        # The physical-copy assertion above has already captured graph_adj's two
        # directions.  Drop that prototype index so this collector stress case
        # is independent of graph_adj's deliberately bounded active segments.
        writer.execute("DROP INDEX g1025.edge_adj;")
        writer.execute(
            "INSERT INTO g1025.edge SELECT 10000+i,10,20,'bulk' FROM generate_series(1,4096) i;"
        )
        cancel_before = generations(observer, stats[1])
        pending = collector.send(
            f"SELECT pg_graph_statistic_collect({stats[1]},"
            f"{literal(options(max_entries_per_seed=10000, throttle_delay_ms=200))});"
        )
        wait_throttle(observer, collector.pid)
        if observer.value(f"SELECT pg_cancel_backend({collector.pid});") != "t":
            raise RuntimeError("could not cancel active graph collection")
        collector.finish(pending, "57014")
        if generations(observer, stats[1]) != cancel_before:
            raise RuntimeError("cancelled collection exposed a partial generation")
        writer.execute("VACUUM g1025.edge;")
        recovered = collect(collector, stats[1], options(max_entries_per_seed=10000))
        recovered_doc = read_stats(observer, stats[1], scopes[1])
        if recovered["visible_edges"] != 4105 or metric(recovered_doc, 101)["estimate"]["expected"] != 4105:
            raise RuntimeError("collector backend did not recover after cancel/VACUUM")

        writer.execute("DROP ROLE IF EXISTS g1025_untrusted; CREATE ROLE g1025_untrusted; SET ROLE g1025_untrusted;")
        writer.execute(
            f"SELECT pg_graph_statistic_collect({stats[1]},{literal(options())});", "42501"
        )
        writer.execute("RESET ROLE; DROP ROLE g1025_untrusted;")

        results = {
            "index_info": index_info, "full": full, "sql_oracle": sql_oracle,
            "sample": {"diagnostic": sampled_diag, "document": sampled_doc,
                       "weights": expected_weights, "weighted_mean": sampled_mean},
            "prefix": prefix, "censored": {"diagnostic": censored, "document": censored_doc},
            "lock_budget": lock_result, "rollback": rollback_result,
            "cancel_generation": cancel_before, "recovered": recovered,
            "recovered_document": recovered_doc, "postgres_sha256": sha(INSTALL / "bin/postgres"),
            "golden_sha256": sha(GOLDEN), "finished": now(),
        }
        write(out / "results.json", results)
        statistics = {
            "collector": "pg_graph_statistic_collect(oid,jsonb)",
            "snapshot": "one active MVCC snapshot borrowed by three TableAM scans",
            "truth_source": "SQL LEFT JOIN from vertex population to logical edge heap",
            "logical_edges": 9, "graph_adj_physical_direction_entries": 18,
            "zero_degree_fraction": 0.375, "sample_weights": expected_weights,
            "heavy_profiles_exact_rechecked": len(heavy),
            "censored_metrics": [102, 104, 105, 106, 107],
            "full_generations": {str(d): full[d]["document"]["generation"] for d in (1, 2)},
        }
        maintenance = {
            "entrypoint": "explicit superuser SQL only; no ANALYZE/planner/DML hook",
            "prefix_result": prefix, "lock_result": lock_result,
            "cancelled_backend_pid": collector.pid,
            "generation_before_cancel": cancel_before,
            "post_cancel_generation": recovered["generation"],
            "post_cancel_resources": recovered["resources"],
            "vacuum_after_cancel": "PASS", "backend_reuse_after_cancel": "PASS",
            "transactional_rollback": "PASS",
        }
        write(out / "statistics.json", statistics)
        write(out / "maintenance.json", maintenance)
        for session in sessions:
            session.close()
            write(out / (session.name + ".json"), session.transcript)
            if any("WARNING:" in row for item in session.transcript for row in item["output"]):
                raise RuntimeError("collector cleanup emitted a warning")
        sessions.clear()
        commands.append(run_logged([sys.executable, lab, "stop", "--variant=candidate"], out, "post-stop", timeout=120))
        started = False
        write(out / "summary.json", {
            "status": "PASS", "task_id": TASK_ID, "finished": now(),
            "results_sha256": sha(out / "results.json"), "commands": commands,
        })
        stats_evidence = evidence(out / "statistics.json")
        maintenance_evidence = evidence(out / "maintenance.json")
        print(json.dumps({
            "task_id": TASK_ID, "status": "PASS",
            "assertions_passed": [
                "vertex_population_includes_isolates", "visible_edges_not_index_copies",
                "uniform_stratified_weights", "heavy_hitters_separate",
                "censored_and_prefix_not_full_sample", "bounded_collect_cancel_cleanup",
                "sql_full_oracle",
            ],
            "evidence_files": [evidence(p) for p in (
                GOLDEN, out / "results.json", out / "statistics.json", out / "maintenance.json",
                out / "writer.json", out / "collector.json", out / "observer.json",
                out / "native-results.out", out / "native-regression.log",
            )],
            "maintenance_execution_evidence": maintenance_evidence,
            "statistics_evidence": stats_evidence,
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
