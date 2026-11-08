#!/usr/bin/env python3
"""Verify PGQ rewrite provenance and RTI/Var pull-up mapping."""
from __future__ import annotations

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

TASK_ID = "G1-020"
SOURCE = WORKSPACE / ".worktrees/pg19-graph-core/postgres19"
INSTALL = WORKSPACE / ".install/pg19-graph-core/candidate"
RUNTIME = WORKSPACE / ".runtime/pg19-graph-core/candidate"
MODULE = SOURCE / "src/test/modules/test_graph_core"
GOLDEN = PACKAGE / "golden/G1-020/contracts.json"


def evidence(path: Path) -> dict:
    return {"path": str(path.relative_to(PACKAGE)), "sha256": sha(path)}


def node_format_digest() -> str:
    digest = hashlib.sha256()
    for path in (
        INSTALL / "share/postgresql/postgres.bki",
        INSTALL / "include/postgresql/server/nodes/parsenodes.h",
        INSTALL / "include/postgresql/server/nodes/graphnodes.h",
    ):
        digest.update(path.read_bytes())
    return digest.hexdigest()


def refresh_node_format_cluster(out: Path) -> dict:
    data = RUNTIME / "data"
    marker = RUNTIME / ".g1-020-node-format.sha256"
    current = node_format_digest()
    previous = marker.read_text().strip() if marker.exists() else None
    archived = None
    if data.exists() and previous != current:
        archive_root = RUNTIME / "catalog-archives"
        archive_root.mkdir(parents=True, exist_ok=True)
        archived = archive_root / ("pre-" + out.name)
        if archived.exists():
            raise RuntimeError("refusing to overwrite node-format archive: " + str(archived))
        data.rename(archived)
    return {"marker": marker, "current": current, "previous": previous,
            "archived_data": str(archived) if archived else None}


