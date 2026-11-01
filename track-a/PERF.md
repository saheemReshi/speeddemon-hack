# Track A: performance log

The prior parallel implementation passed official run **67e7631 · Track A**, reported by the team as **150 ms, 58 MB, 200 source lines, K 15.888, machine score 60.054**. The current contiguous candidate has not been run on the board. Earlier local history follows; the latest investigation is recorded at the end. Native RSS is not official cgroup memory.

| Step | Change | Board run ID | Local full wall (s) | Local peak RSS | Correctness |
|---|---|---|---:|---:|---|
| 0 | Organisers' Python reference | pending | not measured here | not measured here | published full SHA256 |
| 1 | C++20 mmap parser, endpoint IDs, per-group latency vectors and sort | pending | 0.605 | 264,840 KiB | seed-1 tiny, sample, full hashes matched |
| 2 | Profiled baseline; made no speculative algorithm change | pending | 0.58–0.61 in three instrumented runs | not measured | full hash still matched |
| 3 | Final build with static libstdc++ and empty-input fix | pending | 0.590–0.621 over five runs | up to 263,320 KiB | four datasets and empty input passed |

Profiling the full seed-1 input with `-DPROFILE` gave parsing and grouping at 0.408, 0.380, and 0.383 s; key sorting at 0.045, 0.041, and 0.041 s; and latency sorting plus output at 0.160, 0.160, and 0.159 s. Parsing is the main remaining bottleneck. The input mapping itself is 238,836,234 bytes, so resident input pages account for most of the measured RSS. No unmeasured optimization was retained.

## Correctness record

| Dataset | Input SHA256 | Output SHA256 | Result |
|---|---|---|---|
| seed 1 tiny | `a99dd14856a3e6e408097dabc1b3ad5ac5a3801fb12f388bacb2802c8591339f` | `c24fa507c963eaeac9224fd8435019b4f4e8c002ece7e32f316681a3957eb92f` | published hash matched |
| seed 1 sample | `7b22c678acceba2b82120394afe27e827ca4083a677a8e51147defa854c86847` | `ca6311bbeb536fa55d85265bf6fd3d24cc9586f13de1ac73bd2a23ac3893aac3` | published hash matched |
| seed 1 full | `7ec80ba45bceef51ac7f79d3a14521fc6187b2dcb87b5e5542d08063fe5d962a` | `495ca09682b9673ef836a506496177680bb4a3af54255174befddb1f83481cd3` | published hash matched |
| seed 2 sample | `72422fd1ec489252d2522619750558f4a472547c696a27616300af242e950ac3` | `fd26778fa5e70fac2905a0a0f0142bd26ca4e6f510dfe182a881e525286ff12f` | official reference byte match |

The official `check.py` build was attempted with the tiny published hash. It accepted the pinned base digests but Docker daemon access was denied, so there is no container timing or memory measurement yet.

The five final full-run wall times were 0.610173, 0.621262, 0.611738, 0.591612, and 0.589726 s. The last full output again had SHA256 `495ca09682b9673ef836a506496177680bb4a3af54255174befddb1f83481cd3`. The empty-input check exited 0 and produced a zero-byte file.

## November 1: measured parallel improvements

Environment: native x86-64 Linux on Intel Core i5-12450H (4 P-cores and 4 E-cores, 12 logical CPUs), GCC 16.2.1. These runs were not pinned and clocks were not fixed; they establish local improvements, not scoring-box performance. Compiler flags were `-O3 -std=c++20 -DNDEBUG -pthread -static-libstdc++ -static-libgcc`. Each comparison alternated old/new five times, timed process launch through exit with Python `perf_counter`, and checked the full output hash after every run. The reported mean drops the fastest and slowest run of each candidate.

| Change | Board run ID | Before mean (s) | After mean (s) | Highest after RSS (KiB) |
|---|---|---:|---:|---:|
| Six private parser workers, merge groups, sort endpoints once instead of sorting all group keys | pending | 0.617321 | 0.323969 | 272,008 |
| Exact successive percentile selection for groups of at least 64; sort smaller groups | pending | 0.336097 | 0.245195 | 272,956 |
| Compute percentiles and format hours in parallel; write buffers in order | pending | 0.270833 | 0.198678 | 280,212 |

The final candidate's five full wall times were 0.202547, 0.189632, 0.203856, 0.220533, and 0.188225 seconds. Every output matched `495ca09682b9673ef836a506496177680bb4a3af54255174befddb1f83481cd3`. All improvements were retained after measurement. No board run IDs exist yet.

