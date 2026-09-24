# Storage design and contracts

## Scope and ownership

The table API owns transaction semantics. Log owns commit publication and reconstruction.
Format owns binary encoding, validation, statistics, and column reads. ObjectStore owns service
or filesystem operations and returns typed results. Format/Table/Log APIs throw typed `Failure`;
unexpected input/metadata parsing errors are translated at their boundary where needed.
No custom query planner, asynchronous runtime, metadata service, or plugin framework is involved.

Stores are shared through `shared_ptr` because tables, decorators, and worker threads share their
lifetime. Values, snapshots, chunks, and results own their buffers. Request string_views are valid
only for the synchronous call; adapters do not retain them. Workers join on all exits, including
thread-construction failure. The mutable shared state consists of backend synchronization,
atomic metrics, and bounded worker scheduling. Independent table operations do not mutate a
shared cached snapshot. MemoryStore serializes operations with a mutex. LocalStore uses POSIX
flock across independent processes, writes/fsyncs a hidden staging file, renames atomically, then
fsyncs ancestor directories. Each local object has a private 36-byte ETag prefix, invisible to the
ObjectStore caller. A stage file interrupted before rename is ignored by listing. These hidden
staging files are not reclaimed by table GC; offline filesystem cleanup may remove them.
LocalStore assumes all access goes through cooperating clients and a filesystem supporting flock,
atomic same-filesystem rename and fsync; it is not an NFS or hostile-directory security abstraction.

## Namespace and publication

```
catalog/<table>.json
tables/<table>/_log/<20-digit version>.json
tables/<table>/_checkpoints/<version>.json
tables/<table>/_last_checkpoint
tables/<table>/data/<uuid>.lsf
gc/pending/<uuid>.json
gc/audit/<uuid>.json
```

Table IDs are portable path components. Catalog registration occurs **before** the first log
commit, using put-if-absent; registration is idempotent. A crash can leave a registered table with
no commits. GC treats this as an incomplete creation with no roots, and create can complete it.
An existing committed target cannot be overwritten by another create/clone. Catalog entries are
never deleted in v1. GC checks for committed table logs missing from the catalog and fails closed.
Clones reference full keys within the same store prefix; cross-bucket/cloud clones are unsupported.
Clone work and metadata cost scale with the source file count; zero-copy means zero data copies,
not constant-time metadata creation.

## Commit protocol

1. Discover and pin the latest numbered commit. Lists consume all pages. Reconstruct a contiguous
   parent chain, optionally starting from a validated checkpoint at or before the requested version.
2. Upload new immutable UUID-named data files using put-if-absent. Upload uncertainty is resolved
   using the original key and exact bytes. Failed writers can leave invisible orphan files.
3. Build an immutable commit payload containing parent, version, commit ID, timestamp and actions.
4. Put-if-absent at parent+1 is the commit point. No listing or hint update is part of publication.
5. On precondition failure or uncertain write, read that exact key. Own ID and identical payload
   means success; another ID means a different writer won; missing means retry the same key and
   bytes. A delayed original request can still finish, so absence never authorizes rebase.
6. Rebase only after observing another winner. Append has no file-removal conflict. Compaction
   checks all its input files remain live. Overwrite, restore, and retention updates abort on any
   intervening commit. Contention and uncertainty have bounded retry budgets.

A result of `UnknownOutcome` includes commit ID and attempted key. CLI exit code 3 means the
operation may have committed. Inspect that key/history before attempting another ingestion.
A new process is a new request: no durable client request identity exists across restarts.
Conditional writes are excluded from the generic retry wrapper; adapters themselves do not retry.
Safe reads and unconditional idempotent operations use bounded exponential backoff with jitter.
S3 conditional 409 conflicts and throttling map to transient errors; 412 maps to condition failure.
The protocol resolves uncertain conditional outcomes instead of treating a transient write error
as proof of failure. Data/log keys must not be deleted by external lifecycle rules or administrators.

Readers pin a version and file set; concurrent commits do not change their logical view. This is
snapshot-consistent scanning and file-level optimistic conflict detection, not a general-purpose
SQL transaction isolation implementation. Commits are ordered by version. Logical timestamps
are max(client wall clock, previous timestamp+1 ms), recomputed on rebase. As-of is deterministic
for those logical timestamps, but clock skew can make them differ from real-world wall time.

## Operation semantics and retention

Create/clone publish version 1 after registration. Append adds files. Overwrite removes all files
in its starting snapshot and adds replacements, committing only if the table has not changed.
Restore diffs the current file set against a retained target, and also requires an unchanged
parent. Compaction rewrites the starting files; unrelated appends survive rebase.
No schema changes are permitted. Empty append/overwrite is permitted and creates a version.

