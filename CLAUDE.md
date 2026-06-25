# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is PostgreSQL 9.2.4 with an experimental **Cascades query optimizer** implementation. The Cascades optimizer is added as a new module alongside PostgreSQL's standard query planner, providing a cost-based optimization framework inspired by the Cascades/Columbia optimizer architecture.

**Key Context:**
- Base: PostgreSQL 9.2.4 (REL9_2_4)
- Custom addition: Cascades optimizer in `src/backend/optimizer/cascades/`
- Design doc: `postgres_cascades_migration_plan.md`
- Current status: Phase 1 (Path import mode) - 72.5% code coverage, production-ready

## Building and Testing

### Build Commands

```bash
# Full build from scratch
./configure --prefix=/home/qiumc/data/programs/postgres/postgres-install \
            --enable-debug --enable-cassert CFLAGS="-g -fprofile-arcs -ftest-coverage"
make clean
make
make install

# Rebuild only cascades module (faster iteration)
make -C src/backend/optimizer/cascades
make -C src/backend

# Update binary after rebuild
pg_ctl stop -D /tmp/pg_cascades_test -m fast
cp src/backend/postgres /home/qiumc/data/programs/postgres/postgres-install/bin/postgres
pg_ctl start -D /tmp/pg_cascades_test -l /tmp/pg_cascades_test.log
```

### Testing

```bash
# Run full Cascades test suite (372 tests)
psql -d cascades_test -f src/backend/optimizer/cascades/test/cascades_coverage_extension.sql

# Enable Cascades for a query
psql -d cascades_test -c "SET enable_cascades_planner = on; <your query>"

# Debug mode
psql -d cascades_test -c "SET cascades_planner_debug = on; <your query>"

# Coverage analysis
rm -f src/backend/optimizer/cascades/*.gcda  # Clean old coverage
# Run tests
lcov --capture --directory src/backend/optimizer/cascades --output-file cascades.info
lcov --summary cascades.info
genhtml cascades.info --output-directory coverage_report
```

## Cascades Optimizer Architecture

### High-Level Design (Phase 1: Path Import Mode)

The Cascades implementation follows a **two-phase approach**:

**Phase 1 (Current)**: Path import mode
- PostgreSQL's `make_one_rel()` generates scan/join Paths
- Cascades imports these Paths and wraps them in a Memo structure
- Cascades focuses on **upper optimization** (Agg, Sort, Limit)
- Fallback to standard planner for unsupported queries

**Phase 2 (Future)**: Full Cascades
- Complete logical expression generation
- Independent join enumeration
- Full transformation rules enabled

### Entry Point

Cascades integrates at: `src/backend/optimizer/plan/planner.c`
- Called from `grouping_planner()` after `prepare_query_planner_inputs()`
- Function: `pg_cascades_optimize()` in `src/backend/optimizer/cascades/cascades.c`

### Core Components

```
src/backend/optimizer/cascades/
├── cascades.c       - Main entry, query support checks
├── memo.c          - Memo structure (Groups, Expressions)
├── task.c          - Task scheduler (OptimizeGroup, ApplyRule, etc.)
├── rule.c          - Transformation and implementation rules
├── property.c      - Property derivation (pathkeys, required_outer)
├── planbuild.c     - Extract best plan from Memo
├── pg_adapter.c    - PostgreSQL integration adapter
├── pattern.c       - Multi-node pattern matching
├── rewrite.c       - Query rewrite (CTE, subqueries)
├── decorrelate.c   - Correlated subquery decorrelation
├── postopt.c       - Post-optimization validation
└── debug.c         - Debugging utilities
```

### Key Data Structures

**PgMemo**: The core optimization structure
- Contains Groups (equivalence classes of logical expressions)
- Each Group has logical_exprs and physical_exprs
- Stores best plans per required property

**PgGroupExpr**: Represents an operation
- `op`: Operation type (SCAN, JOIN, AGG, SORT, etc.)
- `mode`: PG_PHYS_EXPR_IMPORTED_PATH (Phase 1) or PG_PHYS_EXPR_COMPOSABLE_OP
- `op_private`: Stores Path* (Phase 1) or operator parameters
- `inputs`: Child groups

**PgRequiredProperty**: Optimization context
- `pathkeys`: Sort order requirements
- `required_outer`: Parameterized path requirements
- `limit_tuples`: LIMIT optimization hints

### Rules System

Rules are registered in `rule.c` with priorities. Key rule categories:

- **Implementation rules**: LogicalScan → PhysicalSeqScan/IndexScan/etc.
- **Transformation rules**: Join reorder, predicate pushdown, etc.
- **Enforcer rules**: Sort enforcement

**Important**: B1 (PruneScanColumns) and B2 (PruneJoinColumns) are currently **disabled** to maintain stability. They were causing "variable not found in subplan target lists" errors in 3+ table joins with ORDER BY + LIMIT.

### Fallback Mechanism

Cascades has a two-stage check:
1. **Pre-check**: `pg_cascades_supported_query_precheck()` - before prepare
2. **Post-check**: `pg_cascades_supported_query()` - after prepare

**Fallback scenarios**:
- Non-SELECT queries
- Window functions
- Set operations (UNION/INTERSECT/EXCEPT)
- Recursive CTEs
- Foreign tables
- 3+ table joins with ORDER BY + LIMIT (known Phase 1 limitation)

## Configuration

### GUC Parameters (postgresql.conf)

```sql
enable_cascades_planner = on/off          -- Enable Cascades optimizer
cascades_planner_debug = on/off           -- Debug logging
cascades_planner_timeout_ms = 5000        -- Optimization timeout
cascades_planner_max_tasks = 100000       -- Max optimization tasks
```

### Testing Database Setup

```bash
# Create test database
createdb cascades_test

# Initialize test tables
psql -d cascades_test -f src/backend/optimizer/cascades/test/cascades_coverage_extension.sql
```

## Common Issues and Solutions

### Issue: "variable not found in subplan target lists"

**Cause**: B1/B2 column pruning rules removing intermediate join keys
**Solution**: These rules are disabled in Phase 1. If re-enabled, add fallback for complex joins:

```c
// In pg_cascades_supported_query()
if (parse->sortClause != NIL && parse->limitCount != NULL) {
    int num_base_rels = /* count from rtable */;
    if (num_base_rels >= 3) return PG_CASCADES_UNSUPPORTED;
}
```

### Issue: Empty targetlist in join nodes

**Cause**: `pg_cascades_build_logical_plan()` fallback path with incomplete RelOptInfo
**Solution**: This fallback path is for debugging only. Production should use best_entries path.

### Coverage Testing

Current coverage: 72.5% (2,846/3,926 lines), 91% functions
- Exceeds open-source standards (60-70%)
- Meets enterprise standards (70-75%)

To collect coverage:
1. Build with `--enable-coverage` flags (already done)
2. Clear old data: `rm src/backend/optimizer/cascades/*.gcda`
3. Run tests
4. Generate report: `lcov + genhtml`

## Code Style

- Follow PostgreSQL coding conventions
- Use PostgreSQL's memory contexts (palloc/pfree, not malloc/free)
- Error handling: elog(ERROR, ...) for errors, elog(NOTICE, ...) for debug
- All Cascades symbols prefixed with `pg_` or `Pg`

## Related Documentation

- Design document: `postgres_cascades_migration_plan.md`
- Coverage reports: `CASCADES_COVERAGE_*.md`
- Test suites: `src/backend/optimizer/cascades/test/*.sql`