RSS above includes resident input pages and is **not** the competition's M. The participant clarification specifies cgroup v2 `memory.peak`, excludes host-prewarmed input pages, and includes allocations and tmpfs writes. Do not use these native RSS values to predict M or K. The parallel version adds worker state and hour buffers; measure their actual cgroup cost on the board.

The official checker was retried on full seed-1 input after optimization. Both FROM digests passed its live allowlist check; Docker again failed with permission denied on `/var/run/docker.sock`, before building. Container execution and official cgroup memory remain unverified. The next useful work is a board measurement and phase profiling of the parallel version, then evaluating parser hashing, allocation, and worker-count tuning on the six target P-cores. No native-CPU or SIMD flags have been assumed to improve speed.

Final regression checks: seed-1 tiny, sample and full matched the published output hashes, seed-2 sample matched the prior reference hash, a deterministic complete shuffle of seed-2 sample matched the same hash (exercising arbitrary disorder and cross-worker merging), and empty input exited 0 with zero output bytes. The native build was clean with `-Wall -Wextra -Wpedantic`; `git diff --check` passed. Source was formatted with LLVM clang-format style.

## Follow-up: contiguous storage investigation

### Preservation and method

Before edits, the working source, Dockerfile, manifest and docs were copied to `benchmarks/track-a-baseline/` outside the scored folder. Repository HEAD at preservation was `642e2665aa9e291b4885c73f246715844239d576`; a source hash is in `benchmarks/track-a-results/environment.json`. No Git history was changed. The baseline board run is `67e7631`; every new experiment's board ID is **pending**.

Local host: i5-12450H, GCC 16.2.1, Linux x86-64. Controlled experiments pin all child threads to CPUs 0–7: four physical P-cores with SMT, unlike the target's six P-cores. Turbo/governor were not controlled. Each comparison rotates candidate order over five runs, pre-reads the input, times process launch through exit, verifies every output hash, and averages the middle three. Memory is child `wait4().ru_maxrss`, including resident input; it is not the official M. The initial layout comparison was unpinned and is identified separately below. Scripts and every raw timing/RSS sample are preserved outside the scored folder in `benchmarks/` and `benchmarks/track-a-results/`.

### Evidence and experiments

The baseline diagnostic counted 549,486 C++ allocations while parsing, 555,175 after merging and 555,253 after formatting. Cumulative requested bytes reached 82,055,324. Atomic diagnostic counters add overhead, and cumulative requests are not peak live memory. This implicated vector growth and allocator traffic. The two-pass candidate counts groups, computes disjoint worker slices, and fills one exact-size latency array. No latency vectors need merging.

| Experiment | Local control mean (ms) | Candidate mean (ms) | Decision / raw results |
|---|---:|---:|---|
| Contiguous storage, flat table and bounded output (initial unpinned test) | 194.618 | 182.707 | Continue; RSS fell from 280,268 to 257,516 KiB; `layout-results.json` |
| CRC hash alone with library delimiter scans | 182.707 | 184.766 | No gain; not retained alone |
| Combined word delimiter/CRC scan and fixed method lengths | 151.725 | 128.586 | Retain; `scan-results.json` |
| Transpose worker counters to hour-first order | 128.395 | 131.642 | Reject; `flags-results.json` |
| Native compilation plus LTO on the transposed layout | 131.642 | 134.535 | Reject; `flags-results.json` |
| Word decimal latency parsing with scalar tails | 135.295 | 130.084 | Retain; `latency-results.json` |
| LTO alone | 135.295 | 134.400 | Within noise; reject complexity |
| GCC PGO, trained on official full seed 3, evaluated on seed 1 | 126.764 | 132.743 | Reject; `pgo-results.json` |
| Private `pmr::unsynchronized_pool_resource` in baseline | 177.405 | 176.074 | No material win; slightly higher RSS; reject; `pool-results.json` |

The PGO experiment used `-fprofile-generate`, atomic profile updates, then `-fprofile-use -fprofile-correction -Werror=missing-profile`. It trained on generated input, not cached answers. Neither profile data nor a generator is required by the final build. No `-march=native`, LTO or PGO is retained. Final flags add only `-msse4.2` to the prior `-O3 -std=c++20 -DNDEBUG -pthread -static-libstdc++ -static-libgcc` build for the specified Intel target.

### Worker counts