`expire_before` is a monotonic action in the same ordered log. Every API resolves the current
retention floor before historical access. Keep-last counts every version, including maintenance
commits. Older-than keeps versions whose logical timestamp is at least the cutoff and always
keeps the previously current snapshot plus the expiration commit. Expiration does not erase log
actions: old actions remain necessary to reconstruct retained snapshots. All log/checkpoint
objects are retained in v1. GC does not inspect only the newest snapshot; it unions retained
snapshots from every table, so source expiration cannot invalidate a clone's independent roots.

## Offline GC

The caller must guarantee namespace-wide quiescence throughout each destructive invocation,
including paused clients. This is an explicit precondition, not a claim of online coordination.

Mark lists data files outside all retained roots and older than the requested grace. A durable
manifest records keys, ETags and ready-after time. Sweep considers only manifests present when
the invocation began, waits for their deadline, rechecks complete roots, then deletes only
unreferenced objects still bearing the captured ETags. Missing candidates are safe to skip.
Deletion is idempotent after a crash; completed passes get an audit object. Dry-run is read-only.
Clients may run between offline phases, so rechecking protects completed new references. It
cannot protect a clone/restore racing a sweep; the offline precondition excludes that race.
Grace is not a substitute for quiescence and cannot protect indefinitely paused processes.
Only canonical LSF data paths are eligible; malformed metadata fails closed. Concurrent GCs are
unsupported. No tombstones block online commits because this release does not offer online GC.

## LSF v1

```
"LSF1" | row-group column chunks | UTF-8 JSON footer |
footer length (u64 LE) | footer CRC32C (u32 LE) | "LSF1"
```

Every chunk is independently checksummed with CRC32C (Castagnoli). Scalars have a one-byte null
flag (0=null, 1=valid); nulls have no payload. Int64 and timestamps use 8-byte little-endian
signed bit representations; doubles use IEEE-754 binary64 little-endian bits, finite values only.
Strings use u32 little-endian byte length followed by bytes. Row groups have 1..65,536 rows.
Footer includes format version, schema, file row count, each group's row count, chunk offset/
length/checksum, and min/max/null counts. Offsets are validated as a contiguous non-overlapping
layout within the data area. Sizes are capped before allocation. File/chunk/footer limits are
512/64/16 MiB. Bytewise string comparison is identical in stats and predicates.

Reads fetch HEAD, the four-byte header, the 16-byte trailer, then the exact footer range. Only
predicate and projected columns are fetched for matching groups. Selected chunks are verified
before decoding; `verify` reads all columns of all retained files and checks log continuity.
There is no promise to detect corruption in a chunk that a normal scan never fetches.
File statistics in log metadata allow pruning without opening the file. Missing or malformed
stats never justify pruning; generated stats are tested against full scans. Null comparisons
never match. Default scans materialize matching rows. Streaming scans pin the snapshot and deliver
borrowed row-group batches through one serialized sink. There is no producer queue: a slow sink
retains reservations and directly backpressures workers. Returning false or setting cancellation
stops future work; exceptions cancel peers and propagate after joining all workers. Cancellation
cannot preempt active synchronous I/O or the consumer callback. Batch ownership ends on return;
consumers retaining rows must copy them. Output order is unspecified.

A shared budget reserves conservative encoded/decoded/projection buffer charges before each
row group's data reads. A group exceeding the budget fails explicitly instead of waiting forever.
The budget excludes snapshot/footer metadata, allocator overhead, and consumer-retained output;
these limits are not a process RSS guarantee. Footer size remains bounded per worker.

## Checkpoints and scaling limits

Every 10 acknowledged ordinary commits attempts a best-effort checkpoint. Its immutable state
has CRC32C protection. The mutable hint is never a commit point: absent, stale, future, invalid,
or unreadable checkpoint payloads fall back to replay. Historical reads never use a future
checkpoint. Full verify replays the log regardless of checkpoints. A successful commit remains
successful if optional checkpoint publication fails. A resolved timeout may omit the checkpoint.

Listings and clone metadata scale linearly; metadata objects/results are materialized. GC root
reconstruction favors clarity over performance. Many writers on one table can contend/retry.
No claim is made of distributed consensus, production ingestion throughput, adaptive compaction,
cache hierarchy, or a replacement for a production table format.

## Primary references

- [S3 conditional writes](https://docs.aws.amazon.com/AmazonS3/latest/userguide/conditional-writes.html)
- [Azure strong consistency and optimistic concurrency](https://learn.microsoft.com/en-us/azure/storage/blobs/concurrency-manage)
- [Azure Shared Key protocol](https://learn.microsoft.com/en-us/rest/api/storageservices/authorize-with-shared-key)
- [libcurl AWS SigV4](https://curl.se/libcurl/c/CURLOPT_AWS_SIGV4.html)
- [Delta vacuum retention concerns](https://docs.delta.io/delta-utility/)
- [Pinned MinIO source release](https://github.com/minio/minio/tree/RELEASE.2025-10-15T17-29-55Z)
- [Azurite emulator](https://github.com/Azure/Azurite)
