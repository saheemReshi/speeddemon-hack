# Track D: Traverse

C++20, zero third-party dependencies. Reads `/data/graph.txt` and `/data/queries.txt`, writes only `/out/distances.txt`, and exits 0 on success. The frozen [specification](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/d-traverse.md) defines the input and exact output. No input-specific results or indexes are embedded.

## Approach

The input is mapped read-only and parsed directly as ASCII integers. After `mmap`, integer parsing consists of ordinary userspace memory reads; it makes no per-number system calls and creates no strings. Mapping also avoids copying the entire file into a private buffer. The parser relies on the spec's well-formed, single-delimiter, trailing-LF grammar.

One pass reads edge triples and counts incoming/outgoing degrees. Prefix sums and a scatter pass produce forward and reverse compressed sparse row (CSR) graphs. Each graph has contiguous `(neighbor, uint32 weight)` edges and an offset array. The temporary triples and file mapping are released before searching. Every edge is retained, including duplicates, parallel edges, self-loops and zero weights.

Each query uses bidirectional Dijkstra. The backward search follows reversed original edges, preserving directed semantics. We expand the smaller queue, alternating equal sizes. This balances work even when one end has enormous initial weights. A meeting gives an upper bound; termination requires the sum of queue minima to reach that bound. Stale entries are skipped. Zero weights remain legal.

Each worker owns reusable radix heaps and distance arrays. Radix heaps exploit nondecreasing integer keys; bucket storage is retained between queries. Query generation tags avoid clearing a node-sized array every time. An atomic counter distributes queries dynamically, and answers are stored at their original indices. Joining workers synchronizes results before output.

## Compact state without approximate answers

The first search uses 32-bit words: 11 high bits identify the query and 21 low bits hold a distance below `2^21`. Queue entries are only 8 bytes. Entries from older generations are absent; when the tag wraps, arrays are cleared before reusing tags.

The compact search returns an answer **only if it is below `2^21`**. Since weights are nonnegative, every prefix of any shorter competing path would also be below that limit and would have remained eligible. Thus every returned answer is exact. Queries without such an answer are rerun with 64-bit words after all compact worker state is destroyed. These include unreachable queries as well as long paths.

The fallback has 53 distance bits. The frozen graph has 1,740,512 nodes, and `(n-1) * UINT32_MAX < 2^53` for all accepted `n <= 2^21`, so it covers every possible shortest path. Edge-weight addition widens **before** adding, including in the compact search. No distribution assumption is needed for correctness.

Six workers match the target's six physical P-cores and use roughly 80 MiB of distance state, compared with 159 MiB for six workers using 64-bit state. Forward/reverse CSR uses about 90 MiB. `D_THREADS` may select 1–12 workers for controlled experiments; the default is six. This machine has only four P-cores, so the final worker choice still needs a board measurement.

## Build and run

The Dockerfile uses the allowlisted, digest-pinned GCC 16 and Debian 13 images. Compilation is offline and statically links the C++ standard library and libc. The runtime runs as uid/gid 65534, needs no writable scratch, and performs no network operations. No source outside this folder is needed. The unchanged starter Python files are retained for provenance and are not copied into the runtime image.

From the repository root, a local build with the same flags is:

```sh
g++ -std=c++20 -O3 -march=x86-64-v3 -mtune=alderlake -pthread -static -s track-d/main.cpp -o /tmp/traverse
/tmp/traverse /absolute/path/to/data /absolute/path/to/existing-output-directory
```

With Docker access, use the starter's exact sandbox check:

```sh
python3 check.py d --data /absolute/path/to/full-seed1 --expected a23dbdf0a3e49cc8901310a9ee1dc6a9a569a85297b6da75c2a7002cb2ed277e
```

See [PERF.md](PERF.md) and [measurements.json](measurements.json) for local measurements, correctness evidence and pending official validation. The actual Docker build/run and official S, M and K have not been measured: the development session cannot access the Docker socket. Set the actual team name in `.speed-demon.toml` before submission; its starter placeholder is unchanged.

## Dependencies and another 24 hours

No third-party runtime or build libraries. GCC, libc, the C++ standard library and threads are supplied by the allowlisted toolchain; declared runtime dependency count is zero (`D = 1`). The existing repository MIT licence applies.

Next, compare 4/6 workers on the scoring machine, obtain board run IDs, and measure parsing versus CSR scatter there. A small amount of build-time PGO may improve the parser/search loops, but it was not tested. Heavy routing indexes and node reorderings would need to repay their preprocessing and source costs inside the same run; the measured reorderings here did not.
