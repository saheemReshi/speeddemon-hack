## Stopped checkpoint — latest official baseline correction

Work stopped at the user's request. Best code is preserved unchanged in `main.cpp`; all experiment sources and the original snapshot remain in `benchmarks/track-d-v2/`. The concise checkpoint is `benchmarks/track-d-v2/CHECKPOINT.md`.

Latest official baseline: **639 ms, 182 MB, K=11.221**. Conditional candidate K is **17.70** using local relative measurements and source cost; this is **not an official result**. Earlier 706 ms / K=10.670 figures and projections below are historical and superseded for the current comparison. No commit or push was made.

# Track D: optimization round 2

Starts from the entrant's official, correctness/sandbox-passing commit `67e7631`: **706 ms, 182 MB, S=257, K=10.670, machine score=51.408**. These official figures were supplied by the entrant; the run ID was not provided. This round made **no commit or push**. The new official run ID is **pending**. Every result below is local unless explicitly labeled official.

## Final comparison and scoring tradeoff

Seven shuffled rounds on full public seed 1, with each timed output checked against the official hash:

| Metric | Preserved `67e7631` | Selected inline layout |
|---|---:|---:|
| Local median wall time | 1148.3 ms | 587.5 ms |
| Kernel peak RSS, max of runs | 248.8 MiB | 144.7 MiB |
| Max sampled anonymous memory | 171.5 MiB | 64.2 MiB |
| Locally formatted code lines including Dockerfile | 257 | 409 |
| Official run ID | Not supplied; commit known | Pending; not pushed |

RSS includes the mapped input. RssAnon samples exclude that mapping but can miss transients. Neither is official cgroup `memory.peak`. Local line counting strips comments after clang-format LLVM and agrees exactly with the baseline's reported S; official scc measurement is still pending.

The conditional relative-K estimate is

```text
K_new/K_old = sqrt(T_old/T_new) * (Anon_old/Anon_new)^0.20 * (S_old/S_new)^0.15
```

With the local median ratios, this is **1.578x**, or **K≈16.83** anchored to official baseline K=10.670. It assumes local speed and sampled-memory ratios transfer to the scoring box. That gives a conditional projection of roughly **366 ms / 68.1 MB**, not a claimed result. At the projected memory and source size, matching C++ target K=21.340 requires about **228 ms officially**. The 150–200 ms target is **not demonstrated**; an official run is essential.

The larger 256-way radix heap initially helped prototypes. In the clean CSR comparison it was only ~1% faster with 424 rather than 359 code lines, giving a lower K proxy. In the winning inline layout the binary radix version was also faster (0.598 vs 0.612 s median). Keep the smaller binary radix heap. Three inline edges were faster than two in one batch (0.582 vs 0.595 s), but 68.7 rather than 64.1 MiB sampled anonymous memory erased that gain on K. Keep two inline edges. All variants remain preserved outside the scored folder.

## Measurement conditions and bottlenecks

Local CPU: i5-12450H, four physical P-cores/eight P-core hardware threads, 12 MiB L3. Timed processes were pinned to CPUs 0–7. The scoring CPU has six physical P-cores/twelve hardware threads, 24 MiB L3 and turbo disabled. Local clocks, competing activity and THP backing were uncontrolled. Host load averages reached roughly 7–11 during the work. Compare shuffled configurations within a batch; do not interpret the absolute times from different batches as a progression on a calibrated machine.

GCC 16.2.1, C++20, `-O3 -march=x86-64-v3 -mtune=alderlake -pthread -static -s`. `perf` and GCC 15 were unavailable, and Docker socket access was denied. Measurements used phase timers, operation counts, `wait4` peak RSS, and approximately 2 ms RssAnon sampling. Later records also include process CPU time and sampled AnonHugePages. Raw successful timing records and binary hashes are in [measurements-v2.json](measurements-v2.json); benchmark sources/scripts are outside the scored folder.

Baseline three-run phase measurements:

| Baseline phase | Measured range |
|---|---:|
| Allocate degrees + 60 MB edge triples | 14.6–15.0 ms |
| Parse and count degrees | 288.7–337.3 ms |
| Prefix sums | 2.1–2.4 ms |
| Allocate edges and cursor copies | 18.3–19.5 ms |
| Scatter and release temporary data | 224.0–268.5 ms |
| Parallel queries including worker state | 487.8–525.0 ms |
| Wide fallback | 3.9–4.9 ms |
| Output | 0.21–0.26 ms |

