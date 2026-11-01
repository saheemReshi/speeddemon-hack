# Track D: performance log

All solution work below was performed during the competition on November 1, 2026. **Every official board run ID is pending. No commit or push was made.** These are local experiments, not claimed official measurements. Fill in actual board IDs and cgroup peaks after running the corresponding revisions; do not present local runs as board history.

## Measurement conditions

Development machine: Intel Core i5-12450H, four physical P-cores with SMT and four E-cores, 12 MiB L3. Timed candidates were pinned with `taskset -c 0-7` to the four P-cores/eight hardware threads. The target instead has six physical P-cores, twelve hardware threads, fixed clocks and 24 MiB L3. Laptop turbo, other activity and sustained-load variation were not controlled; differences of a few percent need board confirmation.

GCC 16.2.1, C++20, `-O3 -march=x86-64-v3 -pthread` for exploratory builds. Final release additionally uses `-mtune=alderlake -static -s`, exactly as in the Dockerfile. No AVX-512 requirement. Every successful timed full/sample run checked the official output SHA-256. Data generation and correctness suites finished before the main full-scale timing experiments. Initial sample timings ran alongside full-data generation and are preliminary.

Wall time covers process launch through exit, parsing, graph construction, queries and output. Python `wait4` supplies the kernel's peak RSS; a later comparison also sampled `/proc/<pid>/status` RssAnon at approximately 2 ms intervals. RSS includes the mapped input. Anonymous samples are a useful comparison of allocations, **not** the official kernel high-water mark. The harness's M is cgroup `memory.peak`; its host-prewarmed input cache is excluded. Neither local memory metric is substituted for official M or used to claim an official K.

## Experiments and decisions

Unless stated otherwise, rows are medians of three full seed-1 runs. Peak RSS is the maximum in that group, converted to MiB. Raw times, hashes, memory values and profiled phases are retained in `measurements.json`. Every run ID below is pending.

| Step / experiment | Full wall, s | Peak RSS, MiB | Official run ID | Decision |
|---|---:|---:|---|---|
| Published naive Python reference | 72.692655 | N/A | Pending | Official calibration: M=892,915,712 bytes; not a local timing |
| Mapped ASCII parser, dual CSR, forward Dijkstra, binary heap, 1 worker | 9.887 | 251.4 | Pending | Local C++ baseline; avoids Python objects and parsing overhead |
| Bidirectional Dijkstra, binary heap, 1 worker | 3.969 | 251.4 | Pending | Keep: less search work; checks both directed adjacency lists |
| Bidirectional Dijkstra, radix heap, 1 worker | 2.350 | 251.4 | Pending | Keep: integer monotonic queues outperform comparisons |
| Radix heap, 4 workers | 1.168 | 251.4 | Pending | Keep query-level parallelism |
| Full BFS renumbering after constructing both graphs, 4 workers | 1.280 | 251.4 | Pending | Reject: queries ~0.23 s, but load/reorder ~1.01–1.18 s |
| Radix, 2 / 4 / 6 / 8 / 12 workers, repeated batch | 1.517 / 1.137 / 1.173 / 1.175 / 1.166 | 251.4 / 251.4 / 259.4 / 314.4 / 423.9 | Pending | On four P-cores, higher counts mostly add memory |
| Generation tags, 64-bit state, 4 workers | 1.115 | 251.3 | Pending | Small time gain; eliminates touched-node reset writes |
| Renumber before final CSR build, full BFS / 512-node / 2048-node clusters | 1.229 / 1.307 / 1.165 | ~258.1 | Pending | Reject: extra preprocessing/source; no clear total win |

Two-pass parsing removes the 60 MB temporary edge array, but reparses the full file. Filling forward/reverse CSR in separate threads avoids shared writes but parses each edge again. In a five-round shuffled comparison: edge triples + 64-bit state/4 workers took 1.223 s; serial two-pass 1.392 s; parallel two-pass 1.358 s. Compact state/parallel two-pass took 1.398 s with four workers and 1.281 s with six. All hashes matched. Reject both alternate loaders: their lower RSS partly reflects losing the input mapping from the peak; compact six-worker search already determines anonymous memory at roughly the same level as the triple loader. They also add source and synchronization code.

The compact-distance experiment uses an exact bounded fast path and a 64-bit fallback, described and justified in README.md. A final five-round shuffled comparison used the triple loader for all variants:

