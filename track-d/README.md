# Track D: Traverse — C++20

Zero third-party dependencies. Reads `/data/graph.txt` and `/data/queries.txt`, writes exactly `/out/distances.txt`, and exits 0 on success. All input-derived work happens inside the run. The frozen [specification](https://github.com/hackathon-raptors/speed-demon-2026/blob/main/specs/d-traverse.md) defines the contract.

## Current result

The latest official baseline supplied at stop is **639 ms, 182 MB, K=11.221**. The preserved local baseline snapshot is `67e7631` with the earlier reported S=257. This revision has not been pushed or scored officially. Conditional K from local ratios is **17.70**, not an official score. Work is stopped; see `benchmarks/track-d-v2/CHECKPOINT.md`.

In seven interleaved local full-data runs, the unchanged baseline took 1148.3 ms median and the new solution 587.5 ms. Maximum sampled anonymous memory fell from 171.5 to 64.2 MiB. Local formatted code count is 409, including Dockerfile. These are development measurements, not official T, M or S. See [PERF.md](PERF.md) and [measurements-v2.json](measurements-v2.json) for the experiments, hashes, raw runs and projection caveats.

## Graph construction and representation

The read-only input is mapped and divided into six byte ranges whose boundaries are advanced to complete LF-terminated records. A direct ASCII parser consumes the first digit, then scans until SP or LF. This relies on the frozen well-formed numeric grammar; it is not a general text parser.

The first parallel pass counts incoming/outgoing degrees with shared atomic bytes and counts large weights per input range. Every byte wrap records an additional 256 edges; a degree of 300 is stored as 44 plus one wrap. After joining, the code reconstructs exact degrees. Relative insertion cursors reuse these bytes when no count wrapped; otherwise the run uses full uint32 cursors. High degrees are supported rather than assumed absent.

Each direction uses a 12-byte record per node, with two edge words inline and the remaining edges in a contiguous CSR spill array:

```text
node[u]:  metadata | edge[0] | edge[1]
metadata & 3:   degree 0, 1, 2, or at least 3
metadata >> 2:  spill start; node[u+1] supplies the spill end
```

The sentinel record `node[n]` supplies the final spill endpoint. A zero edge word is legitimate data: degree tags determine which slots exist. Prefix metadata and atomic slot reservations ensure every edge has a unique destination. Phase joins synchronize the relaxed counters before the next phase reads them.

A common edge uses 21 vertex bits and 10 weight bits in one uint32. Bit 31 selects an entry in a shared full `{from, to, uint32 weight}` array for weights of 1,024 or more. Thus UINT32_MAX and every other permitted weight remain exact. Reverse traversal chooses the original tail from that record. No reverse weight is inferred from its opposite edge. All duplicate/parallel edges, self-loops, isolated nodes and zero weights are retained.

The second pass reparses the input directly into these arrays. No full temporary edge-triple array, sorting or routing index is built. The public full graph's measured payload is 55,530,828 bytes. Putting common edges beside metadata removes a separate offset lookup; the timing improvement was measured, while hardware cache-miss counters were unavailable.

## Shortest paths and scratch memory

Queries use bidirectional Dijkstra over the directed forward/reverse representations. Each step expands the smaller priority queue, alternating ties. A meeting supplies an upper bound; termination requires the sum of both queue minima to reach that bound. Stale entries are skipped. Nonnegative and zero weights are supported.

Each worker owns two reusable binary radix heaps and a chained hash directory with contiguous state/link arrays. One record holds both directions' distances for a visited node. Node indices keep links valid when vectors grow. Only the fixed 256 KiB directory is cleared between queries; arena capacities survive and active state grows with visited nodes rather than allocating a graph-sized array per worker. No node-by-node heap allocation is needed.

The compact state packs a 21-bit node id and two 21-bit distances in eight bytes. The all-ones distance is reserved, so it returns an answer only below 2,097,151. Every prefix of a shorter nonnegative path is also below that limit; consequently returned answers are exact. Other queries, including unreachable ones, are rerun with full uint64 distance state after the compact worker arenas are destroyed. The fallback's bound is `2^53`: for the accepted `n <= 2^21`, every possible simple path sum `(n-1)*UINT32_MAX` is strictly smaller. Addition widens before adding the edge weight.

An atomic work counter distributes queries; separate answer slots preserve input order. Defaults are six build workers and six query workers, matching the target's physical P-cores. `D_BUILD` and `D_THREADS` allow 1–12 workers for controlled experiments. The local CPU has only four P-cores, so the optimal target setting still needs a board comparison.

## Build, verification and dependencies

The unchanged Dockerfile compiles a static executable offline using allowlisted, digest-pinned GCC 16, then copies only the executable into the allowlisted Debian 13 runtime. It runs as uid/gid 65534, uses no network or scratch files, and needs no source outside this folder. The untouched Python starter files are byte-identical published copies and are not runtime entrypoints. Declared non-stdlib runtime dependency count remains zero (`D=1`). The repository MIT licence applies.

From the repository root:

```sh
g++ -std=c++20 -O3 -march=x86-64-v3 -mtune=alderlake -pthread -static -s track-d/main.cpp -o /tmp/traverse
/tmp/traverse /absolute/data/directory /absolute/existing/output/directory
python3 check.py d --data /absolute/full-seed1 --expected a23dbdf0a3e49cc8901310a9ee1dc6a9a569a85297b6da75c2a7002cb2ed277e
```

Release and ASan/UBSan checks pass the official hand-written examples, independent random all-pairs comparisons, counter/weight/radix boundary tests, tiny seeds 1/2/7/42 and sample seeds 1/2/7. Repeated full public runs match the published SHA-256. Leak detection was disabled because LeakSanitizer cannot operate under the development sandbox's tracing restrictions.

The frozen allowlist checker passes. Actual `check.py` Docker execution remains unverified: the development session cannot access `/var/run/docker.sock`. Official cgroup `memory.peak`, formatted scc S, K and the new board run ID remain pending. Experimental sources, the full original snapshot, raw logs and the verifier are preserved in `benchmarks/track-d-v2/`, outside the scored folder.

## Remaining opportunities

First obtain a new official run and compare 6/12 query workers on the six-P-core machine. Query processing and graph scatter remain the largest measured costs. GCC 15 was unavailable locally; native/LTO and PGO experiments did not provide a reliable K win here. The 150–200 ms official target has not been established. Preserve the current measured candidate while testing further target-specific changes.
