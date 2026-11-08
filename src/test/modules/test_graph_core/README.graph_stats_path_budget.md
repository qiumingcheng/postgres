# Fixed-path degree collection memory budget

`collect_degrees` samples the complete collection context before each degree
hash insertion and immediately after it. If the configured limit is crossed,
the current scan and tuple slot are closed and the collector returns unpublished
`PARTIAL` with reason `max_memory_bytes`. The limit is sampled at allocation
boundaries: one dynahash growth can cross it, but collecting the remaining
unique endpoints cannot continue. It is not a hard per-allocation memory cap.
The peak is sampled before scan/slot cleanup and unconditionally by the caller,
including exits caused by the examined-row budget or deadline.

`sql/graph_stats_path.sql` adds 100,000 distinct edge endpoints with one sampled
seed and a 256 KiB collection budget. It requires early memory termination,
retained peak accounting, and no publication. A separate examined-row-budget
case compares the peak after 2,000 degree rows against the seed-only exit. The
expected file states the intended results; these new cases were not run during
the migration repair.

Deadline exit uses the same pre-cleanup peak sampling as the deterministic row
limit case. Timing-specific deadline validation remains a runtime follow-up;
this change does not claim that it was executed or that runtime memory limits
were measured.
