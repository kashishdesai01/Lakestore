# Measured performance and limitations

October 7, 2026; Release build, AppleClang 17, macOS arm64, eight logical threads.
[Raw repeated workloads](WORKLOAD_BENCHMARK.json) retain every sample. Each scan runs in a fresh
process over local immutable files; contention/startup/CRC use the in-memory backend.
Timing is wall-clock on a shared developer machine, with other validation work running.
These are reproducible workload examples, not isolated microarchitecture or cloud throughput claims.

## Workload matrix

262,144 rows, 128-byte strings, 32 files, 1,024-row groups. Three repetitions of each of
24 configurations compare sorted, deterministically shuffled, and 90%-zero skewed data,
two selectivities, pruning on/off, and one/four workers. Result counts and fingerprints
must agree across pruning/parallelism choices. Four-worker medians:

| Layout / predicate | Without pruning | With pruning | Column bytes without / with |
|---|---:|---:|---:|
| Sorted, id < 2621 | 236.6 ms | 8.7 ms | 37,224,448 / 436,224 |
| Sorted, all rows | 105.4 ms | 112.0 ms | 37,224,448 / 37,224,448 |
| Shuffled, id < 2621 | 125.8 ms | 117.1 ms | 37,224,448 / 37,224,448 |
| Shuffled, all rows | 134.9 ms | 140.6 ms | 37,224,448 / 37,224,448 |
| Skewed, nonzero | 100.2 ms | 101.8 ms | 37,224,448 / 37,224,448 |
| Skewed, zero | 144.5 ms | 163.7 ms | 37,224,448 / 37,224,448 |

Pruning reduces column reads by 98.8% for the sorted selective example. Shuffling destroys useful
min/max locality; pruning then performs additional checks without avoiding I/O. Skew alone does
not establish locality. The negative results are part of the evidence.

## Streaming memory

A separate 1,048,576-row dataset has 136 MiB of logical scalar/string contents. Three fresh-process
runs scan all rows with four workers and an 8 MiB shared buffer budget:

| Consumer | Median peak RSS | Median time |
|---|---:|---:|
| Streaming checksum, no retained output | 8.4 MiB | 448.0 ms |
| Materialized results | 266.8 MiB | 668.8 ms |

The budget charges row-group data and projection copies conservatively; metadata, allocator
overhead, and consumer-retained output are excluded. RSS is measured independently with getrusage.
A consumer storing every batch still needs memory proportional to results. Slow consumers retain
reservations and stall producers; cancellation stops future work and joins workers.

## One measured optimization

The original per-bit CRC32C loop was replaced with a portable 256-entry lookup table.
The wire checksum remains unchanged; the standard vector and 1,000 randomized binary inputs
are checked against the original implementation. The benchmark retains that reference.

[Before](SCAN_BASELINE.json) and [after](SCAN_OPTIMIZED.json) full-scan samples on the same shuffled
262,144-row data, collected before the Linux metadata-validation portability fix, show median 303.1 ms and 166.5 ms respectively (45.1% lower elapsed time).
Only three samples were collected per implementation on a shared machine; this paired experiment
is indicative and does not justify a general 1.8x throughput claim. The repeated CRC comparison
in WORKLOAD_BENCHMARK.json also records the matching checksum and original/table timings.

Writer contention medians for 20 appends per writer are 5.8 ms (one), 68.0 ms (four), and
1,699.6 ms (sixteen), on MemoryStore. These workloads contain different total commit counts;
compare throughput and conflicts in the raw samples, not raw duration alone. One table's
conditional-write publication point limits many-writer throughput.

## Reproduce

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DLAKESTORE_CLOUD=OFF -DBUILD_TESTING=OFF
cmake --build build-release -j 4
python3 scripts/benchmark.py
```

The original small BENCHMARK.json is historical evidence from the initial implementation;
the current runner and WORKLOAD_BENCHMARK.json supersede its workload presentation.
