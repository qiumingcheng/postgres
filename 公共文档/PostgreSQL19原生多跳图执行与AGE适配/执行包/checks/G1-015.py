#!/usr/bin/env python3
"""Exercise real graph-statistics catalog publication, scope and rollback."""
from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys

SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS))
from common import PACKAGE, WORKSPACE, now, read, run_dir, run_logged, sha, write  # noqa: E402
from lab import db_env  # noqa: E402

TASK_ID = "G1-015"
GOLDEN = PACKAGE / "golden/G1-015/catalog_publish.json"
INSTALL = WORKSPACE / ".install/pg19-graph-core/candidate"
RUNTIME = WORKSPACE / ".runtime/pg19-graph-core/candidate"


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
        self.execute("SET statement_timeout='10s';")

    def execute(self, statement: str, expected_error: str | None = None) -> list[str]:
        self.calls += 1
        token = f"__G1015_{self.name}_{self.calls}__"
        assert self.process.stdin is not None and self.process.stdout is not None
        self.process.stdin.write(statement + f"\n\\echo {token}\n")
        self.process.stdin.flush()
        rows = []
        for line in self.process.stdout:
            line = line.rstrip("\n")
            if line == token:
                self.transcript.append({"sql": statement, "output": rows})
                errors = [row for row in rows if row.startswith("ERROR:")]
                if expected_error:
                    if not errors or not any(expected_error in row for row in errors):
                        raise RuntimeError(f"{self.name}: expected {expected_error}, got {rows}")
                elif errors:
                    raise RuntimeError(f"{self.name}: unexpected SQL error: {rows}")
                return rows
            rows.append(line)
        raise RuntimeError(f"{self.name}: psql exited before marker")

    def value(self, statement: str) -> str:
        rows = [row for row in self.execute(statement) if row and not row.startswith("NOTICE:")]
        if len(rows) != 1:
            raise RuntimeError(f"{self.name}: expected one SQL value: {rows}")
        return rows[0]

    def close(self) -> None:
        if self.process.poll() is None:
            assert self.process.stdin is not None
            self.process.stdin.write("\\q\n")
            self.process.stdin.flush()
        self.process.wait(timeout=10)


def scope_sql(stat_oid: int, scope: dict) -> str:
    return f"SELECT pg_graph_statistic_read({stat_oid},{literal(scope)});"


def publish_sql(stat_oid: int, profiles: list[dict]) -> str:
    return f"SELECT pg_graph_statistic_publish({stat_oid},{literal(profiles)});"


def catalog_hash(session: Session) -> dict:
    result = {}
    for table in ("pg_statistic", "pg_statistic_ext", "pg_statistic_ext_data"):
        result[table] = session.value(
            f"SELECT md5(coalesce(string_agg(row_to_json(t)::text,'' ORDER BY row_to_json(t)::text),'')) FROM {table} t;"
        )
    return result


def evidence(path: Path) -> dict:
    return {"path": str(path.relative_to(PACKAGE)), "sha256": sha(path)}


