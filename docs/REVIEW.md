# Independent implementation review

Reviewed on October 7, 2026, by a separate reviewer after the implementation was present.
The reviewer inspected the core format, transaction, table, object-store, and GC code, as well as
CLI, tests, CMake, CI, and the documented contracts. Review was read-only; fixes were applied
by the implementing agent and checked again by the reviewer.

| Finding | Impact | Resolution |
|---|---|---|
| P2 duplicate string projections moved one value twice | Repeating a projected string column returned an empty second value | Copy projected values; regression checks repeated string and mixed columns |
| P2 optional checkpoint hint read errors aborted scans | Intact authoritative logs could become unreadable because the optional hint failed | All hint-read failures fall back to full replay; log failures still propagate |
| P2 non-finite public API predicate literals were accepted | Equality against NaN incorrectly matched finite rows | Reject non-finite double literals before pruning or scanning |
| P2 unchecked stored timestamp conversion | Fractional timestamps passed verify; oversized floating conversion risked undefined behavior | Strict signed-integer/range helper used at metadata timestamp boundaries |

The reviewer reproduced each defect, source-reviewed the fixes, and ran six focused post-fix
regressions successfully. The two additional focused checks covered newly published GC
references and corrupted pruning statistics. No further actionable findings remained within the
explicit immutable-key and offline-GC preconditions.

Implementation verification separately found and fixed:

- Predicate parser mistook AND inside a quoted string for a trailing conjunction.
- Azure duplicate blob conditional creates use HTTP 409 BlobAlreadyExists; map this to
  AlreadyExists while preserving transient S3 409 semantics.
- Azure list XML may omit ETag quotes present in HEAD; normalize tokens before GC comparisons.
- Local post-rename durability failure must surface an uncertain write outcome, not definitive failure.
- Verify now recomputes file and row-group statistics, in addition to CRCs, sizes, row counts, and
  complete log continuity, so corrupted pruning metadata is detected.

Review does not establish absence of every bug. Remaining scope limits and verification levels
are explicit in DESIGN.md and VALIDATION.md.

## Upgrade review

The same independent reviewer examined the streaming/cancellation/budget implementation,
CRC optimization, workload runner, cleanup helper, and CI. Two verification defects were fixed:
TSan selection omitted the streaming regressions, and the real-service expiration assertion
accepted unrelated failures. Streaming cases now run under TSan; real-service expiration must
return exit 1 with the typed Expired error. Documentation now states the buffer and cancellation
limits. Implementation review also moved consumer-exception cancellation inside the sink lock,
so waiting consumers observe cancellation before invoking another callback.

The follow-up reviewer verified all upgrade fixes read-only and reported no remaining concrete
findings. Eleven focused streaming/CRC regressions passed in the independent upgrade review.

## Linux CI portability finding

The first remote TSan run reported a shared libstdc++13 ctype locale-cache race during
concurrent regex construction in metadata validation. Metadata/schema identifiers now use
explicit ASCII checks, and data-path validation is shared with GC. Predicate regex parsing
uses serialized static patterns to avoid concurrent locale-cache initialization. Added boundary
and concurrent parser regressions exercise this fix without sanitizer suppressions.

The reviewer verified the portability fix with four focused tests and reported no remaining
findings. The corrected Linux TSan CI job passed without suppressions.
