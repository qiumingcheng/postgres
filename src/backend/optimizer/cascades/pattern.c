#include "postgres.h"
#include "optimizer/cascades.h"
#include "nodes/pg_list.h"

PgPattern *pg_pattern_leaf(void) {
    PgPattern *p = (PgPattern *) palloc0(sizeof(PgPattern));
    p->type = PG_PATTERN_LEAF; p->children = NIL; return p;
}
PgPattern *pg_pattern_multi_leaf(void) {
    PgPattern *p = (PgPattern *) palloc0(sizeof(PgPattern));
    p->type = PG_PATTERN_MULTI_LEAF; p->children = NIL; return p;
}
PgPattern *pg_pattern_op(PgCascadesOpKind op) {
    PgPattern *p = (PgPattern *) palloc0(sizeof(PgPattern));
    p->type = PG_PATTERN_OPERATOR; p->op = op; p->children = NIL; return p;
}
PgPattern *pg_pattern_tree(PgCascadesOpKind op, List *children) {
    PgPattern *p = (PgPattern *) palloc0(sizeof(PgPattern));
    p->type = PG_PATTERN_TREE; p->op = op; p->children = children; return p;
}
List *pg_pattern_match_root_only(PgPattern *pattern, PgGroupExpr *root) { return NIL; }
List *pg_pattern_match_full(PgPattern *pattern, PgGroupExpr *root) { return NIL; }
