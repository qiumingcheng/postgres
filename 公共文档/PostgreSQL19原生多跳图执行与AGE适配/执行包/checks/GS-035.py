#!/usr/bin/env python3
"""Exercise stable refs, directory splits, scoped fallback, and work counts."""
from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys

SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS))

from common import PACKAGE, WORKSPACE, now, run_dir, run_logged, sha, write  # noqa: E402
from lab import sql  # noqa: E402


TASK_ID = "GS-035"


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


def index_info() -> dict:
    return json.loads(
        sql(
            "candidate",
            "SELECT pg_catalog.graph_adj_index_info('gs035.edges_adj'::regclass);",
        ).strip()
    )


def ref_probe(
    key: int,
    handle_index: str,
    build: int,
    domain: str,
    slot_id: int,
    slot_generation: int,
    persistent: bool,
) -> dict:
    statement = (
        "SELECT pg_catalog.graph_adj_debug_ref_probe("
        "'gs035.edges_adj'::regclass,"
        f"{key},({handle_index})::oid,{build},({domain})::oid,"
        f"{slot_id},{slot_generation},{str(persistent).lower()});"
    )
    return json.loads(sql("candidate", statement).strip())


def walk(start: int, hops: int) -> dict:
    return json.loads(
        sql(
            "candidate",
            "SELECT pg_catalog.graph_adj_debug_walk("
            f"'gs035.edges_adj'::regclass,{start},{hops});",
        ).strip()
    )


def evidence(path: Path) -> dict:
    return {"path": str(path.relative_to(PACKAGE)), "sha256": sha(path)}


