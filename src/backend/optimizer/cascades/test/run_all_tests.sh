#!/bin/bash

# Cascades Optimizer Test Suite Runner
# This script runs all 5 test suites and collects coverage statistics

set -e

POSTGRES_DIR="/home/qiumc/data/code/cpp/postgres"
TEST_DIR="$POSTGRES_DIR/src/backend/optimizer/cascades/test"
CASCADES_DIR="$POSTGRES_DIR/src/backend/optimizer/cascades"

echo "=========================================="
echo "Cascades Optimizer Test Suite Runner"
echo "=========================================="
echo ""

# Clean previous gcov data
echo "Cleaning previous coverage data..."
find "$CASCADES_DIR" -name "*.gcda" -delete
find "$CASCADES_DIR" -name "*.gcov" -delete

# Array of test files
TEST_FILES=(
    "coverage_3md_stage_a.sql"
    "coverage_80pct.sql"
    "coverage_gcov_precision.sql"
    "coverage_selfjoin_subquery.sql"
    "cascades_coverage_extension.sql"
)

# Run each test suite
echo "=========================================="
echo "Running Test Suites..."
echo "=========================================="
echo ""

for test_file in "${TEST_FILES[@]}"; do
    echo "Running: $test_file"
    psql -d postgres -f "$TEST_DIR/$test_file" > "/tmp/test_${test_file}.out" 2>&1
    echo "  → Output saved to /tmp/test_${test_file}.out"
    echo ""
done

echo "=========================================="
echo "Analyzing Test Results..."
echo "=========================================="
echo ""

# Analyze test results
total_cascade=0
total_fallback=0
total_fail=0

for test_file in "${TEST_FILES[@]}"; do
    output_file="/tmp/test_${test_file}.out"

    # Count different outcomes
    cascade_count=$(grep -c "cascades_planner_used.*true" "$output_file" 2>/dev/null || echo 0)
    fallback_count=$(grep -c "cascades_planner_used.*false" "$output_file" 2>/dev/null || echo 0)
    fail_count=$(grep -c "ERROR:" "$output_file" 2>/dev/null || echo 0)

    # Try alternative patterns
    if [ "$cascade_count" -eq 0 ]; then
        cascade_count=$(grep -c "Cascades" "$output_file" 2>/dev/null || echo 0)
    fi

    echo "$test_file:"
    echo "  Cascade: $cascade_count"
    echo "  Fallback: $fallback_count"
    echo "  Fail: $fail_count"
    echo ""

    total_cascade=$((total_cascade + cascade_count))
    total_fallback=$((total_fallback + fallback_count))
    total_fail=$((total_fail + fail_count))
done

echo "=========================================="
echo "Total Statistics:"
echo "=========================================="
echo "  Total Cascade executions: $total_cascade"
echo "  Total Fallbacks: $total_fallback"
echo "  Total Failures: $total_fail"
echo ""

# Generate coverage report
echo "=========================================="
echo "Generating Coverage Report..."
echo "=========================================="
echo ""

cd "$CASCADES_DIR"

# Core files to check
CORE_FILES=(
    "core/memo.c"
    "core/rewrite.c"
    "core/planbuild.c"
    "core/task.c"
    "modules/join.c"
    "modules/scan.c"
    "modules/filter.c"
    "modules/project.c"
)

echo "File Coverage:"
echo "----------------------------------------"

for file in "${CORE_FILES[@]}"; do
    if [ -f "$file" ]; then
        gcov_output=$(gcov "$file" 2>/dev/null || echo "")
        coverage=$(echo "$gcov_output" | grep -oP '\d+\.\d+%' | head -1 || echo "N/A")
        printf "%-30s %s\n" "$file" "$coverage"
    fi
done

echo ""
echo "=========================================="
echo "Test Suite Execution Complete!"
echo "=========================================="
