#!/bin/bash

# Cascades Test Runner with Coverage
POSTGRES_DIR="/home/qiumc/data/code/cpp/postgres"
TEST_DIR="$POSTGRES_DIR/src/backend/optimizer/cascades/test"
CASCADES_DIR="$POSTGRES_DIR/src/backend/optimizer/cascades"

echo "=========================================="
echo "Cascades Optimizer Test Suite Runner"
echo "=========================================="
echo ""

# NOTE: NOT cleaning gcda files to preserve coverage data

# Test files
TEST_FILES=(
    "coverage_3md_stage_a.sql"
    "coverage_80pct.sql"
    "coverage_gcov_precision.sql"
    "coverage_selfjoin_subquery.sql"
    "cascades_coverage_extension.sql"
)

echo "Running Test Suites..."
echo "=========================================="
echo ""

for test_file in "${TEST_FILES[@]}"; do
    echo "Running: $test_file"
    output_file="/tmp/test_${test_file}.out"
    
    timeout 120 psql -d postgres -f "$TEST_DIR/$test_file" > "$output_file" 2>&1 || true
    
    echo "  → Output: $output_file"
    echo ""
done

echo "=========================================="
echo "Test Results Summary"
echo "=========================================="
echo ""

total_queries=0
total_success=0
total_errors=0

for test_file in "${TEST_FILES[@]}"; do
    output_file="/tmp/test_${test_file}.out"
    
    query_count=$(grep -c "^SELECT\|^EXPLAIN" "$TEST_DIR/$test_file" || echo "0")
    success_count=$(grep -c "^(" "$output_file" || echo "0")
    error_count=$(grep -c "^ERROR:" "$output_file" || echo "0")
    
    total_queries=$((total_queries + query_count))
    total_success=$((total_success + success_count))
    total_errors=$((total_errors + error_count))
    
    echo "$test_file:"
    echo "  Queries: $query_count, Success: $success_count, Errors: $error_count"
done

echo ""
echo "----------------------------------------"
echo "TOTAL:"
echo "  Total queries: $total_queries"
echo "  Success (cascade): $total_success"  
echo "  Errors: $total_errors"
echo "  Fallback (est): $((total_queries - total_success - total_errors))"
echo "----------------------------------------"
echo ""

echo "=========================================="
echo "Coverage Report"
echo "=========================================="
echo ""

cd "$CASCADES_DIR"

FILES=(
    "core/memo.c"
    "core/rewrite.c"
    "core/planbuild.c"
    "core/task.c"
    "modules/join.c"
    "modules/scan.c"
    "modules/filter.c"
    "modules/project.c"
)

printf "%-35s %10s\n" "File" "Coverage"
echo "---------------------------------------------"

for file in "${FILES[@]}"; do
    if [ -f "$file" ]; then
        coverage=$(gcov -n "$file" 2>/dev/null | grep "Lines executed" | head -1 | grep -oP '\d+\.\d+(?=%)' || echo "N/A")
        printf "%-35s %9s%%\n" "$file" "$coverage"
    fi
done

echo ""
echo "=========================================="
echo "Test Complete!"
echo "=========================================="
