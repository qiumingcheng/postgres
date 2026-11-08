#!/usr/bin/env python3
"""Verify bounded fixed-path joint and physical graph statistics profiles."""
from __future__ import annotations

from collections import Counter
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS))
from common import PACKAGE, WORKSPACE, now, read, run_dir, run_logged, sha, write  # noqa: E402
from lab import db_env  # noqa: E402

TASK_ID = "G1-046"
SOURCE = WORKSPACE / ".worktrees/pg19-graph-core/postgres19"
INSTALL = WORKSPACE / ".install/pg19-graph-core/candidate"
RUNTIME = WORKSPACE / ".runtime/pg19-graph-core/candidate"
MODULE = SOURCE / "src/test/modules/test_graph_core"
GOLDEN = PACKAGE / "golden/G1-046/contracts.json"


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
        self.execute("SET statement_timeout='30s'; SET max_parallel_workers_per_gather=0;")

    def send(self, sql: str) -> tuple[str, str]:
        self.calls += 1
        token = f"__G1046_{self.name}_{self.calls}__"
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


def options(golden: dict, predicate: dict, **changes: object) -> dict:
    limits = golden["limits"]
    result = {
        "seed": limits["seed"], "seed_target": limits["seed_target"],
        "hops": golden["fixture"]["hops"],
        "predicate_attnum": predicate["attnum"],
        "predicate_min": predicate["min"], "predicate_max": predicate["max"],
        "predicate_clause_id": predicate["clause_id"],
        "max_scan_pages": limits["max_scan_pages"],
        "max_examined_entries": limits["max_examined_entries"],
        "max_bindings": limits["max_bindings"],
        "max_bindings_per_seed": limits["max_bindings_per_seed"],
        "max_entries_per_seed": limits["max_entries_per_seed"],
        "max_memory_bytes": limits["max_memory_bytes"],
        "max_payload_bytes": limits["max_payload_bytes"],
        "max_profiles": limits["max_profiles"],
        "max_parent_batch": limits["max_parent_batch"],
        "deadline_ms": limits["deadline_ms"],
        "lock_timeout_ms": limits["lock_timeout_ms"],
    }
    result.update(changes)
    return result


def scope(golden: dict, graph: int, edge: int, index: int, role: int,
          path_mode: int, fingerprint: int, physical: bool) -> dict:
    common = golden["scope"]
    return {
        "catalog_provider_oid": 0, "graph_oid": graph, "edge_element_oid": edge,
        "index_relid": index if physical else 0, "effective_role": role,
        "endpoint_domain": common["endpoint_domain"],
        "scope_kind": 2 if physical else 1, "direction": 1,
        "path_mode": path_mode, "result_mode": 1,
        "catalog_generation": common["catalog_generation"],
        "security_generation": common["security_generation"],
        "build_generation": 1 if physical else 0,
        "semantic_fingerprint": fingerprint,
    }


def create_pair(session: Session, logical: dict, physical: dict) -> tuple[int, int]:
    return (
        int(session.value(f"SELECT pg_graph_statistic_create({literal(logical)},'{{}}'::jsonb);")),
        int(session.value(f"SELECT pg_graph_statistic_create({literal(physical)},'{{}}'::jsonb);")),
    )


def collect(session: Session, pair: tuple[int, int], opts: dict) -> dict:
    return json.loads(session.value(
        f"SELECT pg_graph_statistic_collect_path({pair[0]},{pair[1]},{literal(opts)});"
    ))


def read_stats(session: Session, oid: int, stat_scope: dict) -> dict:
    return json.loads(session.value(
        f"SELECT pg_graph_statistic_read({oid},{literal(stat_scope)});"
    ))


def mode_allows(vertices: list[int], path_edges: list[int], edge: int,
                opposite: int, next_depth: int, hops: int, mode: int) -> bool:
    if mode == 1:
        return True
    if mode == 2:
        return edge not in path_edges
    if mode == 3:
        return opposite not in vertices
    if opposite not in vertices:
        return True
    return opposite == vertices[0] and next_depth == hops


