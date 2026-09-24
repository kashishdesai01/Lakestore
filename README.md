# Lakestore

[![correctness](https://github.com/kashishdesai01/lakestore/actions/workflows/ci.yml/badge.svg)](https://github.com/kashishdesai01/lakestore/actions/workflows/ci.yml)

A C++20 library and CLI for transactional tables on object storage. Immutable columnar files,
a conditional-write commit log, and snapshot metadata support concurrent appends, time travel,
zero-copy clones, and selective filter scans. Includes local filesystem, in-memory, S3, Azure Blob,
and deterministic fault-injection backends.

This is a learning-scale storage system. Its correctness contracts and limits are in
[DESIGN.md](docs/DESIGN.md); verification evidence is in [VALIDATION.md](docs/VALIDATION.md). The independent review and resolved findings are in
[REVIEW.md](docs/REVIEW.md). Evidence-backed résumé wording and whiteboard prompts are in
[INTERVIEW.md](docs/INTERVIEW.md).
There are no claims of production scale or Snowflake-equivalent performance.

## Build and test

Prerequisites: CMake 3.24+, a C++20 compiler, Python 3, libcurl 7.75+, OpenSSL, and libxml2.
JSON and GoogleTest are fetched at pinned versions with SHA-256 checks. Cloud adapters use the
services' REST APIs through libcurl, avoiding two large SDK dependency trees. AWS SigV4 signing
is provided by libcurl; Azure Shared Key signing uses OpenSSL. Azure bearer tokens also work.
Credentials are supplied explicitly through environment variables; this is not an SDK credential chain.

On Ubuntu:

```sh
sudo apt-get install cmake ninja-build g++ python3 libcurl4-openssl-dev libssl-dev libxml2-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j 4
ctest --test-dir build --output-on-failure -E '^cloud$'
```

On macOS, install CMake and the OpenSSL development package, then set
`-DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"` if CMake cannot locate it.
For a dependency-light build, set `-DLAKESTORE_CLOUD=OFF`.
An optional `vcpkg.json` supplies the cloud dependencies when using the vcpkg CMake toolchain.
The implementation targets POSIX filesystems (Linux/macOS); Windows LocalStore is unsupported.

## Try it locally

```sh
export LAKESTORE_STORE=file:///tmp/lakestore-demo
./build/lakestore create --table sales --schema 'id:int64,ts:timestamp,region:string,amount:double'
printf 'id,ts,region,amount\n1,1700000000000,us,12.5\n2,1700000001000,eu,20.0\n' > /tmp/sales.csv
./build/lakestore append --table sales --input /tmp/sales.csv
./build/lakestore scan --table sales --where "region = 'us' AND amount >= 10" --columns id,amount
./build/lakestore clone --source sales --version 2 --as sales_copy
./build/lakestore history --table sales
./build/lakestore verify --table sales
```

CSV input requires a schema-matching header. Quoted commas, escaped double quotes, and multiline
fields are supported. `\N` denotes null, including in quoted fields. Empty strings are distinct
from null. Timestamps and `--as-of` / `--older-than` arguments use epoch **milliseconds**.
Non-finite doubles are rejected. String ordering is bytewise; there is no locale collation.
Predicates support uppercase `AND`, `=`, `<`, `<=`, `>`, `>=`; string literals use single quotes
with doubled quotes for escaping. There is no SQL or OR expression language.
Scans return JSON on stdout; command metrics and errors go to stderr. Row order is unspecified.
Use `./build/lakestore --help` for all commands.

## Object-store emulators

```sh
docker compose -p lakestore-test -f docker/compose.yml up -d --build
./scripts/emulator-tests.sh build
docker compose -p lakestore-test -f docker/compose.yml down
```

If your installation provides `docker-compose` rather than `docker compose`, use that executable.
Ports 19000 and 11000 bind only to localhost. These containers use ephemeral storage and test-only
credentials. MinIO is built from the pinned upstream source release because upstream container
images are no longer reliably available; the first build takes longer. Its AGPL source release is
linked in [DESIGN.md](docs/DESIGN.md). Azurite is pinned to 3.35.0.
The suite checks conditional writes on **both** emulators, pagination beyond 1,000 objects,
concurrent commits, injected ambiguous outcomes, clones, expiration, and offline GC.
`ctest` reports the cloud suite as skipped unless both endpoint variables are set.

For your own S3 bucket, set `AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`, optionally
`AWS_SESSION_TOKEN`, and `AWS_REGION`; use `--store s3://bucket/prefix`.
For Azure set `AZURE_STORAGE_ACCOUNT` and `AZURE_STORAGE_KEY`, or
`AZURE_STORAGE_BEARER_TOKEN`; use `--store azure://container/prefix`.
`LAKESTORE_S3_ENDPOINT` and `LAKESTORE_AZURE_ENDPOINT` override the service endpoints.
`init-store` is an explicit provisioning command. Other commands do not create cloud buckets.
Use primary endpoints with strong read/list consistency; replicas and S3 directory buckets are
outside the supported contract. Credentials never appear in command output.

## Maintenance and retention

Overwrite and restore abort when the table changes during the operation. Appends rebase;
compaction preserves concurrent appends and aborts if an input file was removed.
`expire-snapshots --keep-last K` counts all commit versions, **including the expiration commit**.
Expiration is durable, monotonic, and immediately prevents scans, restores, or clones of expired
versions. It does not delete metadata or files. The current snapshot is always retained.

**GC requires an offline maintenance window across the entire store prefix.** Stop all readers,
writers, clone/restore operations, and other GC processes, including paused clients that might
resume. `--offline` is an operator assertion, not a distributed lock. A grace period alone does
not make online GC safe. Clients may resume between completed passes, but must be stopped again
before sweeping. `--dry-run` never writes or deletes objects and can run online; its estimate may
change under concurrent activity.

```sh
./build/lakestore expire-snapshots --table sales --keep-last 5
./build/lakestore gc --dry-run --grace 1h
# During an offline window, mark candidates:
./build/lakestore gc --offline --grace 1h
# At least one hour later, during another offline window, recheck and sweep:
./build/lakestore gc --offline --grace 1h
```

GC preserves the union of files referenced by every retained snapshot of every registered table.
Deletion manifests persist across restarts, recheck roots, and check object ETags before deleting.
Data keys must never be reused. Audit manifests record completed deletion passes.
There is no table-drop operation and no log/GC-audit metadata reclamation in v1.

## Verification and benchmarks

```sh
cmake -S . -B build-asan -DLAKESTORE_CLOUD=OFF -DLAKESTORE_SANITIZER=address,undefined
cmake --build build-asan -j 4
ctest --test-dir build-asan --output-on-failure
cmake -S . -B build-tsan -DLAKESTORE_CLOUD=OFF -DLAKESTORE_SANITIZER=thread
cmake --build build-tsan -j 4
ctest --test-dir build-tsan --output-on-failure -R 'Concurrent|Race|Compaction|PinnedReader|OneWinner|Streaming|cli_smoke'
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release -j 4
python3 scripts/benchmark.py
```

The repeated benchmark runs sorted, shuffled, and skewed local datasets in fresh processes,
checks result fingerprints, and records pruning, parallelism, writer contention, startup,
CRC throughput, and peak RSS. See [PERFORMANCE.md](docs/PERFORMANCE.md) for results and limits.
Metrics named `requests` count ObjectStore calls; paginated listing can issue multiple HTTP requests.

Use `scan --stream --buffer-mib 8` for JSONL output with a final completion record.
The library `scan_stream` supplies borrowed row-group batches to a serialized callback;
returning false cancels the scan. Slow consumers apply backpressure directly to workers.
Cancellation is cooperative: it cannot interrupt an active callback or synchronous I/O.
Workers join before the API returns or throws. Row ordering is unspecified.
The shared buffer budget conservatively accounts for encoded and decoded row-group data;
it excludes snapshot/footer metadata, allocator overhead, and output retained by consumers.
It is not a process RSS limit. Files are bounded to 512 MiB, chunks to 64 MiB,
footers to 16 MiB, and scan workers to 64. Default scans and CSV inputs still materialize.

Implemented optional features: compaction and checksummed periodic checkpoints. Deferred:
compression, multipart uploads, external sharing, Prometheus export, and online GC. Real-cloud
execution and remote GitHub CI are separate evidence levels; see the validation report before
making résumé claims. No query engine, row updates, multi-table transactions, schema evolution,
access-control layer, or end-to-end exactly-once ingestion across process restarts.
