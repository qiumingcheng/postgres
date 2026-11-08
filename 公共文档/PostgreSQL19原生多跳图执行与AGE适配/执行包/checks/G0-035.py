#!/usr/bin/env python3
"""Verify the reviewed PostgreSQL 19 AGE baseline adaptations."""
from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys

SCRIPTS = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPTS))

from common import PACKAGE, WORKSPACE, read, sha  # noqa: E402


TASK_ID = "G0-035"
AGE = WORKSPACE / ".worktrees/pg19-graph-core/apache-age-pg19-port"
REVIEW = PACKAGE / "golden/G0-035/diff_review.json"
REVIEWED_TESTS = {
    "expr",
    "cypher_match",
    "cypher_call",
    "jsonb_operators",
    "subgraph",
    "age_upgrade",
}


def git(*args: str) -> str:
    return subprocess.check_output(
        ["git", *args], cwd=AGE, text=True, stderr=subprocess.STDOUT
    ).strip()


def verify_review() -> dict:
    review = read(REVIEW)
    entries = review.get("reviews", [])
    if review.get("task_id") != TASK_ID:
        raise RuntimeError("wrong review task id")
    if {entry.get("test") for entry in entries} != REVIEWED_TESTS:
        raise RuntimeError("the six baseline differences were not reviewed exactly once")
    if any(entry.get("status") != "RESOLVED" for entry in entries):
        raise RuntimeError("a reviewed baseline difference remains unresolved")
    if any(not entry.get("classification") or not entry.get("reason") for entry in entries):
        raise RuntimeError("review classification/reason is incomplete")

    source_lock = read(PACKAGE / "specs/source_lock.json")
    locked_status = source_lock["sources"]["apache-age-pg19-port"]["status"]
    locked_paths = {
        line[3:] for line in locked_status.splitlines() if line.startswith(" M ")
    }
    reviewed_paths = set(review.get("modified_paths", []))
    actual_paths = set(filter(None, git("diff", "--name-only", "HEAD", "--").splitlines()))
    if actual_paths != locked_paths | reviewed_paths:
        raise RuntimeError(
            "candidate AGE changes are not exactly baseline port + reviewed adaptations: "
            + repr(sorted(actual_paths ^ (locked_paths | reviewed_paths)))
        )

    expected_paths = {
        path for path in reviewed_paths if path.startswith("regress/expected/")
    }
    if expected_paths != set(review.get("reviewed_expected_paths", [])):
        raise RuntimeError("an expected-output change is missing from the review ledger")
    if any(path.startswith("regress/results/") for path in actual_paths):
        raise RuntimeError("regression results were copied into tracked files")

    for item in review.get("file_digests", []):
        relative = item["path"]
        current = AGE / relative
        if sha(current) != item["reviewed_sha256"]:
            raise RuntimeError("reviewed file changed after audit: " + relative)
        baseline = subprocess.check_output(
            ["git", "show", "HEAD:" + relative], cwd=AGE
        )
        import hashlib

        if hashlib.sha256(baseline).hexdigest() != item["baseline_sha256"]:
            raise RuntimeError("review baseline digest mismatch: " + relative)

    history_commit = git("log", "-1", "--format=%H", "--", "age.control")
    history = review.get("upgrade_history", {})
    if history_commit != history.get("version_commit"):
        raise RuntimeError("upgrade test is not using the reviewed version-bump commit")
    if int(git("rev-list", "--count", "HEAD")) < history.get("minimum_commits", 2):
        raise RuntimeError("AGE history is too shallow for a meaningful upgrade test")
    return review


def run_regression() -> Path:
    before = set((PACKAGE / "evidence/runs").glob("*-regress-candidate-age"))
    command = [
        sys.executable,
        str(PACKAGE / "scripts/lab.py"),
        "regress",
        "--variant=candidate",
        "--suite=age",
    ]
    result = subprocess.run(command, cwd=WORKSPACE, text=True)
    if result.returncode:
        raise RuntimeError("candidate AGE regression failed")
    after = set((PACKAGE / "evidence/runs").glob("*-regress-candidate-age"))
    created = sorted(after - before)
    if len(created) != 1:
        raise RuntimeError("could not identify the regression evidence directory")
    run = created[0]
    summary = read(run / "summary.json")
    if summary.get("status") != "PASS" or summary.get("variant") != "candidate":
        raise RuntimeError("candidate regression receipt is not PASS")
    output = (run / "regression.log").read_text(errors="replace")
    if "1..43" not in output or "All 43 tests passed." not in output:
        raise RuntimeError("the complete 43-case AGE suite did not pass")
    if "not ok" in output or (run / "regression.diffs").exists():
        raise RuntimeError("regression reported an unexpected failing test")
    if "Building synthetic initial version install SQL" not in output:
        raise RuntimeError("age_upgrade did not build a historical initial version")
    return run


def evidence(path: Path) -> dict:
    return {"path": str(path.relative_to(PACKAGE)), "sha256": sha(path)}


def main() -> None:
    verify_review()
    subprocess.run(
        [sys.executable, str(PACKAGE / "scripts/oracle.py")],
        cwd=PACKAGE / "scripts",
        check=True,
    )
    run = run_regression()
    record = {
        "task_id": TASK_ID,
        "status": "PASS",
        "assertions_passed": [
            "each_diff_reviewed",
            "no_blind_expected_copy",
            "age_43_cases_pass",
        ],
        "evidence_files": [
            evidence(REVIEW),
            evidence(run / "summary.json"),
            evidence(run / "regression.log"),
            evidence(run / "results/age_upgrade.out"),
            evidence(run / "results/cypher_match.out"),
        ],
    }
    print(json.dumps(record, ensure_ascii=False, sort_keys=True))


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError, ValueError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