def main() -> None:
    out = run_dir("check-GS-035")
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
            "DROP SCHEMA IF EXISTS gs035 CASCADE; CREATE SCHEMA gs035; "
            "CREATE TABLE gs035.edges("
            "edge_id bigint PRIMARY KEY,src bigint NOT NULL,dst bigint NOT NULL); "
            "CREATE INDEX edges_adj ON gs035.edges USING graph_adj(src,dst); "
            "INSERT INTO gs035.edges VALUES"
            "(101,10,20),(102,10,30),(103,20,40),"
            "(104,30,40),(105,40,50),(106,50,60);",
        )

        key_resolved = ref_probe(
            10,
            "'gs035.edges_adj'::regclass",
            1,
            "'gs035.edges'::regclass",
            0,
            0,
            False,
        )
        if not key_resolved["found"] or key_resolved["directory_lookups"] != 1:
            raise RuntimeError(f"initial key resolution failed: {key_resolved}")
        direct = ref_probe(
            10,
            "'gs035.edges_adj'::regclass",
            key_resolved["build_generation"],
            "'gs035.edges'::regclass",
            key_resolved["slot_id"],
            key_resolved["slot_generation"],
            False,
        )
        if direct["mode"] != "ref_direct" or direct["directory_lookups"] != 0:
            raise RuntimeError(f"valid ref did not bypass the directory: {direct}")
        if direct["slot_id"] != key_resolved["slot_id"]:
            raise RuntimeError("key and ref resolution reached different Slots")

        sql_paths = sql(
            "candidate",
            "SELECT concat_ws('>',e1.edge_id,e2.edge_id,e3.edge_id,e4.edge_id,e4.dst) "
            "FROM gs035.edges e1 JOIN gs035.edges e2 ON e2.src=e1.dst "
            "JOIN gs035.edges e3 ON e3.src=e2.dst "
            "JOIN gs035.edges e4 ON e4.src=e3.dst WHERE e1.src=10 "
            "ORDER BY 1;",
        ).strip().splitlines()
        if sql_paths != ["101>103>105>106>60", "102>104>105>106>60"]:
            raise RuntimeError(f"four-hop SQL baseline mismatch: {sql_paths}")
        four_hop = walk(10, 4)
        expected_walk = {
            "final_keys": [60, 60],
            "final_binding_count": 2,
            "directory_lookups": 1,
            "ref_direct_lookups": 4,
            "ref_fallbacks": 0,
            "slot_dereferences": 5,
            "physical_candidates": 6,
            "visible_candidates": 6,
        }
        for key, expected in expected_walk.items():
            if four_hop[key] != expected:
                raise RuntimeError(
                    f"stable-ref four-hop {key}: {four_hop[key]} != {expected}"
                )

        # Thirty self edges add thirty endpoints.  Together with the six graph
        # endpoints this forces both directory roots to split below Slot extent 1.
        sql(
            "candidate",
            "INSERT INTO gs035.edges "
            "SELECT 1000+g,1000+g,1000+g FROM generate_series(1,30) g;",
        )
        after_split = index_info()
        if not after_split["directory_split_observed"]:
            raise RuntimeError(f"directory split was not observed: {after_split}")
        if after_split["out_root_kind"] != "DIR_INTERNAL" or after_split["in_root_kind"] != "DIR_INTERNAL":
            raise RuntimeError("split did not publish internal OUT/IN roots")
        if after_split["out_directory_leaf_pages"] < 2 or after_split["in_directory_leaf_pages"] < 2:
            raise RuntimeError("split did not publish multiple direction-local leaves")

        direct_after_split = ref_probe(
            10,
            "'gs035.edges_adj'::regclass",
            key_resolved["build_generation"],
            "'gs035.edges'::regclass",
            key_resolved["slot_id"],
            key_resolved["slot_generation"],
            True,
        )
        key_after_split = ref_probe(
            10,
            "'gs035.edges_adj'::regclass",
            key_resolved["build_generation"],
            "'gs035.edges'::regclass",
            0,
            0,
            False,
        )
        if direct_after_split["slot_id"] != key_resolved["slot_id"]:
            raise RuntimeError("directory split moved a live Endpoint Slot")
        if key_after_split["slot_id"] != key_resolved["slot_id"]:
            raise RuntimeError("post-split key lookup no longer reaches the live Slot")

        cross_scope = ref_probe(
            10,
            "'gs035.edges'::regclass",
            key_resolved["build_generation"] + 1,
            "0::oid",
            key_resolved["slot_id"],
            key_resolved["slot_generation"],
            False,
        )
        if cross_scope["mode"] != "key_fallback" or cross_scope["ref_fallbacks"] != 1:
            raise RuntimeError(f"cross-scope hint did not safely fall back: {cross_scope}")
        if cross_scope["slot_id"] != key_resolved["slot_id"]:
            raise RuntimeError("cross-scope fallback resolved a different canonical Slot")

        try:
            ref_probe(
                10,
                "'gs035.edges_adj'::regclass",
                key_resolved["build_generation"],
                "'gs035.edges'::regclass",
                key_resolved["slot_id"],
                key_resolved["slot_generation"] + 1,
                True,
            )
        except RuntimeError as error:
            persistent_error = str(error)
        else:
            raise RuntimeError("conflicting persistent ref silently fell back by key")
        if "conflicting persistent VertexRef" not in persistent_error:
            raise RuntimeError(f"unexpected persistent ref error: {persistent_error}")

        sql(
            "candidate",
            "INSERT INTO gs035.edges VALUES(9001,9000,9001),(9002,9000,9002); "
            "DELETE FROM gs035.edges WHERE edge_id=9002;",
        )
        physical_logical = walk(9000, 1)
        if physical_logical["physical_candidates"] != 2:
            raise RuntimeError(f"physical candidate count is wrong: {physical_logical}")
        if physical_logical["visible_candidates"] != 1 or physical_logical["final_keys"] != [9001]:
            raise RuntimeError(f"MVCC-visible logical count is wrong: {physical_logical}")
        if physical_logical["tableam_calls"] < physical_logical["physical_candidates"]:
            raise RuntimeError("Table AM calls were conflated with logical output")
        final_info = index_info()
        if final_info["out_physical_entries"] <= final_info["out_visible_entries"]:
            raise RuntimeError("index-wide physical/visible counts were conflated")

        forced_plan = json.loads(
            sql(
                "candidate",
                "SET enable_seqscan=off; SET enable_bitmapscan=off; "
                "EXPLAIN (ANALYZE,BUFFERS,FORMAT JSON) "
                "SELECT edge_id FROM gs035.edges WHERE src=10;",
            ).strip()
        )[0]
        if forced_plan["Plan"]["Node Type"] != "Index Scan" or forced_plan["Plan"].get("Index Name") != "edges_adj":
            raise RuntimeError(f"post-split ordinary scan missed graph_adj: {forced_plan}")

        results = {
            "task_id": TASK_ID,
            "finished": now(),
            "catalog_refresh": catalog_refresh,
            "key_resolution": key_resolved,
            "ref_resolution": direct,
            "four_hop_sql_paths": sql_paths,
            "four_hop_stable_ref_walk": four_hop,
            "after_directory_split": after_split,
            "direct_after_split": direct_after_split,
            "key_after_split": key_after_split,
            "cross_scope_fallback": cross_scope,
            "persistent_ref_conflict_error": persistent_error,
            "physical_logical_counts": physical_logical,
            "final_index_info": final_info,
            "post_split_index_plan": forced_plan,
            "commands": commands,
        }
        write(out / "results.json", results)
    finally:
        run_logged(stop, out, "post-stop", cwd=WORKSPACE, timeout=120)

    record = {
        "task_id": TASK_ID,
        "status": "PASS",
        "assertions_passed": [
            "key_ref_same_results",
            "four_hop_directory_bypass",
            "split_keeps_live_slot",
            "cross_scope_hint_key_fallback",
            "persistent_ref_conflict_error",
            "logical_physical_fetch_counts_separate",
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