def oracle(golden: dict, predicate: dict, mode: int) -> dict:
    vertices = golden["fixture"]["vertices"]
    edges = golden["fixture"]["edges"]
    hops = golden["fixture"]["hops"]
    degree = Counter(src for _, src, _, _ in edges)
    # Keep the executor's global binding order.  GgcBatch restores this
    # fixture's one-page heap candidates in root-TID order and fans each one
    # back out to matching parents in input order.  That order determines the
    # endpoint deduplication boundaries of the following parent batches.
    bindings: list[tuple[int, list[int], list[int]]] = [
        (seed, [seed], []) for seed in vertices
    ]
    joint = {seed: {name: [] for name in ("F", "B", "E", "A", "P")}
             for seed in vertices}
    stages: list[dict] = []
    arrivals: list[list[float | None]] = []
    for stage in range(hops):
        batch_size = golden["limits"]["max_parent_batch"]
        parent_batches = [bindings[start:start + batch_size]
                          for start in range(0, len(bindings), batch_size)]
        request_batches = [{path_vertices[-1]
                            for _, path_vertices, _ in batch}
                           for batch in parent_batches]
        global_endpoints = {path_vertices[-1]
                            for _, path_vertices, _ in bindings}
        summary = {
            "stage": stage + 1,
            # GgcBatch deduplicates equal endpoint keys within each bounded
            # parent batch, but not across batches.  The provider therefore
            # restores one physical candidate set per batch-local endpoint.
            "N": sum(len(batch) for batch in request_batches),
            "X": sum(degree[endpoint]
                     for batch in request_batches for endpoint in batch),
            "H_edge": sum(degree[endpoint]
                          for batch in request_batches for endpoint in batch),
            "zero_degree_requests": sum(degree[endpoint] == 0
                                         for endpoint in global_endpoints),
        }
        frontier = {seed: set() for seed in vertices}
        binding_count = Counter()
        logical = {seed: set() for seed in vertices}
        accepted = {seed: set() for seed in vertices}
        produced_count = Counter()
        arrival_degree = Counter()
        next_bindings: list[tuple[int, list[int], list[int]]] = []
        for seed, path_vertices, _ in bindings:
            binding_count[seed] += 1
            frontier[seed].add(path_vertices[-1])
        for batch, batch_endpoints in zip(parent_batches, request_batches):
            # Edge insertion order is root-TID order for this deliberately
            # compact fixture and therefore the independent expected restore
            # order after the page-group sort.
            for edge, src, dst, weight in edges:
                if src not in batch_endpoints:
                    continue
                for seed, path_vertices, path_edges in batch:
                    if path_vertices[-1] != src:
                        continue
                    logical[seed].add((src, edge))
                    if not predicate["min"] <= weight <= predicate["max"]:
                        continue
                    accepted[seed].add((src, edge))
                    if not mode_allows(path_vertices, path_edges, edge, dst,
                                       stage + 1, hops, mode):
                        continue
                    next_bindings.append(
                        (seed, path_vertices + [dst], path_edges + [edge]))
                    produced_count[seed] += 1
                    arrival_degree[seed] += degree[dst]
        stage_arrivals: list[float | None] = []
        for seed in vertices:
            values = (len(frontier[seed]), binding_count[seed],
                      len(logical[seed]), len(accepted[seed]),
                      produced_count[seed])
            for name, value in zip(("F", "B", "E", "A", "P"), values):
                joint[seed][name].append(value)
            stage_arrivals.append(
                arrival_degree[seed] / produced_count[seed]
                if produced_count[seed] else None)
        for name in ("F", "B", "E", "A", "P"):
            summary[name] = sum(joint[seed][name][-1] for seed in vertices)
        stages.append(summary)
        arrivals.append(stage_arrivals)
        bindings = next_bindings
    return {"stages": stages, "joint": joint, "arrivals": arrivals,
            "degree_mean": sum(degree.values()) / len(vertices)}


