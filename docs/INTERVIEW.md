# Interview and résumé claims

Use this as an evidence-backed starting point; be ready to explain the implementation without notes.

- Built a C++20 transactional table store over immutable object files, with snapshot reads,
  time travel, zero-copy clones, and bounded parallel filter scans.
- Implemented conditional-write commit publication with optimistic concurrency and commit-ID
  resolution of ambiguous failures, verified against MinIO and Azurite.
- Added a checksummed columnar format, ranged reads, conservative file and row-group pruning,
  offline garbage collection across shared clone files, and seeded fault/model tests under sanitizers.

AWS S3 smoke tests ran on the real service and cleaned their temporary bucket. Azure Blob is
verified against Azurite; real Azure execution requires locally configured credentials.
See VALIDATION.md for current sanitizer and remote CI evidence.
Do not claim production-grade, consensus, unqualified exactly-once ingestion, online GC, an SDK
credential chain, constant-time clone creation, or a performance improvement over Snowflake.

Whiteboard these four executions:

1. Two appends start at the same version. One wins the conditional write; the other reads the
   winner and rebases. Explain why there is one publication point and no lost append.
2. A commit lands but its response times out. Read the attempted key and compare ID and bytes.
   Explain why a missing key does not prove a delayed request can no longer succeed.
3. Overwrite races append. The overwrite aborts; compaction instead can preserve unrelated appends.
   Explain why restore needs the strict rule to reproduce its target file set exactly.
4. GC's last root check finishes before a clone publishes a reference. Explain why another scan
   or grace period cannot eliminate this race, and why this release requires offline maintenance.

Also explain buffer ownership, joining workers during exceptions, the little-endian scalar layout,
null and finite-double semantics, defensive footer/chunk bounds, bytewise string statistics,
logical commit timestamps under clock skew, and why checkpoint hints are optional.

Discuss the million-row streaming memory experiment and the shuffled-data pruning overhead
in PERFORMANCE.md. Explain why the buffer budget is not a total RSS cap and why cancellation
cannot preempt synchronous I/O.

Use REVIEW.md's real defects to discuss debugging and regression testing. The benchmark's
contention results also provide a concrete example of the design's limits: correctness does not
imply good throughput with many writers on one table. Partitioning or a metadata service would
be future architectural work, not capabilities of this release.
