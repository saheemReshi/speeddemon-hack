# Track A: performance log

Board run IDs are pending. The timings below are local native runs on this workstation, not scoring-box numbers. The direct binary used GCC 16.2.1, `-O3 -std=c++20 -DNDEBUG`; child peak RSS came from Linux `getrusage(RUSAGE_CHILDREN)`. The output file was on `/tmp`.

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
