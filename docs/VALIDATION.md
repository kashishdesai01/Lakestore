# Verification evidence

Executed October 7, 2026. Tests and review are evidence, not proof that no bugs remain.

| Check | Result | Evidence |
|---|---|---|
| Final local CPU/model/property/CLI suite | **75/75 passed** | [raw](validation/final-local-tests.txt) |
| Local TSan concurrency selection | **22/22 passed**, no reports | [raw](validation/final-tsan-tests.txt) |
| Local focused ASan + UBSan after portability fix | **33/33 passed**, no reports | [raw](validation/final-asan-ubsan-regressions.txt) |
| Local full ASan + UBSan before portability fix | **73/73 passed**, no reports | [raw](validation/upgrade-asan-ubsan-tests.txt) |
| MinIO/Azurite suite, normal and ASan + UBSan | **8/8 passed in each run** | [normal](validation/upgrade-emulator-tests.txt), [sanitizers](validation/upgrade-emulator-asan-ubsan-tests.txt) |
| Final Linux GitHub CI | **Passed:** normal and ASan/UBSan 75 CPU + 8 emulator checks each; TSan 20 checks | [run](https://github.com/kashishdesai01/lakestore/actions/runs/37656923236), [record](validation/github-ci.json) |
| Real AWS S3 smoke | **Six checks passed**, cleanup confirmed | [report](REAL_S3.json) |
| Real Azure Blob | **Blocked** on local account/container credentials | Azurite does not establish real-service execution |
| Independent implementation and upgrade reviews | Findings fixed and rechecked; no remaining concrete findings | [review](REVIEW.md) |
| Release build, formatting, scripts and JSON | Passed | AppleClang 17; clang-format 23.1.3; Python compile and JSON parsing; bash -n |
| Repeated workload and memory benchmarks | Executed, matching result fingerprints | [interpretation](PERFORMANCE.md), [raw](WORKLOAD_BENCHMARK.json) |

The final CPU suite has 74 GoogleTest cases and one Python CLI smoke. Cloud conformance runs
four cases per emulator. Correctness coverage includes 12 seeded model sequences of 45 operations,
250 seeded pruning predicates over typed/null data, exact concurrent append unions, ambiguous write
outcomes, independent local CLI processes, and process-death cutpoints around publication.
Streaming regressions cover borrowed output, duplicate projections, early/external cancellation,
slow consumers, serialization, undersized budgets, corruption, exceptions, and pinned snapshots.
CRC checks compare the standard vector and 1,000 randomized binary inputs with the original loop.

Sanitizer recovery is disabled and halt-on-error runtime settings are enabled. System
curl/OpenSSL/XML libraries are not rebuilt with instrumentation; Lakestore code is instrumented.
Local full 73-case ASan and post-fix focused 33-case ASan runs are separate evidence levels.
Remote Linux verification runs the final 75-case suite under normal and ASan/UBSan builds.
The first Linux TSan run caught a libstdc++ locale-cache race during concurrent regex construction.
Explicit ASCII metadata validators and serialized static predicate parsing fixed it; the subsequent
Linux TSan job passed without suppressions. Details are in REVIEW.md.

Local environment: macOS 15.7.9, arm64, AppleClang 17.0.0, eight logical threads. CI uses
Ubuntu 24.04/GCC. MinIO is pinned to RELEASE.2025-10-15T17-29-55Z, Azurite to 3.35.0.
Local containers were isolated to the lakestore-test Compose project and removed after verification.
Real S3 verification used a fresh private bucket and random prefix. It checked append/ranged reads,
pruning, clone divergence, restore, streaming, expiration with typed Expired rejection, offline GC,
and verification of retained clone roots. All created cloud resources were cleaned up.

Benchmarks are local developer-machine measurements, with repeated fresh processes and raw
samples. They measure sorted/shuffled/skewed pruning behavior, a million-row streaming versus
materialized memory experiment, CRC before/after, writer contention, and checkpoint replay.
Metadata, consumer-retained output and allocator overhead are outside the scan buffer budget;
measured RSS is reported separately. No comparison with Snowflake performance is supported.

## Reproduce

Follow README.md's build instructions, then:

```sh
ctest --test-dir build --output-on-failure -E '^cloud$'
docker compose -p lakestore-test -f docker/compose.yml up -d --build
./scripts/emulator-tests.sh build
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ctest --test-dir build-asan --output-on-failure -E '^cloud$'
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  ./scripts/emulator-tests.sh build-asan
TSAN_OPTIONS=halt_on_error=1 ctest --test-dir build-tsan --output-on-failure \
  -R 'Concurrent|Race|Compaction|PinnedReader|OneWinner|Streaming|cli_smoke'
docker compose -p lakestore-test -f docker/compose.yml down
python3 scripts/benchmark.py
# Explicitly use your own test account; this creates and cleans a small private bucket.
python3 scripts/real-cloud-tests.py --provider s3 --profile default --region us-east-1 \
  --binary build/lakestore --report docs/REAL_S3.json
# With Azure account credentials configured locally and an existing test container:
python3 scripts/real-cloud-tests.py --provider azure --container YOUR_TEST_CONTAINER \
  --binary build/lakestore --report docs/REAL_AZURE.json
```

Earlier initial-implementation reports remain in validation/ and BENCHMARK.json as historical evidence.
The public repository is [kashishdesai01/lakestore](https://github.com/kashishdesai01/lakestore).
Optional Tier 2 deferrals are explicit in IMPLEMENTATION_PLAN.md.

Final source commit: `356ca503c4daca920ee5763a258a6bf31101ee8b`. The following documentation-only commit records these results;
its CI is skipped because no executable sources changed.