The final separate profiling build reports:

| Final phase | Local measured range |
|---|---:|
| setup_byte_degrees | 1.666–1.850 ms |
| parse_count_degrees | 64.595–73.852 ms |
| widen_degrees_if_needed | 0.011–0.013 ms |
| allocate_nodes | 17.605–18.118 ms |
| encode_metadata_reset_cursors | 6.091–6.343 ms |
| allocate_spill_large_edges | 6.323–6.458 ms |
| parse_scatter_release_builder | 141.496–164.997 ms |
| read_queries | 0.136–0.173 ms |
| parallel_queries_including_arenas | 351.010–367.282 ms |
| exact_fallback | 0.529–0.567 ms |
| output | 0.223–0.248 ms |

Query time includes private directory/arena allocation and reuse. The graph's measured payload is **55,530,828 bytes**, including 3,283,593 spill codes and 52,012 full-weight records on public full seed 1. These are observed data sizes, not program constants. A pooled-CSR operation profile counted about 7.6M pops and 21.0M relaxations; its wide fallback visited fewer than 1,000 nodes. Graph work and queries both mattered, so optimizing Dijkstra alone could not meet the target.

## Experiments, hypotheses and decisions

All times in this section are medians within their own shuffled batches, full seed 1 unless noted. Every timed output matched. Local experiments have **no official run IDs**; they are not presented as board history.

| Experiment | Measured evidence | Decision |
|---|---|---|
| Parallel two-pass construction, eliminate full edge triples | First batch baseline 1.144 s → 0.853 s | Keep parallel parsing/construction |
| 32-bit edge words, full record escape for weights ≥1024 | Packed/dense 0.845 s, anonymous 133.9 MiB vs 171.5 MiB baseline | Keep exact packing |
| Fixed open-addressed visited state | 0.875 s, anonymous 92.7 MiB | Useful memory improvement, superseded |
| Paired state and growing exact hash fallback | Removed the graph-sized uint64 fallback allocation; atomic-builder version sampled ~65.9 MiB | Keep visited-node storage, refine layout |
| Pooled chained directory + contiguous state/link arenas | Shuffled batch: baseline 1.173 s, pooled atomic 0.753 s | Keep: no per-node allocation, no per-worker n-sized distances |
| Node-range ownership, private byte/nibble degrees | Similar time to atomic constructor, but ~86.6 MiB anonymous vs ~65.4 MiB | Reject at that stage on K |
| Exact unqueried degree-two chain contraction | Removed 23% of nodes; original preprocessing added ~163 ms; refined version 0.732 s vs 0.735 s without contraction but ~110 vs ~86 MiB | Reject: did not repay time/memory/source |
| BFS/label-propagation reorderings | Example batch: plain pooled 0.704 s /65.4 MiB; BFS 0.707 s /111.2 MiB; four label rounds 0.671 s /111.2 MiB | Reject on K despite faster query phase |
| 256-way linked radix buckets and bitmap minima | Prototype 0.676 s vs binary 0.740 s trimmed mean; clean comparisons later favored binary on K | Preserve experiment, use binary in final |
| Shared atomic query state, retry collisions exactly | 236 collisions in a profile; exact retry phase 1.173 s vs fast phase 0.539 s | Reject: duplicated work outweighed sharing |
| Byte degree counts + explicit wrap records | Example batch 0.934 s vs 1.119 s with uint32 counts | Keep; final phase ~65–74 ms |
| Byte relative cursors with CSR | 1.000 s vs 0.934 s with wide cursors, despite a few MiB saved | Reject for CSR; useful with inline slots |
| 1 MiB/worker buffered pread vs mmap | 1.217 vs 1.248 s, but ~6 MiB more anonymous memory and more source | Keep mmap on K |
| First-digit parser, delimiter rule from frozen grammar | 1.135 vs 1.248 s in that batch, same memory | Keep; also tested on all edge cases |
| Hash-directory sizes 2^14, 2^16, 2^18 | No stable K win over 2^16; larger directory used more memory and was slower | Keep 2^16 |
| Explicit aligned huge-page allocation and fixed malloc threshold | 0.660/0.681 vs 0.638 s default, with no useful memory gain | Reject; normal allocations already sometimes had THP backing |
| `-march=native -flto` together | 0.727 vs 0.638 s | Keep existing x86-64-v3/Alder Lake flags |
| GCC PGO, sample seeds 2 and 7 | Aggregate times confounded by load spikes; median paired PGO/plain ratio 1.026 | Reject as unproven, no training dependency shipped |
| Minimum-key and alternating bidirectional expansion | 0.733/0.665 vs 0.638 s for queue-size balancing | Keep smaller-queue expansion |
| Inline first 1/2/3 edges, CSR spill | 0.650/0.595/0.582 s vs 0.707 s CSR; 63.6/64.1/68.7 MiB anonymous | Keep two inline edges for K |