def profile(document: dict, metric_id: int, stage: int | None = None) -> list[dict]:
    rows = [item for item in document["profiles"]
            if item["metric_kind"] == metric_id]
    if stage is not None:
        rows = [item for item in rows
                if int(item["profile_key"].get("stage", -1)) == stage]
    return rows


def joint_bag(document: dict) -> Counter:
    result: Counter = Counter()
    for item in profile(document, 209):
        key = item["profile_key"]
        result[(tuple(key["F"]), tuple(key["B"]), tuple(key["E"]),
                tuple(key["A"]), tuple(key["P"]))] += 1
    return result


def oracle_joint_bag(document: dict) -> Counter:
    return Counter((tuple(value["F"]), tuple(value["B"]), tuple(value["E"]),
                    tuple(value["A"]), tuple(value["P"]))
                   for value in document["joint"].values())


def assert_status(actual: dict, expected: dict) -> None:
    if actual["status"] != "READY" or not actual["published"] or \
            actual["complete_depth"] != len(expected["stages"]):
        raise RuntimeError("complete fixed-path collection did not publish READY")
    for got, want in zip(actual["stages"], expected["stages"]):
        for key in ("F", "B", "N", "E", "A", "P", "X", "H_edge",
                    "zero_degree_requests"):
            if got[key] != want[key]:
                raise RuntimeError(f"stage {got['stage']} {key}: {got[key]} != {want[key]}")
        if want["H_edge"] and not 0 < got["heap_page_groups"] <= want["H_edge"]:
            raise RuntimeError("heap page grouping is absent or exceeds restored rows")


def evidence(path: Path) -> dict:
    return {"path": str(path.relative_to(PACKAGE)), "sha256": sha(path)}


