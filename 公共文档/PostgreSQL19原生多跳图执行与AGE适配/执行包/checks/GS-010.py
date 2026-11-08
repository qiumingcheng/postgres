#!/usr/bin/env python3
"""Build and execute the GS-010 graph_adj format/DDL contract."""
from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys

SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS))

from common import PACKAGE, WORKSPACE, now, run_dir, run_logged, sha, write  # noqa: E402
from lab import sql  # noqa: E402


TASK_ID = "GS-010"


def expect_rejected(statement: str) -> str:
    try:
        sql("candidate", statement)
    except RuntimeError as error:
        return str(error)
    raise RuntimeError("unsupported DDL was accepted: " + statement)


def evidence(path: Path) -> dict:
    return {"path": str(path.relative_to(PACKAGE)), "sha256": sha(path)}


def refresh_catalog_cluster(out: Path) -> tuple[Path, dict]:
    """Re-bootstrap the owned lab when bootstrap catalog input changes."""
    runtime = WORKSPACE / ".runtime/pg19-graph-core/candidate"
    data = runtime / "data"
    marker = runtime / ".catalog-bki.sha256"
    bki = (
        WORKSPACE
        / ".install/pg19-graph-core/candidate/share/postgresql/postgres.bki"
    )
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


def main() -> None:
    out = run_dir("check-GS-010")
    lab = PACKAGE / "scripts/lab.py"
    python = sys.executable
    commands = []
    stop = [python, lab, "stop", "--variant=candidate"]
    commands.append(run_logged(stop, out, "pre-stop", cwd=WORKSPACE, timeout=120))
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
    catalog_marker.write_text(
        catalog_refresh["installed_postgres_bki_sha256"] + "\n"
    )
    try:
        sql(
            "candidate",
            "DROP SCHEMA IF EXISTS gs010 CASCADE; CREATE SCHEMA gs010; "
            "CREATE TABLE gs010.edges(src bigint NOT NULL, dst bigint NOT NULL); "
            "CREATE INDEX edges_adj ON gs010.edges USING graph_adj(src, dst);",
        )
        catalog = sql(
            "candidate",
            "SELECT json_build_object("
            "'am',a.amname,'handler',p.proname,'opclass',o.opcname,"
            "'keytype',o.opcintype::regtype::text,'valid',i.indisvalid,"
            "'natts',i.indnatts,'nkeys',i.indnkeyatts,"
            "'bytes',pg_relation_size(i.indexrelid))::text "
            "FROM pg_index i JOIN pg_class c ON c.oid=i.indexrelid "
            "JOIN pg_am a ON a.oid=c.relam JOIN pg_proc p ON p.oid=a.amhandler "
            "JOIN pg_opclass o ON o.oid=i.indclass[0] "
            "WHERE i.indexrelid='gs010.edges_adj'::regclass;",
        ).strip()
        catalog_doc = json.loads(catalog)
        if catalog_doc != {
            "am": "graph_adj",
            "handler": "graphadjhandler",
            "opclass": "int8_graph_adj_ops",
            "keytype": "bigint",
            "valid": True,
            "natts": 2,
            "nkeys": 2,
            "bytes": 32768,
        }:
            raise RuntimeError("unexpected graph_adj catalog/layout: " + catalog)

        info = json.loads(
            sql(
                "candidate",
                "SELECT pg_catalog.graph_adj_index_info('gs010.edges_adj'::regclass);",
            ).strip()
        )
        required = {
            "magic": "0x47474132",
            "format_version": 2,
            "build_generation": 1,
            "out_root": 1,
            "in_root": 2,
            "slot_extent_head": 3,
            "out_root_kind": "DIR_LEAF",
            "out_root_direction": "OUT",
            "in_root_kind": "DIR_LEAF",
            "in_root_direction": "IN",
            "slot_page_kind": "ENDPOINT_SLOT",
            "slot_page_direction": "NONE",
            "directory_leaf_has_descriptor": False,
            "directory_leaf_item_bytes": 40,
            "descriptor_bytes": 64,
            "slot_bytes": 152,
            "entry_bytes": 40,
            "segment_header_bytes": 64,
            "vertex_ref_bytes": 24,
            "data_ref_bytes": 16,
            "invalid_slot_id": 0,
            "segment_bounds_checked": True,
        }
        for key, value in required.items():
            if info.get(key) != value:
                raise RuntimeError(f"metapage/layout mismatch for {key}: {info.get(key)!r}")
        if info.get("slots_per_page", 0) <= 1:
            raise RuntimeError("endpoint slots are not packed")
        if info.get("allocation_generation") == info.get("build_generation") == info.get("format_version"):
            raise RuntimeError("format/build/allocation generations are conflated")
        formats = sql(
            "candidate",
            "SELECT pg_catalog.graph_adj_format_supported(1),"
            "pg_catalog.graph_adj_format_supported(2),"
            "pg_catalog.graph_adj_format_supported(3);",
        ).strip()
        if formats != "f|t|f":
            raise RuntimeError("format registry does not reject V1/unknown versions")

        rejects = {}
        cases = {
            "unlogged": "CREATE UNLOGGED TABLE gs010.u(src bigint NOT NULL,dst bigint NOT NULL); CREATE INDEX ON gs010.u USING graph_adj(src,dst);",
            "nullable": "CREATE TABLE gs010.n(src bigint,dst bigint NOT NULL); CREATE INDEX ON gs010.n USING graph_adj(src,dst);",
            "wrong_type": "CREATE TABLE gs010.i(src integer NOT NULL,dst bigint NOT NULL); CREATE INDEX ON gs010.i USING graph_adj(src,dst);",
            "one_column": "CREATE TABLE gs010.one(src bigint NOT NULL,dst bigint NOT NULL); CREATE INDEX ON gs010.one USING graph_adj(src);",
            "three_columns": "CREATE TABLE gs010.three(src bigint NOT NULL,dst bigint NOT NULL,x bigint NOT NULL); CREATE INDEX ON gs010.three USING graph_adj(src,dst,x);",
            "expression": "CREATE TABLE gs010.e(src bigint NOT NULL,dst bigint NOT NULL); CREATE INDEX ON gs010.e USING graph_adj((src+0),dst);",
            "partial": "CREATE TABLE gs010.p(src bigint NOT NULL,dst bigint NOT NULL); CREATE INDEX ON gs010.p USING graph_adj(src,dst) WHERE src>0;",
            "include": "CREATE TABLE gs010.inc(src bigint NOT NULL,dst bigint NOT NULL,x text); CREATE INDEX ON gs010.inc USING graph_adj(src,dst) INCLUDE(x);",
            "unique": "CREATE TABLE gs010.uniq(src bigint NOT NULL,dst bigint NOT NULL); CREATE UNIQUE INDEX ON gs010.uniq USING graph_adj(src,dst);",
            "nonempty_before_GS020": "CREATE TABLE gs010.full(src bigint NOT NULL,dst bigint NOT NULL); INSERT INTO gs010.full VALUES(1,2); CREATE INDEX ON gs010.full USING graph_adj(src,dst);",
        }
        for name, statement in cases.items():
            rejects[name] = expect_rejected(statement)
        if any("graph_adj" not in message and "does not support" not in message for message in rejects.values()):
            raise RuntimeError("a rejection did not identify graph_adj/capability")

        result = {
            "task_id": TASK_ID,
            "finished": now(),
            "catalog": catalog_doc,
            "page_and_encoding": info,
            "format_registry": formats,
            "ddl_rejections": rejects,
            "catalog_refresh": catalog_refresh,
            "commands": commands,
        }
        write(out / "results.json", result)
    finally:
        run_logged(stop, out, "post-stop", cwd=WORKSPACE, timeout=120)

    record = {
        "task_id": TASK_ID,
        "status": "PASS",
        "assertions_passed": [
            "int8_nonnull_logged_only",
            "ddl_reject_unsupported",
            "page_header_and_registry",
            "reject_v1_format",
            "key_to_ref_leaf_no_descriptor",
            "stable_packed_slot_encoding",
            "segment_ref_bounds_and_generations",
            "format_build_separate",
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
