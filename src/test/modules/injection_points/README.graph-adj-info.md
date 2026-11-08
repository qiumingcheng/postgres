# graph_adj diagnostic snapshot regression

`graph-adj-info.spec` exercises a concurrent INSERT while
`graph_adj_index_info` has released its owner-Slot lock to validate copied
opposite references. The `graph-adj-info-direction-copied` injection point
makes the interleaving deterministic. The injection is local to the reader;
the writer remains able to insert and compact the same endpoint.

The fixture starts with eight inline entries and a full eight-entry active
segment per direction. In the first permutation, INSERT seals that active
segment and publishes a base plus a new active. In the second, eight earlier
inserts establish a base and another full active, so the concurrent INSERT
also replaces the base. The diagnostic must finish without an index-corruption
error. After it finishes, both directions must contain all 17 or 25 entries,
with 16 or 32 base entries in total, respectively.

The implementation copies inline, base, sealed, and active candidates for one
direction while holding its owner-Slot shared lock continuously. Compaction
needs the exclusive Slot lock before changing active/sealed identity or
publishing a descriptor. Segment reads follow the existing Slot-to-data-page
lock order. Opposite-Slot validation runs only after releasing all those locks;
it uses the copied candidates, not retained data references. No advisory lock
is added and no retry loop is needed. Ordinary graph execution providers are
unchanged. The diagnostic remains a per-direction observation, not a promise
of a transaction-wide physical snapshot during concurrent insertion.

The test is registered in the injection_points module's Makefile and Meson
isolation lists and requires a PostgreSQL build with injection points enabled.
Test source and expected output were authored and statically reviewed during
the migration repair. They were **not executed**, and no build was performed.