def main() -> None:
    golden = read(GOLDEN)
    fixture = golden["fixture"]
    out = run_dir("check-G1-046")
    lab = PACKAGE / "scripts/lab.py"
    commands: list[dict] = []
    sessions: list[Session] = []
    started = False
    try:
        commands.append(run_logged([sys.executable, lab, "stop", "--variant=candidate"],
                                   out, "pre-stop", timeout=120))
        commands.append(run_logged([sys.executable, lab, "build", "--variant=candidate", "--jobs=4"],
                                   out, "build", timeout=1800))
        make = ["make", "-C", MODULE, "USE_PGXS=1",
                f"PG_CONFIG={INSTALL / 'bin/pg_config'}"]
        for label, args in (("clean", ["clean"]), ("build", ["-j4"]),
                            ("install", ["install"])):
            commands.append(run_logged(make + args, out, "module-" + label, timeout=180))

        digest = hashlib.sha256((INSTALL / "share/postgresql/postgres.bki").read_bytes()).hexdigest()
        marker = RUNTIME / ".catalog-bki.sha256"
        previous = marker.read_text().strip() if marker.exists() else None
        if previous != digest and (RUNTIME / "data").exists():
            archive = RUNTIME / "catalog-archives" / ("pre-" + out.name)
            archive.parent.mkdir(exist_ok=True)
            if archive.exists():
                raise RuntimeError("refusing to overwrite candidate catalog archive")
            (RUNTIME / "data").rename(archive)
        commands.append(run_logged([sys.executable, lab, "start", "--variant=candidate"],
                                   out, "start", timeout=180))
        started = True
        marker.write_text(digest + "\n")
        (RUNTIME / ".g1-046-catalog.sha256").write_text(digest + "\n")
        commands.append(run_logged(make + ["installcheck"], out, "native-regression",
                                   env=db_env("candidate"), timeout=240))
        shutil.copyfile(MODULE / "results/graph_stats_path.out", out / "native-results.out")

        writer, collector = Session("writer"), Session("collector")
        sessions.extend((writer, collector))
        edge_values = ",".join(
            f"({edge},{src},{dst},{weight},'e')"
            for edge, src, dst, weight in fixture["edges"]
        )
        writer.execute(
            "SET client_min_messages=warning; DROP SCHEMA IF EXISTS g1046 CASCADE; "
            "CREATE SCHEMA g1046; CREATE TABLE g1046.vertex(id bigint PRIMARY KEY); "
            "CREATE TABLE g1046.edge(id bigint PRIMARY KEY,src bigint NOT NULL,"
            "dst bigint NOT NULL,weight bigint,payload text) WITH(fillfactor=40); "
            "INSERT INTO g1046.vertex VALUES " +
            ",".join(f"({vertex})" for vertex in fixture["vertices"]) + "; "
            f"INSERT INTO g1046.edge VALUES {edge_values}; "
            "CREATE INDEX edge_out ON g1046.edge(src); "
            "CREATE PROPERTY GRAPH g1046.graph VERTEX TABLES(g1046.vertex KEY(id)) "
            "EDGE TABLES(g1046.edge KEY(id) SOURCE KEY(src) REFERENCES vertex(id) "
            "DESTINATION KEY(dst) REFERENCES vertex(id));"
        )
        graph = int(writer.value("SELECT 'g1046.graph'::regclass::oid;"))
        edge_element = int(writer.value(
            f"SELECT oid FROM pg_propgraph_element WHERE pgepgid={graph} AND pgekind='e';"
        ))
        index = int(writer.value("SELECT 'g1046.edge_out'::regclass::oid;"))
        role = int(writer.value("SELECT oid FROM pg_roles WHERE rolname=current_user;"))

        definitions = {
            "walk": (1, golden["scope"]["walk_fingerprint"], fixture["selective_predicate"]),
            "trail": (2, golden["scope"]["trail_fingerprint"], fixture["selective_predicate"]),
            "wide": (1, golden["scope"]["wide_fingerprint"], fixture["wide_predicate"]),
        }
        scopes: dict[str, tuple[dict, dict]] = {}
        pairs: dict[str, tuple[int, int]] = {}
        for name, (path_mode, fingerprint, _) in definitions.items():
            logical = scope(golden, graph, edge_element, index, role,
                            path_mode, fingerprint, False)
            physical = scope(golden, graph, edge_element, index, role,
                             path_mode, fingerprint, True)
            scopes[name] = (logical, physical)
            pairs[name] = create_pair(writer, logical, physical)

        statuses: dict[str, dict] = {}
        documents: dict[str, dict] = {}
        physical_documents: dict[str, dict] = {}
        oracles: dict[str, dict] = {}
        for name, (path_mode, _, predicate) in definitions.items():
            expected = oracle(golden, predicate, path_mode)
            status = collect(collector, pairs[name], options(golden, predicate))
            logical_doc = read_stats(collector, pairs[name][0], scopes[name][0])
            physical_doc = read_stats(collector, pairs[name][1], scopes[name][1])
            assert_status(status, expected)
            if joint_bag(logical_doc) != oracle_joint_bag(expected):
                raise RuntimeError(name + " per-seed joint vectors differ from independent enumeration")
            if any(item["estimate"]["kind"] != 0 or
                   item["estimate"]["state"] != 1 or
                   item["estimate"]["censored"]
                   for item in profile(logical_doc, 209)):
                raise RuntimeError(name + " complete joint vector was not exact-at-sample")
            expected_clause = predicate["clause_id"]
            for item in logical_doc["profiles"]:
                key = item["profile_key"]
                actual_clause = key.get("template", key.get("predicate", {})).get("clause_id")
                if actual_clause != expected_clause:
                    raise RuntimeError(name + " profile escaped its exact predicate clause scope")
                if "endpoint" in key:
                    raise RuntimeError("raw endpoint identity leaked into a profile key")
            statuses[name] = status
            documents[name] = logical_doc
            physical_documents[name] = physical_doc
            oracles[name] = expected

        if statuses["walk"]["stages"][-1]["P"] <= statuses["trail"]["stages"][-1]["P"]:
            raise RuntimeError("WALK and TRAIL profiles did not preserve their semantic difference")
        if statuses["wide"]["stages"][-1]["P"] == statuses["walk"]["stages"][-1]["P"]:
            raise RuntimeError("registered predicate profiles collapsed into one scope")
        if documents["trail"]["scope"]["path_mode"] != 2 or \
                documents["wide"]["scope"]["semantic_fingerprint"] == \
                documents["walk"]["scope"]["semantic_fingerprint"]:
            raise RuntimeError("mode or semantic fingerprint was not retained in the catalog scope")

        arrival = profile(documents["walk"], 208, 1)
        if len(arrival) != 1:
            raise RuntimeError("missing first-hop arrival-degree profile")
        arrival_mean = arrival[0]["estimate"]["expected"]
        expected_arrivals = [value for value in oracles["walk"]["arrivals"][0]
                             if value is not None]
        oracle_arrival = sum(expected_arrivals) / len(expected_arrivals)
        if abs(arrival_mean - oracle_arrival) > 1e-12 or \
                abs(arrival_mean - oracles["walk"]["degree_mean"]) < 1e-12:
            raise RuntimeError("arrival degree was replaced by the uniform vertex degree mean")

        physical = physical_documents["walk"]
        for stage, stage_status in enumerate(statuses["walk"]["stages"], 1):
            values = {}
            for metric_id, key in ((220, "X"), (221, "H_edge"),
                                   (222, "heap_page_groups"), (223, "H_edge"),
                                   (224, "zero_degree_requests")):
                rows = profile(physical, metric_id, stage)
                if len(rows) != 1:
                    raise RuntimeError(f"physical metric {metric_id} stage {stage} is absent")
                values[metric_id] = rows[0]["estimate"]["expected"]
                if values[metric_id] != stage_status[key]:
                    raise RuntimeError(f"physical metric {metric_id} differs from instrumentation")
            if stage_status["H_edge"] and values[222] >= values[221]:
                raise RuntimeError("heap page groups were conflated with per-edge TableAM fetches")
        if statuses["walk"]["stages"][0]["zero_degree_requests"] == 0 or \
                statuses["walk"]["stages"][0]["N"] <= \
                statuses["walk"]["stages"][0]["zero_degree_requests"]:
            raise RuntimeError("zero-degree endpoints disappeared from physical request work")

        partial_opts = options(
            golden, fixture["selective_predicate"],
            max_bindings_per_seed=golden["limits"]["partial_bindings_per_seed"],
        )
        partial = collect(collector, pairs["walk"], partial_opts)
        partial_doc = read_stats(collector, pairs["walk"][0], scopes["walk"][0])
        if partial["status"] != "PARTIAL" or not partial["published"] or \
                partial["complete_depth"] != 3 or partial["reason"] != "max_bindings_per_seed":
            raise RuntimeError("bounded truncation did not retain its completed prefix")
        stage_four = [item for item in partial_doc["profiles"]
                      if int(item["profile_key"].get("stage", -1)) == 4 and
                      item["metric_kind"] in (203, 204, 205, 207, 208)]
        if len(stage_four) != 5 or any(
                item["estimate"]["kind"] != 2 or
                item["estimate"]["state"] != 2 or
                not item["estimate"]["censored"] or
                item["metadata"]["complete_depth"] != 3
                for item in stage_four):
            raise RuntimeError("incomplete depth was published as a complete estimate")
        if not any((item["estimate"].get("lower_bound") or 0) > 0
                   for item in stage_four):
            raise RuntimeError("censored depth was silently replaced by zero")
        partial_joint = profile(partial_doc, 209)
        if not partial_joint or any(item["estimate"]["kind"] != 2 or
                                    item["estimate"]["state"] != 2 or
                                    not item["estimate"]["censored"]
                                    for item in partial_joint):
            raise RuntimeError("partial joint vectors claimed exact-at-sample state")

        source_file = SOURCE / "src/backend/statistics/graph_stats_path_collect.c"
        source_text = source_file.read_text()
        if "GgcFixedBegin(" in source_text or '"executor/graph_fixed.h"' in source_text or \
                "GgcGetIndexAdjProvider(index)" not in source_text or "GgcBatchBegin(" not in source_text:
            raise RuntimeError("collector depends on GraphPath/fixed-plan bootstrap or bypasses the low-level provider")
        if "GetGgcBtreeAdjProvider" in source_text or "GetGgcGraphAdjProvider" in source_text:
            raise RuntimeError("shared collector still links to a concrete adjacency provider")
        source_manifest = {}
        for path in (source_file, SOURCE / "src/include/statistics/graph_stats.h",
                     SOURCE / "src/include/catalog/pg_proc.dat",
                     MODULE / "sql/graph_stats_path.sql",
                     MODULE / "expected/graph_stats_path.out", GOLDEN):
            source_manifest[str(path.relative_to(WORKSPACE))] = sha(path)
        write(out / "source-manifest.json", source_manifest)

        results = {
            "statuses": statuses,
            "logical_documents": documents,
            "physical_documents": physical_documents,
            "oracles": oracles,
            "partial": {"status": partial, "document": partial_doc},
            "arrival_mean": arrival_mean,
            "uniform_degree_mean": oracles["walk"]["degree_mean"],
            "postgres_sha256": sha(INSTALL / "bin/postgres"),
            "golden_sha256": sha(GOLDEN), "finished": now(),
        }
        write(out / "results.json", results)
        statistics = {
            "collector": "pg_graph_statistic_collect_path(oid,oid,jsonb)",
            "bootstrap": "uniform vertex reservoir + provider/GgcBatch; no GraphPath",
            "joint_vectors_per_scope": len(fixture["vertices"]),
            "hops": fixture["hops"], "modes": ["WALK", "TRAIL"],
            "predicate_clause_ids": [fixture["selective_predicate"]["clause_id"],
                                     fixture["wide_predicate"]["clause_id"]],
            "arrival_mean": arrival_mean,
            "uniform_degree_mean": oracles["walk"]["degree_mean"],
            "complete_depth_after_censoring": partial["complete_depth"],
        }
        maintenance = {
            "entrypoint": "explicit superuser maintenance SQL",
            "full_status": statuses["walk"], "partial_status": partial,
            "resource_budgets": golden["limits"],
            "physical_stage_work": statuses["walk"]["stages"],
            "catalog_digest": digest,
        }
        write(out / "statistics.json", statistics)
        write(out / "maintenance.json", maintenance)

        for session in sessions:
            session.close()
            write(out / (session.name + ".json"), session.transcript)
            if any("WARNING:" in row for entry in session.transcript
                   for row in entry["output"]):
                raise RuntimeError("statistics maintenance emitted a cleanup warning")
        sessions.clear()
        commands.append(run_logged([sys.executable, lab, "stop", "--variant=candidate"],
                                   out, "post-stop", timeout=120))
        started = False
        write(out / "summary.json", {
            "status": "PASS", "task_id": TASK_ID, "finished": now(),
            "results_sha256": sha(out / "results.json"), "commands": commands,
        })
        files = (GOLDEN, out / "results.json", out / "statistics.json",
                 out / "maintenance.json", out / "source-manifest.json",
                 out / "writer.json", out / "collector.json",
                 out / "native-results.out", out / "native-regression.log")
        print(json.dumps({
            "task_id": TASK_ID, "status": "PASS",
            "assertions_passed": [
                "collector_no_graphpath_bootstrap", "frontier_binding_joint_profiles",
                "mode_predicate_scope_exact", "arrivals_not_uniform_vertices",
                "censored_depth_not_zero", "physical_x_fetch_h_page_groups_separate",
                "zero_degree_physical_work",
            ],
            "maintenance_execution_evidence": evidence(out / "maintenance.json"),
            "statistics_evidence": evidence(out / "statistics.json"),
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