def main() -> None:
    out = run_dir("check-G1-020")
    golden = read(GOLDEN)
    lab = PACKAGE / "scripts/lab.py"
    commands: list[dict] = []
    started = False

    def sql(label: str, statement: str, value: bool = False) -> str | None:
        command = run_logged(
            [INSTALL / "bin/psql", "-X", "-qAt", "-v", "ON_ERROR_STOP=1",
             "-d", "graph_lab", "-c", statement],
            out, label, cwd=WORKSPACE, env=db_env("candidate"), timeout=120,
        )
        commands.append(command)
        if not value:
            return None
        lines = [line for line in (out / f"{label}.log").read_text().splitlines()
                 if line and not line.startswith(("NOTICE:", "WARNING:"))]
        if len(lines) != 1:
            raise RuntimeError(f"{label}: expected one SQL value, got {lines}")
        return lines[0]

    directed_query = """
      SELECT pad.x,gt.edge_id,gt.src_id,gt.dst_id
      FROM (VALUES (7)) pad(x),
           GRAPH_TABLE(g1020.g
            MATCH (a IS vertex)-[e IS edge]->(b IS vertex)
            COLUMNS(e.id AS edge_id,a.id AS src_id,b.id AS dst_id)) gt
    """
    graph_rows_sql = """
      SELECT coalesce(jsonb_agg(jsonb_build_array(x,edge_id,src_id,dst_id)
                                ORDER BY x,edge_id,src_id,dst_id),'[]'::jsonb)
      FROM (""" + directed_query + ") q"

    try:
        commands.append(run_logged(
            [sys.executable, lab, "stop", "--variant=candidate"],
            out, "pre-stop", cwd=WORKSPACE, timeout=120,
        ))
        commands.append(run_logged(
            [sys.executable, lab, "build", "--variant=candidate", "--jobs=4"],
            out, "build", cwd=WORKSPACE, timeout=1800,
        ))
        format_state = refresh_node_format_cluster(out)

        make = ["make", "-C", MODULE, "USE_PGXS=1",
                f"PG_CONFIG={INSTALL / 'bin/pg_config'}"]
        for label, args in (("module-clean", ["clean"]),
                            ("module-build", ["-j4"]),
                            ("module-install", ["install"])):
            commands.append(run_logged(make + args, out, label,
                                       cwd=WORKSPACE, timeout=180))
        commands.append(run_logged(
            [sys.executable, lab, "start", "--variant=candidate"],
            out, "start", cwd=WORKSPACE, timeout=180,
        ))
        started = True
        format_state["marker"].write_text(format_state["current"] + "\n")
        commands.append(run_logged(
            make + ["installcheck"], out, "native-regression", cwd=WORKSPACE,
            env=db_env("candidate"), timeout=180,
        ))
        shutil.copyfile(MODULE / "results/graph_provenance.out",
                        out / "native-results.out")

        vertices = ",".join(f"({row[0]},'{row[1]}')" for row in golden["fixture"]["vertices"])
        edges = ",".join(f"({row[0]},{row[1]},{row[2]},'e')" for row in golden["fixture"]["edges"])
        sql("setup", f"""
          SET client_min_messages=warning;
          DROP SCHEMA IF EXISTS g1020 CASCADE;
          DROP EXTENSION IF EXISTS test_graph_core CASCADE;
          CREATE EXTENSION test_graph_core;
          CREATE SCHEMA g1020;
          CREATE TABLE g1020.vertex(id bigint PRIMARY KEY,name text);
          CREATE TABLE g1020.edge(
            id bigint PRIMARY KEY,
            src bigint NOT NULL REFERENCES g1020.vertex(id),
            dst bigint NOT NULL REFERENCES g1020.vertex(id),payload text);
          INSERT INTO g1020.vertex VALUES {vertices};
          INSERT INTO g1020.edge VALUES {edges};
          CREATE PROPERTY GRAPH g1020.g
            VERTEX TABLES(g1020.vertex)
            EDGE TABLES(g1020.edge
              SOURCE KEY(src) REFERENCES vertex(id)
              DESTINATION KEY(dst) REFERENCES vertex(id));
        """)

        diagnostic = json.loads(sql(
            "provenance", "SELECT test_graph_pgq_provenance($q$" +
            directed_query + "$q$)::jsonb;", True
        ) or "null")
        if diagnostic["before_programs"] != 1 or diagnostic["before_graph_patterns"] != 0:
            raise RuntimeError("provenance was not captured before GraphPattern was cleared")
        if not diagnostic["top_program"] or diagnostic["top_disabled"] or not diagnostic["mapping_valid"]:
            raise RuntimeError("valid PGQ provenance did not survive query pull-up")
        variables = diagnostic["variables"]
        if len(variables) != 3 or not all(v["mapped"] for v in variables):
            raise RuntimeError("PGQ variables do not map one-to-one to fallback relations")
        if len({v["rti"] for v in variables}) != 3 or any(
                v["rti"] <= golden["outer_rti_floor"] for v in variables):
            raise RuntimeError("subquery pull-up did not offset all local RTIs")

        relation_oids = json.loads(sql(
            "relation-oids",
            "SELECT jsonb_build_array('g1020.vertex'::regclass::oid::bigint,"
            "'g1020.edge'::regclass::oid::bigint,"
            "'g1020.vertex'::regclass::oid::bigint);", True
        ) or "null")
        if [v["relation"] for v in variables] != relation_oids:
            raise RuntimeError("variable relation identity differs from the graph catalog mapping")

        graph_rows = json.loads(sql("graph-rows", graph_rows_sql, True) or "null")
        relational_rows = json.loads(sql("relational-rows", """
          SELECT jsonb_agg(jsonb_build_array(7,e.id,a.id,b.id) ORDER BY e.id)
          FROM g1020.edge e
          JOIN g1020.vertex a ON a.id=e.src
          JOIN g1020.vertex b ON b.id=e.dst;
        """, True) or "null")
        if graph_rows != golden["expected_rows"] or graph_rows != relational_rows:
            raise RuntimeError("GRAPH_TABLE fallback differs from the relational SQL oracle")

        invalid = json.loads(sql(
            "invalid-mapping", "SELECT test_graph_pgq_invalid_mapping($q$" +
            directed_query + "$q$)::jsonb;", True
        ) or "null")
        if (invalid["before_programs"] != 1 or invalid["after_programs"] != 0 or
                not invalid["top_disabled"] or invalid["mapping_valid"]):
            raise RuntimeError("invalid RTI mapping was not rejected fail-closed")
        post_rejection_rows = json.loads(sql(
            "post-rejection-rows", graph_rows_sql, True
        ) or "null")
        if post_rejection_rows != graph_rows:
            raise RuntimeError("rejected optional mapping changed the fallback query")

        undirected_query = """
          SELECT * FROM GRAPH_TABLE(g1020.g
           MATCH (a IS vertex)-[e IS edge]-(b IS vertex)
           COLUMNS(e.id AS edge_id,a.id AS src_id,b.id AS dst_id))
        """
        unsupported = json.loads(sql(
            "unsupported", "SELECT test_graph_pgq_provenance($q$" +
            undirected_query + "$q$)::jsonb;", True
        ) or "null")
        if (unsupported["before_programs"] != 0 or unsupported["top_program"] or
                not unsupported["top_disabled"]):
            raise RuntimeError("unsupported provenance was not disabled")
        undirected_count = int(sql(
            "unsupported-fallback", "SELECT count(*) FROM (" +
            undirected_query + ") q;", True
        ) or "0")
        if undirected_count <= 0:
            raise RuntimeError("unsupported sidecar disabled the relational fallback")

        mapping = {
            "before_pattern_cleared": {
                "programs": diagnostic["before_programs"],
                "graph_patterns": diagnostic["before_graph_patterns"],
            },
            "pullup": diagnostic,
            "variable_relations": golden["expected_variable_relations"],
            "invalid_mapping": invalid,
            "unsupported": unsupported,
        }
        fallback = {
            "graph_rows": graph_rows,
            "relational_rows": relational_rows,
            "post_rejection_rows": post_rejection_rows,
            "undirected_row_count": undirected_count,
        }
        write(out / "mapping.json", mapping)
        write(out / "fallback.json", fallback)
        results = {
            "task_id": TASK_ID, "finished": now(),
            "postgres_sha256": sha(INSTALL / "bin/postgres"),
            "golden_sha256": sha(GOLDEN), "mapping": mapping,
            "fallback": fallback,
            "node_format": {
                key: (str(value.relative_to(WORKSPACE)) if isinstance(value, Path) else value)
                for key, value in format_state.items()
            },
            "commands": commands,
        }
        write(out / "results.json", results)
        sql("cleanup", "DROP SCHEMA g1020 CASCADE;")
        commands.append(run_logged(
            [sys.executable, lab, "stop", "--variant=candidate"],
            out, "post-stop", cwd=WORKSPACE, timeout=120,
        ))
        started = False
        write(out / "summary.json", {
            "status": "PASS", "task_id": TASK_ID, "finished": now(),
            "results_sha256": sha(out / "results.json"), "commands": commands,
        })
        print(json.dumps({
            "task_id": TASK_ID, "status": "PASS",
            "assertions_passed": [
                "before_pattern_cleared", "pullup_offset_mapping",
                "fallback_query_unchanged", "invalid_mapping_rejected",
            ],
            "evidence_files": [evidence(path) for path in (
                GOLDEN, out / "results.json", out / "mapping.json",
                out / "fallback.json", out / "native-results.out",
                out / "native-regression.log", out / "provenance.log",
                out / "invalid-mapping.log",
            )],
        }, ensure_ascii=False, sort_keys=True))
    finally:
        if started:
            run_logged(
                [sys.executable, lab, "stop", "--variant=candidate"],
                out, "failure-stop", cwd=WORKSPACE, timeout=120,
            )


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, KeyError,
            subprocess.SubprocessError, json.JSONDecodeError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