The pooled-CSR worker matrix tested 1/2/4/6/8/12 query workers and construction workers. Query medians were 2.276/1.291/0.772/0.709/0.748/0.691 s, with 12 workers raising sampled memory from ~65.4 to ~72.0 MiB. Build medians were 1.161/0.969/0.897/0.839/0.724/0.794 s in a separate, noisier batch. These are not final-layout target-CPU measurements. Default six workers matches the target's physical P-cores; an official 6/12 comparison remains worthwhile.

## Correctness and contract

Final release and ASan/UBSan suites pass with query worker counts 1, 4 and 6:

- Seven official hand-written cases, including direction, zero paths, parallel edges, self-loops, isolated nodes and sums above uint32.
- Eighty random directed multigraphs, all pairs, independently checked with Floyd–Warshall (test RNG seed 77019).
- Compact cutoff and wide arithmetic, 3,000 queries reusing arenas, empty query files.
- Degrees 255/256/257/512/1025, parallel edges, weight 1023/1024 escape boundary.
- Radix carries across 255/256 and 65535/65536, and long chains of UINT32_MAX weights.
- Tiny generator seeds 1/2/7/42 and sample seeds 2/7 checked against the frozen Python reference; public sample seed 1 checked repeatedly against its published hash.
- Repeated full public seed-1 output matches and final executable rechecked from `track-d/main.cpp`.

| Dataset | distances.txt SHA-256 |
|---|---|
| Tiny seed 1, reference checked | `ecaca77d3221fe3928109da36480cfdb39f288f2e57ecbc61d3d7706d5cb4bd5` |
| Sample seed 1, published | `a4c76ed030bc7e365db33191f6cf74708fba7f25eb48d64c8c4279f2e57046a0` |
| Full seed 1, published | `a23dbdf0a3e49cc8901310a9ee1dc6a9a569a85297b6da75c2a7002cb2ed277e` |
| Sample seed 2, reference checked | `70e94813fc6fbee12c22cc040bc4a4629c70508d13eaab2bfabbdd75e1d51aed` |
| Sample seed 7, reference checked | `88bf36c94620505d1beafe7cd56a08dc2d261b71f9824d2f764d5ce6e5d7872e` |

LeakSanitizer is disabled (`ASAN_OPTIONS=detect_leaks=0`) because this sandbox's tracing restrictions prevent it from running. Address and undefined-behavior checks pass; leak detection itself is not claimed.

The frozen Dockerfile allowlist checker passes and the host executable is static. `check.py d --data ... --expected ...` was attempted, but Docker refused `/var/run/docker.sock` access before the offline build. No actual new Docker sandbox gate pass, cgroup M, official S or K is claimed. The Dockerfile and its flags/images are unchanged from the official baseline. No third-party dependency was added. No A/B/C files or Git history were modified.

## Next official measurements

Submit the concrete candidate when authorized, record the new run ID, and replace projections with the real T/M/S/K. Then compare six and twelve query workers and test GCC 15 on the allowed image if useful. Queries and graph scatter are now the largest measured costs. Changes that only accelerate queries while adding more preprocessing or memory must still repay their entire-run K cost.

The complete baseline and all alternative sources are preserved in `benchmarks/track-d-v2/`. The smaller CSR candidate is `final-binary.cpp`; the fastest inline public-data variant can be reproduced from `inline.cpp` with `-DINLINE_EDGES=3`. No experimental source is in the scored build context.


<details>
<summary>Archived initial development log, before the entrant committed 67e7631</summary>

The following records describe the earlier development session, not the current implementation. Its raw measurements remain in `measurements.json`.

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

</details>
