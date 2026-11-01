# Track B: Index

The task, the input files and the exact output format are in the [spec](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/b-index.md).

## Approach

C++23, GCC 16, standard library and Linux APIs only. The process reads the queries
first and interns their terms in an exact open-addressed dictionary. Short terms
use packed, lowercase bytes. Terms longer than 16 bytes also compare their entire
suffix, so hashing never substitutes for equality. Query strings are immutable
views into the mapped query file.

A 1 MiB corpus sample chooses phrase anchors and the Boolean storage format.
Sampling affects performance only: every document is still scanned and every
query is evaluated exactly. Only Boolean terms and selected phrase anchors need
dictionary entries during the main scan.

Twelve workers scan independent document partitions. Partitions contain at most
65,536 documents and start at multiples of 64 IDs. Sparse Boolean postings store
16-bit local document IDs; frequent terms use bitmaps. Posting lists are naturally
sorted, and repeated occurrences of a term are recorded once per document.
Boolean evaluation applies operators strictly left to right, switching between
sparse lists and dense bitmaps. Very unbalanced sparse operations search the
shorter list against the longer one. Results use 64-bit counts and ID sums.

Phrases are verified directly around anchor occurrences during the scan, forward
and backward within the current document. Byte-wise ASCII token checks ignore
punctuation and UTF-8 separator bytes, preserve repeated words, and cannot cross
document boundaries. A per-worker stamp records each matching query/document
only once. Phrase hits are compact pairs rather than positional indexes.

Workers own their mutable indexes. Query evaluation shares the completed indexes
read-only. Partitions are combined in ascending document order, preserving the
ten smallest global IDs. The output is formatted once, explicitly closed, then
the process exits without individually freeing the temporary index allocations.
The kernel reclaims that memory as part of process exit.

## Build and run

```bash
g++ -O3 -std=c++23 -DNDEBUG -pthread -march=x86-64-v3 -static-libstdc++ -static-libgcc main.cpp -o /tmp/track-b-index
/tmp/track-b-index --data /path/to/data --out /path/to/out
```

The submission defaults to `/data` and `/out`. The Dockerfile builds completely
offline from pinned, frozen-allowlist GCC 16 and Debian 13 images. It uses the
AVX2/SSE4.2 x86-64-v3 baseline supported by the scoring machine. There are no
download, package-installation, runtime training, or answer-cache steps.

For local diagnostics, `B_THREADS=1..32` overrides the worker count and
`B_PROFILE=1` prints phase timings to stderr. Neither affects result semantics.
The Python starter files are preserved byte-for-byte and are not the entrypoint.

## Dependencies

None.

## Validation and performance

See [PERF.md](PERF.md) for exact correctness hashes, baseline and optimized
measurements, rejected experiments, scoring caveats, and remaining bottlenecks.
Reproducible scripts, source checkpoints, full raw measurements, and the command
record live in `../benchmarks/track-b/`, outside the scored directory.

Docker daemon access on the development machine is denied. Native builds and
validation are available; a Docker build, sandbox check, and official leaderboard
measurement must still be performed on a machine with Docker access.
