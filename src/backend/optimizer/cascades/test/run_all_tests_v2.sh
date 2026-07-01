#!/bin/bash

# Cascades Optimizer Test Suite Runner v2
# Runs all 5 test suites and collects detailed statistics

set -e

POSTGRES_DIR="/home/qiumc/data/code/cpp/postgres"
TEST_DIR="$POSTGRES_DIR/src/backend/optimizer/cascades/test"
CASCADES_DIR="$POSTGRES_DIR/src/backend/optimizer/cascades"

echo "=========================================="
echo "Cascades Optimizer Test Suite Runner v2"
echo "=========================================="
echo ""

# Clean previous gcov data
echo "Cleaning previous coverage data..."
find "$CASCADES_DIR" -name "*.gcda" -delete 2>/dev/null || true
find "$CASCADES_DIR" -name "*.gcov" -delete 2>/dev/null || true

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
    output_file="/tmp/test_${test_file}.out"

    # Run test and capture output
    psql -d postgres -f "$TEST_DIR/$test_file" > "$output_file" 2>&1 || true

    # Count queries executed
    query_count=$(grep -c "^SELECT\|^EXPLAIN" "$TEST_DIR/$test_file" 2>/dev/null || echo 0)
    echo "  → Queries: $query_count"
    echo "  → Output: $output_file"
    echo ""
done

echo "=========================================="
echo "Analyzing Test Results..."
echo "=========================================="
echo ""

# Analyze test results
total_success=0
total_fallback=0
total_error=0
total_queries=0

for test_file in "${TEST_FILES[@]}"; do
    output_file="/tmp/test_${test_file}.out"

    # Count different outcomes
    success_count=$(grep -c "^(" "$output_file" 2>/dev/null || echo 0)
    error_count=$(grep -c "^ERROR:" "$output_file" 2>/dev/null || echo 0)
    connection_lost=$(grep -c "connection to server was lost" "$output_file" 2>/dev/null || echo 0)

    # Count queries in test file
    query_count=$(grep -c "^SELECT\|^EXPLAIN" "$TEST_DIR/$test_file" 2>/dev/null || echo 0)

    echo "$test_file:"
    echo "  Total queries: $query_count"
    echo "  Successful: $success_count"
    echo "  Errors: $error_count"
    echo "  Connection lost: $connection_lost"

    # Estimate fallback (queries - success - errors)
    fallback_est=$((query_count - success_count - error_count))
    if [ $fallback_est -lt 0 ]; then
        fallback_est=0
    fi
    echo "  Est. Fallback: $fallback_est"
    echo ""

    total_queries=$((total_queries + query_count))
    total_success=$((total_success + success_count))
    total_error=$((total_error + error_count))
    total_fallback=$((total_fallback + fallback_est))
done

echo "=========================================="
echo "Total Statistics:"
echo "=========================================="
echo "  Total queries: $total_queries"
echo "  Cascade success: $total_success"
echo "  Fallback: $total_fallback"
echo "  Errors: $total_error"
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
    "core/property.c"
    "modules/join.c"
    "modules/scan.c"
    "modules/filter.c"
    "modules/project.c"
)

echo "Coverage by File:"
echo "----------------------------------------"
printf "%-40s %10s\n" "File" "Coverage"
echo "----------------------------------------"

total_files=0
total_coverage=0

for file in "${CORE_FILES[@]}"; do
    if [ -f "$file" ]; then
        # Run gcov and capture output
        gcov_out=$(gcov -n "$file" 2>/dev/null | grep "Lines executed" || echo "Lines executed:0.00%")

        # Extract coverage percentage
        coverage=$(echo "$gcov_out" | grep -oP '\d+\.\d+(?=%)' || echo "0.00")

        printf "%-40s %9s%%\n" "$file" "$coverage"

        # Add to totals
        total_files=$((total_files + 1))
        total_coverage=$(echo "$total_coverage + $coverage" | bc)
    fi
done

echo "----------------------------------------"

# Calculate average coverage
if [ $total_files -gt 0 ]; then
    avg_coverage=$(echo "scale=2; $total_coverage / $total_files" | bc)
    printf "%-40s %9s%%\n" "Average Coverage" "$avg_coverage"
fi

echo ""
echo "=========================================="
echo "Test Suite Execution Complete!"
echo "=========================================="
