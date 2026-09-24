# Implemented scope

The original build spec was revised before implementation to make its guarantees implementable.
The authoritative final contracts are in DESIGN.md.

| Original item | Implementation |
|---|---|
| C++20 library and CLI | CMake, public headers, small format/log/table/store modules |
| AWS/Azure SDK adapters | Small libcurl REST adapters; SigV4, Azure Shared Key/bearer auth |
| Emulators | Pinned upstream-source MinIO Docker build; pinned Azurite |
| Conditional commit | Immutable numbered put-if-absent log; bounded ambiguity resolution |
| Snapshot isolation | Snapshot-consistent scans; explicit per-operation file conflict rules |
| Overwrite/restore rebase | Strict conflict on any concurrent commit |
| Durable retention | Ordered expire-before log action; current snapshot always retained |
| Online two-phase GC | Two-phase **offline** GC; no online safety claim |
| Reader lifetime during GC | All readers stopped during destructive GC |
| Catalog registration | Before publication; resumable incomplete creation; fail-closed GC |
| Format | LSF v1: explicit bounds, nulls, scalar encoding, footer/chunk CRC32C |
| Pruning | File and row-group min/max, bytewise string comparison, projection |
| Parallelism | Bounded joining workers; parallel uploads and streaming scans with shared budget/backpressure/cancellation |
| Checkpoints and compaction | Implemented optional features |
| Failure tests | Deterministic faults, races, cross-process local writers, crash cutpoints |
| Model/property tests | Seeded table operations + pruned/full equivalence |
| Sanitizers and CI | Configured jobs; actual verification tracked separately |
| Benchmarks | Repeated sorted/shuffled/skewed local scans, million-row RSS, CRC reference, memory contention/startup |
| Real cloud and GitHub publication | Real S3 smoke run passed; public repository and CI; real Azure awaits credentials |
| LZ4/multipart/sharing/Prometheus | Optional Tier 2, deferred |

The narrower GC contract replaces unsafe guarantees, not just their wording. Grace alone does
not exclude a paused client or reference publication after the last root check. An online-GC
protocol is a separate future project. CLI --offline records the caller's assertion that the
namespace is quiescent; it does not create a distributed lock.
