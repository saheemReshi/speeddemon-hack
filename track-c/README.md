# Track C: Join

C++23 implementation of the frozen [Track C specification](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/c-join.md).
Reads `/data/records.csv` and `/data/regions.csv`; writes only `/out/aggregates.txt`.
Native options: `--data DIR --out DIR --threads N`. The output directory must exist.

## Algorithm

Regions populate a dense byte array indexed by region ID. Country codes receive
compact IDs 1–240; zero means `_UNMATCHED`. Unknown IDs are bounds-checked.
Each worker has fixed, cache-line-aligned country aggregates.

Twelve workers parse disjoint record ranges. An AVX2 quote-parity pass locates
partition boundaries: an LF ends a record only outside quotes. Doubled quotes
toggle parity twice and cannot produce a false boundary. The per-record CSV
scanner uses a bounded SSE quote search and `memchr` for longer spans. Quoted
commas, escaped quotes, embedded LF/CR, empty text, optional final terminators
and BOMs inside data remain intact. A BOM is skipped only at a record boundary.
The scanner trusts valid input and the guaranteed unquoted numeric columns.

Names use a shared flat table with **no fast-path string allocations or copies**.
A slot holds an atomic pointer to the original CSV field and a 256-bit country
bitmap. CSV interiors are canonical keys: every literal quote must be doubled,
while unquoted fields cannot contain quotes. Comparing exact interior bytes is
therefore equivalent to comparing decoded values. Quoted and unquoted copies of
the same name compare equal; empty values agree and trailing spaces remain distinct.

CRC32 of the length and first/last words selects slots. Full exact comparisons
resolve collisions; correctness never depends on a fingerprint. Short comparisons
use overlapping word loads without reading outside the values. After at most 64
occupied probes, a mutex-protected standard hash map handles overflow. Slots are
never deleted, so once a key reaches overflow, future copies also reach overflow.
This supports arbitrary distinct-name counts and hostile hash collisions.

An atomic compare-and-swap publishes a field pointer. The pointed-to bytes were
already immutable before workers started, so relaxed ordering is sufficient.
Atomic OR sets country bits; only the worker that first sets a bit increments
its local distinct counter. Workers join before the final aggregate reduction.
No per-worker name-table merge is required.

Notes are never materialized: literal span lengths plus one byte per doubled
quote give their decoded byte counts. Money uses integer cents throughout, with
64-bit sums and ID totals. Only populated groups are emitted, sorted by country
bytes, with exact signed money formatting. `_UNMATCHED` sorts last.

The read-only mapping outlives all workers and stored keys. It remains mapped
for name comparisons. Native RSS includes the input's resident file pages;
the official harness instead measures cgroup charges after prewarming the data
outside the run cgroup. Those two memory measurements are not interchangeable.
The main table itself occupies 10 MiB, plus region lookup and runtime overhead.

## Build and dependencies

No third-party dependencies. The Dockerfile uses allowlisted GCC 16 and Debian
images pinned by their exact index digests. No downloads or package installation
occur in build steps. The build command is:

```sh
g++ -std=c++23 -O3 -DNDEBUG -flto -march=x86-64-v3 -static -pthread -s main.cpp -o /join
```

`x86-64-v3` supports the published AVX2 CPU without depending on the build host.
PGO, native CPU tuning, owned input buffering, region hashing, and private name
tables were tested and rejected. See [PERF.md](PERF.md) for comparative results.
The Python starter files remain byte-identical to the published references and
are not the container entrypoint. Team identity is unchanged.

## Validation and performance, 2026-11-01

The final native GCC 16.2.1 release passes the published sample/full seed-1
hashes, all 14 adapted official tests, tiny seeds 2/42/2026, sample seed 2,
full seed 2, and 2,002 legal grammar cases. ASan/UBSan passes the official and
grammar cases; ThreadSanitizer passes the grammar and forced-collision overflow
tests. Leak detection is disabled because it cannot run under this environment's
process tracing. There are no reported sanitizer errors in the completed checks.

The final interleaved full-scale comparison on an i5-12450H measured **140.815 ms**
versus **839.098 ms** for the preserved correct baseline, approximately **6× faster**.
A separate five-run batch measured 143.371 ms. Final maximum native RSS was
**209,440 KiB**, including the approximately 202 MB mapped input. These are local
measurements, not leaderboard time, cgroup memory or K.

**Official Docker validation remains blocked by socket permissions.** `check.py`
verified every `FROM` against the allowlist, but the offline build could not start.
The user is handling Docker access separately. Once available, rerun:

```sh
python3 check.py c --data data/c/full-seed1 --expected d8088b1cd42ba1338b1345b107b55db7b96ef1f5066f3ff64fd26a8912a0f536
```

No commit, push or leaderboard submission has been made. Official image size,
sandbox execution, cgroup memory and formatted S/D still require validation.