def main() -> None:
    out = run_dir("check-G1-015")
    golden = read(GOLDEN)
    lab = PACKAGE / "scripts/lab.py"
    commands = []
    sessions: list[Session] = []
    closed_transcripts: dict = {}
    started = False
    try:
        commands.append(run_logged([sys.executable, lab, "stop", "--variant=candidate"], out, "pre-stop", timeout=120))
        commands.append(run_logged([sys.executable, lab, "build", "--variant=candidate", "--jobs=4"], out, "build", timeout=1800))
        digest = hashlib.sha256()
        for path in (
            INSTALL / "share/postgresql/postgres.bki",
            INSTALL / "include/postgresql/server/nodes/parsenodes.h",
            INSTALL / "include/postgresql/server/nodes/graphnodes.h",
        ):
            digest.update(path.read_bytes())
        current = digest.hexdigest()
        marker = RUNTIME / ".g1-015-catalog.sha256"
        previous = marker.read_text().strip() if marker.exists() else None
        archive = None
        data = RUNTIME / "data"
        if previous != current and data.exists():
            root = RUNTIME / "catalog-archives"
            root.mkdir(exist_ok=True)
            archive = root / ("pre-" + out.name)
            data.rename(archive)
        commands.append(run_logged([sys.executable, lab, "start", "--variant=candidate"], out, "start", timeout=180))
        started = True
        marker.write_text(current + "\n")

        writer, observer, old_reader = (Session(name) for name in ("writer", "observer", "old_reader"))
        sessions.extend((writer, observer, old_reader))
        writer.execute(
            "DROP SCHEMA IF EXISTS g1015 CASCADE; DROP ROLE IF EXISTS g1015_untrusted; CREATE SCHEMA g1015; "
            "CREATE TABLE g1015.vertex(id bigint PRIMARY KEY,payload text); "
            "CREATE TABLE g1015.edge(id bigint PRIMARY KEY,src bigint NOT NULL,dst bigint NOT NULL,payload text); "
            "CREATE INDEX edge_out ON g1015.edge(src); "
            "INSERT INTO g1015.vertex SELECT i,'v' FROM generate_series(1,7) i; "
            "INSERT INTO g1015.edge SELECT i,i,i+1,'e' FROM generate_series(1,6) i; "
            "CREATE PROPERTY GRAPH g1015.graph VERTEX TABLES(g1015.vertex KEY(id)) "
            "EDGE TABLES(g1015.edge KEY(id) SOURCE KEY(src) REFERENCES vertex(id) DESTINATION KEY(dst) REFERENCES vertex(id)); "
            "ANALYZE g1015.vertex; ANALYZE g1015.edge;"
        )
        role = int(writer.value("SELECT oid FROM pg_roles WHERE rolname=current_user;"))
        graph = int(writer.value("SELECT 'g1015.graph'::regclass::oid;"))
        edge = int(writer.value(f"SELECT oid FROM pg_propgraph_element WHERE pgepgid={graph} AND pgekind='e';"))
        index = int(writer.value("SELECT 'g1015.edge_out'::regclass::oid;"))
        scope = {
            "catalog_provider_oid": 0, "graph_oid": graph, "edge_element_oid": edge,
            "index_relid": 0, "effective_role": role, "endpoint_domain": 77,
            "scope_kind": 1, "direction": 1, "path_mode": 1, "result_mode": 1,
            "catalog_generation": golden["catalog_generation"],
            "security_generation": golden["security_generation"], "build_generation": 0,
            "semantic_fingerprint": 12345,
        }
        profiles = copy.deepcopy(golden["profiles"])
        for profile in profiles:
            profile["metadata"] = copy.deepcopy(golden["metadata"])
        # Equal metric IDs with distinct full profile keys must coexist.
        conditional = copy.deepcopy(profiles[0])
        conditional["profile_key"] = {"case": "logical_edges", "predicate": "safe_template_1"}
        profiles.append(conditional)
        profile_order = lambda p: (p["metric_kind"], json.dumps(p["profile_key"], sort_keys=True))
        before_native = catalog_hash(writer)
        codec = json.loads(writer.value(f"SELECT pg_graph_statistic_scope({literal(scope)});"))
        if codec != scope:
            raise RuntimeError("statistics scope codec changed the complete scope")
        stat = int(writer.value(f"SELECT pg_graph_statistic_create({literal(scope)},'{{}}'::jsonb);"))
        if writer.value(publish_sql(stat, profiles)) != "1":
            raise RuntimeError("initial publication did not create stats generation 1")
        first = json.loads(observer.value(scope_sql(stat, scope)))
        if first["scope"] != scope or first["generation"] != 1:
            raise RuntimeError("initial full scope/generation read differs")
        received = sorted(first["profiles"], key=profile_order)
        if received != sorted(profiles, key=profile_order):
            raise RuntimeError(f"typed metric/profile codec roundtrip differs: {received}")

        old_reader.execute("BEGIN ISOLATION LEVEL REPEATABLE READ;")
        old_first = json.loads(old_reader.value(scope_sql(stat, scope)))
        writer.execute("BEGIN;")
        second_profiles = copy.deepcopy(profiles)
        second_profiles[0]["estimate"]["expected"] = 8
        second_profiles[0]["estimate"]["uncertainty_low"] = 8
        second_profiles[0]["estimate"]["uncertainty_high"] = 8
        if writer.value(publish_sql(stat, second_profiles)) != "2":
            raise RuntimeError("second publication generation differs")
        during = json.loads(observer.value(scope_sql(stat, scope)))
        if during != first:
            raise RuntimeError("uncommitted head/payload leaked into a second backend")
        writer.execute("ROLLBACK;")
        after_abort = json.loads(observer.value(scope_sql(stat, scope)))
        if after_abort != first:
            raise RuntimeError("aborted publication changed the active generation")
        if observer.value(f"SELECT count(*) FROM pg_graph_statistic_data WHERE gstdstatid={stat} AND gstdgeneration=2;") != "0":
            raise RuntimeError("aborted payload survived")

        writer.execute("BEGIN;")
        writer.value(publish_sql(stat, second_profiles))
        writer.execute("COMMIT;")
        second = json.loads(observer.value(scope_sql(stat, scope)))
        if second["generation"] != 2 or len(second["profiles"]) != len(profiles):
            raise RuntimeError("commit did not atomically expose all new payloads")
        writer.execute("VACUUM pg_graph_statistic; VACUUM pg_graph_statistic_data;")
        old_after_commit = json.loads(old_reader.value(scope_sql(stat, scope)))
        if old_after_commit != old_first:
            raise RuntimeError("old snapshot combined different head/data generations")
        old_reader.execute("COMMIT;")
        if observer.value(f"SELECT count(*) FROM pg_graph_statistic_data WHERE gstdstatid={stat} AND gstdgeneration=1;") != "0":
            raise RuntimeError("superseded payloads remain live instead of using MVCC deletion")

        writer.execute("BEGIN;")
        writer.value(publish_sql(stat, profiles))
        observer.execute("SET lock_timeout='200ms';")
        observer.execute(publish_sql(stat, profiles), "55P03")
        observer.execute("SET lock_timeout=0;")
        writer.execute("ROLLBACK;")
        if json.loads(observer.value(scope_sql(stat, scope))) != second:
            raise RuntimeError("contending/aborted publishers changed the active generation")

        # Duplicate profiles fail after partial catalog insertion; rollback must
        # preserve the head and remove every inserted row from the failed call.
        writer.execute(publish_sql(stat, profiles + [profiles[0]]), "23505")
        if json.loads(observer.value(scope_sql(stat, scope))) != second:
            raise RuntimeError("mid-publication ERROR exposed a partial generation")
        writer.execute(publish_sql(stat, []), "54000")
        writer.execute(publish_sql(stat, [profiles[0]] * 65), "54000")
        bad_metric = copy.deepcopy(profiles[:1])
        bad_metric[0]["estimate"]["censored"] = True
        writer.execute(publish_sql(stat, bad_metric), "22023")
        bad_metric = copy.deepcopy(profiles[1:2])
        bad_metric[0]["estimate"]["state"] = 1
        writer.execute(publish_sql(stat, bad_metric), "22023")
        writer.execute(f"SELECT pg_graph_statistic_scope({literal(dict(scope, direction=1.5))});", "22023")
        for assignment in ("gstdpayload=decode('00','hex')", "gstdformat=99"):
            writer.execute("BEGIN;")
            writer.execute(f"UPDATE pg_graph_statistic_data SET {assignment} WHERE gstdstatid={stat} AND gstdgeneration=2;")
            writer.execute(scope_sql(stat, scope), "XX001")
            writer.execute("ROLLBACK;")
        if json.loads(observer.value(scope_sql(stat, scope))) != second:
            raise RuntimeError("invalid input/corruption tests changed the committed payload")
        mismatch = dict(scope, security_generation=scope["security_generation"] + 1)
        if observer.value(f"SELECT pg_graph_statistic_read({stat},{literal(mismatch)}) IS NULL;") != "t":
            raise RuntimeError("security scope mismatch reused global graph metrics")

        writer.execute("CREATE ROLE g1015_untrusted; SET ROLE g1015_untrusted;")
        writer.execute("SELECT * FROM pg_graph_statistic_data;", "42501")
        writer.execute("SELECT * FROM pg_graph_statistic;", "42501")
        writer.execute(scope_sql(stat, scope), "42501")
        writer.execute(f"SELECT pg_graph_statistic_create({literal(scope)},'{{}}'::jsonb);", "42501")
        writer.execute("RESET ROLE; DROP ROLE g1015_untrusted;")
        writer.execute("ALTER TABLE g1015.edge ENABLE ROW LEVEL SECURITY;")
        writer.execute(scope_sql(stat, scope), "55000")
        writer.execute(publish_sql(stat, profiles), "55000")
        writer.execute(f"SELECT pg_graph_statistic_create({literal(scope)},'{{}}'::jsonb);", "55000")
        writer.execute("ALTER TABLE g1015.edge DISABLE ROW LEVEL SECURITY;")
        if json.loads(observer.value(scope_sql(stat, scope))) != second:
            raise RuntimeError("RLS rejection changed an independently committed generation")

        # Prove these are persistent catalog tuples, not a backend-local cache.
        for session in sessions:
            closed_transcripts[session.name] = session.transcript
            session.close()
        sessions.clear()
        commands.append(run_logged([sys.executable, lab, "stop", "--variant=candidate"], out, "persistence-stop", timeout=120))
        started = False
        commands.append(run_logged([sys.executable, lab, "start", "--variant=candidate"], out, "persistence-start", timeout=180))
        started = True
        writer, observer = (Session(name) for name in ("writer_restart", "observer_restart"))
        sessions.extend((writer, observer))
        if json.loads(observer.value(scope_sql(stat, scope))) != second:
            raise RuntimeError("committed statistics generation did not survive restart")

        physical_scope = dict(scope, scope_kind=2, index_relid=index, build_generation=golden["physical_build_generation"])
        physical = int(writer.value(f"SELECT pg_graph_statistic_create({literal(physical_scope)},'{{}}'::jsonb);"))
        writer.value(publish_sql(physical, profiles[:1]))
        physical_read = json.loads(observer.value(scope_sql(physical, physical_scope)))
        if physical_read["generation"] != 1 or physical_read["scope"]["build_generation"] != 91:
            raise RuntimeError("stats generation was coupled to the index build generation")
        if "slot_generation" in physical_read["scope"]:
            raise RuntimeError("statistics scope stores persistent endpoint-slot identity")

        dependency_count = int(writer.value(f"SELECT count(*) FROM pg_depend WHERE classid='pg_graph_statistic'::regclass AND objid={stat};"))
        if dependency_count < 4:
            raise RuntimeError("graph/type/base-relation dependencies were not registered")
        description = writer.value(f"SELECT pg_describe_object('pg_graph_statistic'::regclass,{stat},0);")
        if description != f"graph statistics object {stat}":
            raise RuntimeError("catalog dependency cannot describe its statistics object")
        writer.execute("REINDEX INDEX g1015.edge_out;")
        if observer.value(f"SELECT pg_graph_statistic_read({physical},{literal(physical_scope)}) IS NULL;") != "t":
            raise RuntimeError("REINDEX did not invalidate the physical profile")
        if json.loads(observer.value(scope_sql(stat, scope)))["generation"] != 2:
            raise RuntimeError("REINDEX invalidated independent logical statistics")

        writer.execute("ALTER TABLE g1015.edge ADD COLUMN extra integer;")
        if observer.value(f"SELECT pg_graph_statistic_read({stat},{literal(scope)}) IS NULL;") != "t":
            raise RuntimeError("relation-shape mismatch reused old statistics")
        writer.execute(publish_sql(stat, profiles), "55000")
        writer.execute("DROP PROPERTY GRAPH g1015.graph;")
        if writer.value(f"SELECT count(*) FROM pg_graph_statistic WHERE oid IN ({stat},{physical});") != "0":
            raise RuntimeError("DROP graph did not remove dependent statistics definitions")
        if writer.value(f"SELECT count(*) FROM pg_graph_statistic_data WHERE gstdstatid IN ({stat},{physical});") != "0":
            raise RuntimeError("DROP graph left payload rows behind")
        after_native = catalog_hash(writer)
        if after_native != before_native:
            raise RuntimeError("graph-statistics maintenance modified native column/extended statistics")

        maintenance = {
            "task_id": TASK_ID, "finished": now(), "scope": scope,
            "first": first, "during_uncommitted": during, "after_abort": after_abort,
            "second_committed": second, "old_snapshot_after_commit": old_after_commit,
            "physical": physical_read, "dependency_count": dependency_count,
            "native_statistics_before": before_native, "native_statistics_after": after_native,
            "transcripts": {**closed_transcripts, **{s.name: s.transcript for s in sessions}},
            "node_catalog_digest": current, "archived_data": str(archive) if archive else None,
            "commands": commands,
        }
        write(out / "maintenance.json", maintenance)
        for session in sessions:
            session.close()
        sessions.clear()
        commands.append(run_logged([sys.executable, lab, "stop", "--variant=candidate"], out, "post-stop", timeout=120))
        started = False
        write(out / "summary.json", {"status": "PASS", "task_id": TASK_ID, "commands": commands, "finished": now()})
        record = {
            "task_id": TASK_ID, "status": "PASS",
            "assertions_passed": ["stats_scope_codec_roundtrip", "catalog_transaction_atomic_publish", "old_generation_on_abort", "dependency_and_security_scope", "stats_generation_not_build_or_slot"],
            "maintenance_execution_evidence": evidence(out / "maintenance.json"),
            "evidence_files": [evidence(GOLDEN), evidence(out / "maintenance.json"), evidence(out / "summary.json")],
        }
        print(json.dumps(record, ensure_ascii=False, sort_keys=True))
    finally:
        if sessions:
            write(out / "session_transcripts.json", {**closed_transcripts, **{s.name: s.transcript for s in sessions}})
        for session in sessions:
            session.close()
        if started:
            run_logged([sys.executable, lab, "stop", "--variant=candidate"], out, "failure-stop", timeout=120)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError, KeyError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
