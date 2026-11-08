# Graph statistics and role reassignment

Graph statistics contain samples scoped to the collecting role. `REASSIGN OWNED`
invalidates these derived objects through normal dependency deletion when the
owner changes. It does not relabel sampled data as belonging to the destination
role. The destination must register and collect a fresh scope under its own
permissions. A reassignment to the same role is a no-op.

Deletion removes the encoded scope/index entry, all profile generations, and
normal/shared dependencies in the same transaction. It invalidates graph plans
or, for a cost scope, the graph-statistics catalog plan dependency. Deletion uses
`DROP_RESTRICT`: referenced property graphs, tables and indexes survive, and an
existing identical scope owned by the destination is left untouched. Rollback
restores the original statistics through normal catalog MVCC.

`sql/graph_stats_reassign.sql` and `expected/graph_stats_reassign.out` cover a
non-bootstrap superuser cost scope with a published payload, a logical scope
with real relation dependencies, same-role reassignment, destination-scope
collision, dependency/payload cleanup, business-object preservation, removal of
the old role, and fresh publication by the destination role.

These test sources and expected results were added during static review. They
have not been run in the cloud migration environment.
