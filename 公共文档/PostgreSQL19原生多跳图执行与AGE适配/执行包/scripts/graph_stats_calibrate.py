#!/usr/bin/env python3
"""Fit non-negative graph cost coefficients from raw timing measurements.

The input keeps training and holdout observations separate and carries the
raw timing samples used to derive one PostgreSQL cost unit.  This tool never
queries a graph, changes a catalog, or promotes incomplete measurements.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import statistics
import sys
import tempfile


COMPONENTS = (
    "batch_startup",
    "key_probe",
    "ref_deref",
    "adj_item",
    "heap_fetch",
    "heap_page_group",
    "binding_extend",
)
METRIC_KINDS = dict(zip(COMPONENTS, range(300, 307)))
ALGORITHM = "nonnegative_coordinate_descent_v1"


def finite_number(value: object, name: str, *, positive: bool = False) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{name} must be numeric")
    result = float(value)
    if not math.isfinite(result) or (positive and result <= 0):
        raise ValueError(f"{name} must be finite" + (" and positive" if positive else ""))
    return result


def canonical_hash(document: object) -> str:
    encoded = json.dumps(document, ensure_ascii=False, sort_keys=True,
                         separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def matrix_rank(matrix: list[list[float]], tolerance: float = 1e-12) -> int:
    work = [row[:] for row in matrix]
    rows = len(work)
    columns = len(work[0]) if rows else 0
    rank = 0
    for column in range(columns):
        pivot = max(range(rank, rows), key=lambda row: abs(work[row][column]),
                    default=rank)
        if rank >= rows or abs(work[pivot][column]) <= tolerance:
            continue
        work[rank], work[pivot] = work[pivot], work[rank]
        scale = work[rank][column]
        work[rank] = [value / scale for value in work[rank]]
        for row in range(rows):
            if row == rank:
                continue
            factor = work[row][column]
            if abs(factor) <= tolerance:
                continue
            work[row] = [left - factor * right
                         for left, right in zip(work[row], work[rank])]
        rank += 1
        if rank == rows:
            break
    return rank


def observations(document: dict, split: str, us_per_cost_unit: float
                 ) -> tuple[list[str], list[list[float]], list[float], list[float]]:
    raw = document.get(split)
    if not isinstance(raw, list) or not raw:
        raise ValueError(f"{split} must be a non-empty array")
    identifiers: list[str] = []
    matrix: list[list[float]] = []
    targets: list[float] = []
    elapsed_values: list[float] = []
    for position, item in enumerate(raw):
        prefix = f"{split}[{position}]"
        if not isinstance(item, dict):
            raise ValueError(f"{prefix} must be an object")
        identifier = item.get("id")
        if not isinstance(identifier, str) or not identifier:
            raise ValueError(f"{prefix}.id must be a non-empty string")
        if identifier in identifiers:
            raise ValueError(f"duplicate {split} id: {identifier}")
        if item.get("complete") is not True or item.get("censored") is not False:
            raise ValueError(f"{prefix} is not a complete uncensored measurement")
        work = item.get("work")
        if not isinstance(work, dict) or set(work) != set(COMPONENTS):
            raise ValueError(f"{prefix}.work must contain exactly the seven cost components")
        row = [finite_number(work[name], f"{prefix}.work.{name}")
               for name in COMPONENTS]
        if any(value < 0 for value in row) or not any(value > 0 for value in row):
            raise ValueError(f"{prefix}.work must be non-negative and non-empty")
        elapsed = finite_number(item.get("elapsed_us"), f"{prefix}.elapsed_us",
                                positive=True)
        identifiers.append(identifier)
        matrix.append(row)
        elapsed_values.append(elapsed)
        targets.append(elapsed / us_per_cost_unit)
    return identifiers, matrix, targets, elapsed_values


def nnls_coordinate_descent(matrix: list[list[float]], targets: list[float],
                            tolerance: float = 1e-12,
                            max_iterations: int = 100000
                            ) -> tuple[list[float], int, bool]:
    columns = len(COMPONENTS)
    coefficients = [0.0] * columns
    predictions = [0.0] * len(matrix)
    squared_norms = [sum(row[column] ** 2 for row in matrix)
                     for column in range(columns)]
    if any(norm <= 0 for norm in squared_norms):
        raise ValueError("training matrix has an unobserved cost component")
    for iteration in range(1, max_iterations + 1):
        maximum_change = 0.0
        maximum_value = 1.0
        for column in range(columns):
            old = coefficients[column]
            numerator = 0.0
            for row_index, row in enumerate(matrix):
                residual_without_column = (targets[row_index] -
                                           predictions[row_index] +
                                           row[column] * old)
                numerator += row[column] * residual_without_column
            new = max(0.0, numerator / squared_norms[column])
            delta = new - old
            if delta:
                coefficients[column] = new
                for row_index, row in enumerate(matrix):
                    predictions[row_index] += row[column] * delta
            maximum_change = max(maximum_change, abs(delta))
            maximum_value = max(maximum_value, abs(new))
        if maximum_change <= tolerance * maximum_value:
            return coefficients, iteration, True
    return coefficients, max_iterations, False


def error_report(identifiers: list[str], matrix: list[list[float]],
                 targets: list[float], elapsed_values: list[float],
                 coefficients: list[float], us_per_cost_unit: float) -> dict:
    cases = []
    squared = 0.0
    absolute = 0.0
    relative = 0.0
    maximum_relative = 0.0
    maximum_ratio = 1.0
    for identifier, row, actual, elapsed in zip(
            identifiers, matrix, targets, elapsed_values):
        predicted = sum(value * coefficient
                        for value, coefficient in zip(row, coefficients))
        difference = predicted - actual
        relative_error = abs(difference) / actual
        ratio = max(predicted / actual, actual / predicted) if predicted > 0 else math.inf
        squared += difference * difference
        absolute += abs(difference)
        relative += relative_error
        maximum_relative = max(maximum_relative, relative_error)
        maximum_ratio = max(maximum_ratio, ratio)
        cases.append({
            "id": identifier,
            "actual_cost_units": actual,
            "predicted_cost_units": predicted,
            "actual_elapsed_us": elapsed,
            "predicted_elapsed_us": predicted * us_per_cost_unit,
            "absolute_error_cost_units": abs(difference),
            "relative_error": relative_error,
            "ratio_error": ratio,
        })
    count = len(cases)
    return {
        "case_count": count,
        "rmse_cost_units": math.sqrt(squared / count),
        "mae_cost_units": absolute / count,
        "mean_relative_error": relative / count,
        "max_relative_error": maximum_relative,
        "max_ratio_error": maximum_ratio,
        "cases": cases,
    }


def calibrate(document: dict) -> dict:
    if document.get("format_version") != 1 or document.get("profile_version") != 1:
        raise ValueError("unsupported calibration or cost-profile version")
    if document.get("components") != list(COMPONENTS):
        raise ValueError("components must use the canonical cost-profile order")
    environment = document.get("environment")
    required_environment = {"postgres_build_id", "catalog_version", "machine_id"}
    if not isinstance(environment, dict) or not required_environment <= set(environment):
        raise ValueError("environment is missing a build, catalog, or machine identity")
    pg_costs = document.get("pg_costs")
    if not isinstance(pg_costs, dict):
        raise ValueError("pg_costs must be an object")
    for name in ("cpu_tuple_cost", "cpu_operator_cost", "random_page_cost"):
        finite_number(pg_costs.get(name), f"pg_costs.{name}", positive=True)

    normalization = document.get("normalization")
    if not isinstance(normalization, dict) or normalization.get("aggregation") != "median":
        raise ValueError("normalization must request median aggregation")
    probe_units = finite_number(normalization.get("probe_pg_cost_units"),
                                "normalization.probe_pg_cost_units", positive=True)
    samples = normalization.get("elapsed_us_samples")
    if not isinstance(samples, list) or len(samples) < 3 or len(samples) % 2 == 0:
        raise ValueError("normalization requires an odd count of at least three timing samples")
    normalized_samples = [finite_number(value, "normalization.elapsed_us_samples",
                                        positive=True) for value in samples]
    median_elapsed_us = statistics.median(normalized_samples)
    us_per_cost_unit = median_elapsed_us / probe_units

    train_ids, train_matrix, train_targets, train_elapsed = observations(
        document, "train", us_per_cost_unit)
    holdout_ids, holdout_matrix, holdout_targets, holdout_elapsed = observations(
        document, "holdout", us_per_cost_unit)
    overlap = sorted(set(train_ids) & set(holdout_ids))
    if overlap:
        raise ValueError("training and holdout ids overlap: " + ",".join(overlap))
    rank = matrix_rank(train_matrix)
    if rank != len(COMPONENTS):
        raise ValueError(f"training matrix rank {rank} does not identify all cost components")
    coefficients, iterations, converged = nnls_coordinate_descent(
        train_matrix, train_targets)
    if not converged or any(value < 0 or not math.isfinite(value)
                            for value in coefficients):
        raise ValueError("non-negative cost fit did not converge")

    train_report = error_report(train_ids, train_matrix, train_targets,
                                train_elapsed, coefficients, us_per_cost_unit)
    holdout_report = error_report(holdout_ids, holdout_matrix, holdout_targets,
                                  holdout_elapsed, coefficients, us_per_cost_unit)
    input_hash = canonical_hash(document)
    measurement_window = document.get("measurement_window")
    if not isinstance(measurement_window, dict) or not isinstance(
            measurement_window.get("started_at"), str) or not isinstance(
            measurement_window.get("finished_at"), str):
        raise ValueError("measurement_window requires started_at and finished_at")
    metadata = {
        "started_at": measurement_window["started_at"],
        "finished_at": measurement_window["finished_at"],
        "snapshot_source": "offline_cost_calibration",
        "sample_method": ALGORITHM,
        "truncation_reason": "none",
        "seed": 0,
        "population": len(train_ids),
        "sample_count": len(train_ids),
        "effective_sample_size": len(train_ids),
        "complete_depth": 0,
        "layout_version": 1,
        "model_version": 1,
        "change_baseline": {
            "input_sha256": input_hash,
            "postgres_build_id": environment["postgres_build_id"],
            "catalog_version": environment["catalog_version"],
            "machine_id": environment["machine_id"],
        },
        "resources": {
            "scan_pages": 0,
            "examined_entries": len(train_ids),
            "peak_bytes": 0,
        },
    }
    coefficient_map = dict(zip(COMPONENTS, coefficients))
    profiles = []
    for component in COMPONENTS:
        profiles.append({
            "metric_kind": METRIC_KINDS[component],
            "profile_key": {"cost_profile_version": 1, "component": component},
            "metadata": metadata,
            "estimate": {
                "kind": 1,
                "state": 1,
                "sample_count": len(train_ids),
                "p95_known": False,
                "uncertainty_known": False,
                "censored": False,
                "expected": coefficient_map[component],
                "workload_p95": None,
                "uncertainty_low": None,
                "uncertainty_high": None,
            },
        })
    return {
        "format_version": 1,
        "cost_profile_version": 1,
        "algorithm": ALGORITHM,
        "input_sha256": input_hash,
        "environment": environment,
        "pg_costs": pg_costs,
        "normalization": {
            "probe_pg_cost_units": probe_units,
            "elapsed_us_sample_count": len(normalized_samples),
            "median_elapsed_us": median_elapsed_us,
            "us_per_pg_cost_unit": us_per_cost_unit,
        },
        "fit": {
            "constraint": "coefficient >= 0",
            "matrix_rank": rank,
            "iterations": iterations,
            "converged": converged,
            "coefficients_pg_cost_units": coefficient_map,
            "train_error_report": train_report,
        },
        "holdout_estimate_error_report": holdout_report,
        "profiles": profiles,
    }


def write_json(path: Path, document: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile("w", encoding="utf-8", dir=path.parent,
                                     delete=False) as stream:
        json.dump(document, stream, ensure_ascii=False, sort_keys=True, indent=2,
                  allow_nan=False)
        stream.write("\n")
        temporary = Path(stream.name)
    temporary.replace(path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    arguments = parser.parse_args()
    try:
        source = json.loads(arguments.input.read_text(encoding="utf-8"))
        if not isinstance(source, dict):
            raise ValueError("calibration input must be a JSON object")
        write_json(arguments.output, calibrate(source))
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(str(error), file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