| Workers | Earlier contiguous parser mean (ms) | Final parser mean (ms) | Final max RSS (KiB) |
|---:|---:|---:|---:|
| 1 | 553.299 | not repeated | — |
| 2 | 300.841 | not repeated | — |
| 4 | 187.227 | 167.222 | 254,736 |
| 6 | 152.779 | 128.463 | 257,744 |
| 8 | 142.686 | 139.800 | 261,012 |
| 12 | 164.189 | 137.480 | 267,404 |

The final experiment supports six workers locally: eight and twelve cost more time and memory. Four saves a little RSS but costs much more time. Six remains the default; `-DWORKERS=N` allows controlled target-machine tests. These local SMT results do not establish the optimum on six target physical cores. Files smaller than 8 MiB use one worker to avoid thread startup overhead.

### Final paired results and remaining bottlenecks

| Full dataset | Baseline mean (ms) | New mean (ms) | Baseline max RSS (KiB) | New max RSS (KiB) |
|---|---:|---:|---:|---:|
| seed 1 | 178.358 | 131.899 | 280,540 | 257,744 |
| seed 3 | 167.758 | 120.557 | 283,856 | 260,780 |

Every timed output matched its expected hash. The new allocation diagnostic recorded **130 C++ allocations**, 13,016,862 cumulative requested bytes, plus the separate **12,400,000-byte latency mmap**. Phases were count 44.335 ms, merge/prefix 8.055 ms, fill 40.643 ms, and selection/format/ordered writes 21.014 ms. The dominant remaining cost is two-pass scanning and repeated endpoint lookup. Workers share the input read-only; cursor slices and output groups are disjoint. Only ordered writes take a lock. There is no per-line synchronization. Whole completed latency pages are discarded; pages straddling hour boundaries are protected. One hour buffer per worker bounds formatted output memory.

The 20–30 ms / 15–20 MB ambition has **not** been demonstrated. The measured improvement is 26–28% in local wall time and about 22 MiB less local RSS, not an official time or memory prediction. The next work should measure this candidate on the board, profile on the target, and investigate avoiding repeated lookup without restoring large intermediate record buffers. Cache-miss and bandwidth counters were not collected: `perf` was unavailable locally. No claim of target-cache optimality is made.

### K and submission status

The added code must pay for itself in time and memory. For the relevant uncapped-memory range and source sizes above the 56-line reference, with zero dependencies unchanged:

`K_new / 15.888 = (0.150 / T_new_seconds)^0.50 * (58 / M_new_MB)^0.20 * (200 / S_new)^0.15`.

Use the **new official** T, cgroup M and formatted S in that expression. Raw local RSS or raw file line counts are not substitutes. The candidate reduces both measured local time and RSS; increasing worker count without a speed benefit loses on both axes. The retained code is larger, and its exact S and resulting K still need the official harness. No official result or run ID has been invented for this version. No commit or push was made.

### Correctness

All eleven final checks passed (`benchmarks/track-a-results/correctness.json` records hashes and output bytes): published seed-1 tiny/sample/full; seed-2 sample; full seed 3 against the official Python reference; seed-7 tiny/sample and seed-17 tiny against the reference; a complete shuffle of seed-2 sample; an adversarial exact-page-sized file; and empty input. The adversarial file exercises every status from 100 to 599, all methods, endpoint lengths around word boundaries and the 125-byte limit, latency 0 through uint32 max, the 63/64 selection threshold, nearest ranks including n=101, date rollovers, and an endpoint tail at the mapping boundary.

Additional reference hashes:

- full seed 3: `5207056c6c7479374bc695261be9c4d37e13f2e49ece1f27669694ab122bcb24`
- sample seed 7: `39bfccc5334c93cbe0c762f5e6c3300e91ca2e266567561da3176c038cee49f7`
- tiny seed 7: `4ba05ab1a7a257e847c25beded9bf3cc658fb1d41f398b84a8a983a7b91f026d`
- tiny seed 17: `38cc17f9205c0d9f6b9f16fa19f722e9fc348a07258b96ac56c7a55d1c62e4f6`
- adversarial page boundary: `d56e628ed4af07361a889469f4fe268b4f3ac4b656deee3e8936bb172e763c87`

Final container-check attempt: `python3 check.py a --data /tmp/sd-a-full-seed1 --expected 495ca09682b9673ef836a506496177680bb4a3af54255174befddb1f83481cd3` confirmed both FROM digests are allowlisted, then failed before building with permission denied on `/var/run/docker.sock`, including through escalated execution. The final native build matched the full hash and was warning-free with `-Wall -Wextra -Wpedantic`. Container validation and new official T/M/S/K remain pending; no commit or push was made.