| Search state / queue / workers | Median wall, s | Peak RSS, MiB | Max sampled anonymous, MiB | Official run ID |
|---|---:|---:|---:|---|
| 64-bit / 16-byte queue entries / 4 | 1.246 | 251.5 | 198.9 | Pending |
| 64-bit / 16-byte queue entries / 6 | 1.178 | 256.7 | 253.4 | Pending |
| Compact / 8-byte queue entries / 4 | 1.293 | 251.5 | 160.3 | Pending |
| Compact / 16-byte queue entries / 6 | 1.181 | 251.5 | 173.6 | Pending |
| Compact / 8-byte queue entries / 6 | **1.143** | 251.5 | **171.6** | Pending |

Keep compact state, compact queues and six workers: versus six workers with 64-bit state, both time and memory improve. Six workers also match the target's physical P-cores. Source cost is limited by using the same search/heap templates for both widths. Remove all losing search, loader and reordering branches from the final source. No ready-made graph algorithm, thread pool, shortest-path index or non-stdlib library is used.

Review of compact queue entries identified an arithmetic hazard: adding a uint32 key and uint32 weight can wrap before conversion to uint64. Final code explicitly uses `U64(d) + w`; a hand-computed 4,294,967,302-distance regression covers it. Preliminary full-hash success alone did not establish this edge case.

## Correctness

Final release verification covers the seven official hand-written cases; 80 random directed multigraphs (random seed 77019) with all-pairs Floyd–Warshall as an independent oracle; zero and UINT32_MAX weights; parallel/duplicate edges; isolated nodes; self-loops; empty query files; the compact cutoff; and 3,000 queries to wrap generation tags. Run with 1, 4 and 6 workers. Generated tiny seeds 1, 2, 7 and 42 and sample seeds 2 and 7 match the official Python reference.

| Dataset | Expected distances.txt SHA-256 |
|---|---|
| Tiny seed 1, reference-checked | `ecaca77d3221fe3928109da36480cfdb39f288f2e57ecbc61d3d7706d5cb4bd5` |
| Sample seed 1, official | `a4c76ed030bc7e365db33191f6cf74708fba7f25eb48d64c8c4279f2e57046a0` |
| Full seed 1, official | `a23dbdf0a3e49cc8901310a9ee1dc6a9a569a85297b6da75c2a7002cb2ed277e` |
| Sample seed 2, reference-checked | `70e94813fc6fbee12c22cc040bc4a4629c70508d13eaab2bfabbdd75e1d51aed` |
| Sample seed 7, reference-checked | `88bf36c94620505d1beafe7cd56a08dc2d261b71f9824d2f764d5ce6e5d7872e` |

The generated full input hashes also exactly match the frozen spec. Local verifier, generator data and exploratory source snapshot are in `/tmp/speed-demon-d`; none are needed by the build or included in the image.

### Final static release

Five runs per dataset, default six workers, same P-core affinity and exact Dockerfile compilation flags. Every hash matched. Trimmed mean discards the fastest and slowest, as the official timing protocol does, but these remain local observations. Sample data is not a predictor of full-query cost.

| Dataset | Median wall, s | Middle-three mean, s | Peak RSS, MiB | Max sampled anonymous, MiB | Official run ID |
|---|---:|---:|---:|---:|---|
| sample seed 1 | 0.077250 | 0.078777 | 24.9 | 19.3 | Pending |
| full seed 1 | 1.075605 | 1.071546 | 248.8 | 171.6 | Pending |

The final static binary passes all release checks described above. The same regression and generated-seed suite passes AddressSanitizer and UndefinedBehaviorSanitizer. LeakSanitizer initially could not operate under this sandbox's tracing restrictions; rerunning with `ASAN_OPTIONS=detect_leaks=0` leaves address/undefined-behavior checks enabled and passes. Leak detection itself was not verified. The binary has no dynamic ELF section.

Reproduction helpers and all test data remain under `/tmp/speed-demon-d`: `bench.py`, `interleave.py`, `verify.py`, `experimental.cpp` and the generated `data/` tree. The final image copies only the executable. `measurements.json` retains raw successful timing records and hashes; it is measurement data and is not read by the solution.


## Limits and next measurements

The frozen Track D spec §3 says: “You may answer queries in any order and in parallel; only the output order is fixed.” All preprocessing and fallback work occur inside the timed process. No input-derived state survives into another run.

The official Dockerfile allowlist checker passes. The host-built executable is static. The actual offline image build and exact Docker sandbox run remain unverified because this session cannot access `/var/run/docker.sock`. Official formatted S, cgroup M and K also remain pending; scc/static-tools are unavailable here. No official run IDs or final scores are fabricated.

Profiling puts approximately half of time in input parsing, degree counts and CSR construction. That is the next bottleneck to split and measure on the scoring CPU. First compare 4/6 workers and record board IDs. PGO and heavier graph indexes were not tested. Any further complexity must buy enough total time or memory to offset the S penalty; a faster query phase alone did not suffice in the reordering experiments.
