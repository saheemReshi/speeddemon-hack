# Track C: performance and correctness log

## Status, 2026-11-01

Correct native baseline and optimized C++23 release established. The user
authorized continued native optimization while fixing Docker access separately.
Official Docker validation remains blocked by host socket permissions, including
outside the execution sandbox. No leaderboard run ID, commit or push exists.

| Step | Implementation | Leaderboard run ID | Official T | Official M | Points |
|---|---|---|---|---|---|
| 0 | Published Python reference | Pending | Not measured here | Not measured here | — |
| 1 | C++23 native baseline | Pending | Pending Docker access | Pending Docker access | — |
| 2 | Shared exact-key table, SIMD CSV, 12 workers | Pending | Pending Docker access | Pending Docker access | — |

The starter's reference figures were 14.44 s / 367 MB; they are supplied context,
not a run measured in this session. No K, formatted source count or official
memory result is claimed.

## Baseline decisions

- A specialized CSV scanner follows the frozen grammar and exploits guaranteed
  unquoted numeric fields. `memchr` searches literal spans; doubled quotes are
  decoded for names and counted for notes. Embedded newlines never split records.
- Dense byte region lookup avoids hashing each record's integer region ID.
  Compact country IDs permit fixed aggregate arrays and 256-bit name bitmaps.
- Each distinct decoded name is stored once globally, rather than once per
  country. Exact comparisons resolve hash collisions. No note strings are stored.
- `mmap` avoids a full input copy. Consumed pages are dropped in approximately
  1 MiB batches to bound RSS. Buffered input has not yet been benchmarked.
- The release build uses GCC 16.2.1, C++23, `-O3 -DNDEBUG -flto
  -march=x86-64-v3 -static`. These flags are the baseline; no isolated LTO,
  architecture flag or PGO speedup has been measured. Docker additionally strips
  the binary with `-s`.
- Single-threaded execution is intentional for the first correctness checkpoint.
  Parallelism and explicit SIMD have not been measured. No external runtime
  dependencies are used.

## Correctness

The official unmodified generator/reference suite passed **14 tests**. The
validation adapter also passed **14 tests**, replacing direct calls to the
reference aggregation entrypoint with the C++ executable. This includes the
worked example, BOM-in-data and absent-final-terminator rules, sorting, and
independent-parser comparisons on tiny/sample seed 1. Reference subprocess
determinism tests still execute the published Python reference.

The initial deterministic 2,000-record differential case exercised quoted
commas/quotes, literal BOMs, CR/LF and non-ASCII UTF-8 inside text, empty values,
mixed record terminators, unknown regions and a final record without a terminator.
Its C++ and reference outputs matched byte-for-byte. The final case expands this
to **2,002 records**, adds explicit quoted/unquoted copies of one nonempty name,
and also emits legal nonempty unquoted fields. The final release passes it.

| Scale | Seed | Output SHA-256 | Comparison |
|---|---|---|---|
| sample | 1 | `9035fed36823232dd41ae2d1b9d6d21aa7c73f49d8e95647e953a89d5ebe9a66` | Published hash |
| full | 1 | `d8088b1cd42ba1338b1345b107b55db7b96ef1f5066f3ff64fd26a8912a0f536` | Published hash and fresh reference |
| tiny | 2 | `33751caa2d533502b540bad1ad914bce49118ca9732c89c37892541c91b6713a` | Fresh reference |
| tiny | 42 | `7d151bcd945ddf65dcddaa4b956f75435d638a411bddb7cdb42f4fb7e2d8e816` | Fresh reference |
| tiny | 2026 | `b4878698f3b971202c9e090a0d46cfc606e9f11c77ff946188352f8d91a9e2d8` | Fresh reference |
| sample | 2 | `f389938275df1b495a38c4375fe741795df180e64617e01e2cf77a95a56982a3` | Fresh reference |
| full | 2 | `f99e6f9133abcb324258614db301967398b0da6e628cd882c164a1665f9317be` | Fresh reference |

The final release was rechecked against every row above, not only the initial
baseline. Published seed-1 input hashes also match the spec:

| Input | SHA-256 |
|---|---|
| full records | `36166f4ac44101a22ebf25b4f949436a681a2698cb0f2ae1f7f6853010259f48` |
| full regions | `414c57c4f3ec968f9bce28c59eebfdf57a9425322b468db7ee964c18c4852fe1` |
| sample records | `07bc636857b70556921be08d406f887eb07dbdacb8384587ed7fd3b52addfd86` |
| sample regions | `1bd3904fd5eaed7a4dc1ae761bf369521ce5f384e4c400342919d6edf9e5ece1` |

ASan and UBSan passed the final 14 adapted official tests and 2,002-record case.
LeakSanitizer initially failed because it cannot operate under the environment's
process tracing; rerunning with `ASAN_OPTIONS=detect_leaks=0` passed. Leak detection
is therefore unverified. No submission-source change was needed for this failure.
ThreadSanitizer passed a twelve-thread grammar run. A second TSAN build forced
all fast-table hashes to zero and reduced capacity to eight slots; all six
official handwritten tests and the 2,002-record case passed with exact reference
output and no race reports. This exercises both atomic insertion and the
mutex-protected overflow path.

## Initial baseline measurements

Host: Intel Core i5-12450H, 12 logical CPUs, Linux x86-64. This is not the scoring
machine. No CPU pinning or fixed frequency was applied. Data was prewarmed and
one untimed executable run preceded five sequential timed runs. The native
measurement helper times fork through child exit with `CLOCK_MONOTONIC`, and uses
`wait4` for that child's peak RSS. This includes fork overhead; official timing
starts closer to exec. Each timed output was checked against the published hash.

| Run | Wall seconds | Peak RSS KiB |
|---|---:|---:|
| 1 | 1.240118493 | 12036 |
| 2 | 1.083399088 | 12016 |
| 3 | 1.123946324 | 11816 |
| 4 | 1.326253974 | 11944 |
| 5 | 1.243867317 | 11832 |

Middle-three mean: **1.202644045 s**. Highest RSS: **12,036 KiB** (11.75 MiB).
The wall-time spread exceeds the official stability criterion, so this is an
initial noisy local benchmark, not a stable official T. No claim is made that
this is near the cited 20 ms leader. Official cgroup memory includes different
charges than RSS and cannot be inferred from this number.

Measurement corrections: `/usr/bin/time` was absent, so no package was installed.
An initial Python `posix_spawn`/`wait4` controller reported 621,508 KiB because the
memory-heavy reference-test controller contaminated the fork/exec high-water
mark. Those memory values were discarded. Its five wall times were
1.186668321, 1.193513835, 1.066465457, 1.230888803 and 1.253339526 s. A fresh native
parent fixed the memory measurement; the table above is the retained batch.

## Initial commands actually executed

Run from the repository root. Generated data and reports are under ignored
`data/c/`; benchmark tooling is outside the submitted `track-c/` context.

```sh
python3 ../speed-demon-2026/generators/c_join.py --seed 1 --scale tiny --out data/c/tiny-seed1
python3 ../speed-demon-2026/generators/c_join.py --seed 1 --scale sample --out data/c/sample-seed1
python3 ../speed-demon-2026/generators/c_join.py --seed 1 --scale full --out data/c/full-seed1
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s ../speed-demon-2026/tests -p 'test_c_*.py'
g++ -std=c++23 -O3 -DNDEBUG -flto -march=x86-64-v3 -static -Wall -Wextra -Wpedantic track-c/main.cpp -o /tmp/track-c-join
mkdir -p data/c/native-out
/tmp/track-c-join --data data/c/sample-seed1 --out data/c/native-out
sha256sum data/c/native-out/aggregates.txt
/tmp/track-c-join --data data/c/full-seed1 --out data/c/native-out
sha256sum data/c/native-out/aggregates.txt
PYTHONDONTWRITEBYTECODE=1 python3 benchmarks/track-c/validate.py --binary /tmp/track-c-join
g++ -std=c++23 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra track-c/main.cpp -o /tmp/track-c-join-sanitized
PYTHONDONTWRITEBYTECODE=1 python3 benchmarks/track-c/validate.py --binary /tmp/track-c-join-sanitized --tests-only
gcc -O2 -Wall -Wextra benchmarks/track-c/measure.c -o /tmp/track-c-measure
PYTHONDONTWRITEBYTECODE=1 python3 benchmarks/track-c/validate.py --binary /tmp/track-c-join --benchmark-only
ASAN_OPTIONS=detect_leaks=0 PYTHONDONTWRITEBYTECODE=1 python3 benchmarks/track-c/validate.py --binary /tmp/track-c-join-sanitized --tests-only
cmp track-c/c_join.py ../speed-demon-2026/reference/c_join.py
cmp track-c/run.py ../speed-demon-2026/reference/run.py
git diff --check
```

The validation script imports the official generator/reference for additional
seeds rather than modifying them. On a fresh checkout, compile the measurement
helper before invoking the full validation script.

Docker diagnostics executed: `docker version`, `docker image ls --digests`,
`docker context ls`, `id`, `getent group docker`, and
`stat -c '%A %U %G %n' /var/run/docker.sock`. Direct and escalated Docker access
were both denied. `sudo -n docker version` and `sudo -n docker image ls --digests`
both failed because a password is required. No permissions or system
configuration were changed.

The following required command was attempted both normally and escalated:

```sh
python3 check.py c --data data/c/full-seed1 --expected d8088b1cd42ba1338b1345b107b55db7b96ef1f5066f3ff64fd26a8912a0f536
```

The escalated attempt printed `ok every FROM is allowlisted`, then Docker
reported socket permission denied. The offline build never started, and sandbox
execution, image size, Docker filesystem diff, official timer/cgroup measurements
and static S/D tools remain unverified. Rerun this command after Docker access is
enabled, then obtain approval to commit/push the first valid checkpoint.

## Optimization experiments

Every timing below uses the full 3,320,000-record seed-1 input. Every output in
every batch was verified against the published hash. Batches prewarm both input
files and run one untimed warmup per candidate, then interleave candidates in
deterministic randomized order. Five-run summaries discard the highest/lowest
and average the middle three. The nine-run confirmation averages the middle
seven. Absolute times vary between batches; compare candidates within a batch.
Raw JSON reports and preserved source/binary checkpoints are under ignored
`data/c/`. Existing untracked benchmark files were left intact.

### Flat table and encoded keys

Hypothesis: general-purpose node allocation and pointer chasing dominate name
lookup. A flat slot array, contiguous name metadata and a byte pool improved
time, at a modest RSS increase. Kept as an intermediate implementation.

| Candidate | Mean seconds | Max RSS KiB |
|---|---:|---:|
| Original `unordered_map` baseline | 0.989486 | 12132 |
| Owned-key flat table | 0.709507 | 15120 |

Next hypothesis: comparing canonical CSV interiors avoids decoding names.
Canonical identity is exact because all literal quotes are doubled and
unquoted values cannot contain quotes. Byte lengths are still computed after
unescaping. Encoded keys alone did not help this batch; adding CRC32 did.

| Candidate | Mean seconds | Max RSS KiB |
|---|---:|---:|
| Decoded keys, standard hash | 0.804851 | 15172 |
| Encoded keys, standard hash | 0.827373 | 15188 |
| Encoded keys, CRC32 | 0.701869 | 15160 |

### Parallel private tables

Hypothesis: partitioning records and merging independent bitmaps will scale
parsing. Quote parity locates valid boundaries; each worker owns its aggregates
and names. This passed all correctness checks but duplicated names and incurred
a merge pass. The AVX2 partition scan also brought mapped file pages into RSS.

This batch used `taskset -c 0-7`: four local P-cores including SMT, not the
scoring machine's six P-cores. High noise makes absolute times unrepresentative.

| Threads | Mean seconds | Max RSS KiB |
|---:|---:|---:|
| 1 | 1.582671 | 13352 |
| 2 | 0.965178 | 109424 |
| 4 | 0.598780 | 164176 |
| 6 | 0.552694 | 186308 |
| 12 | 0.505998 | 216816 |

### Shared immutable keys and atomic bitmaps

Hypothesis: retaining pointers into the input and atomically setting country
bits removes name copies, per-worker tables and the merge. Relaxed operations
are sufficient: the mapped bytes are immutable before thread creation, bits
only transition from zero to one, and reductions occur after all workers join.
Each first-set bit contributes exactly one local distinct increment.

This unpinned interleaved batch favored the shared design at larger thread
counts. The local CPU has four P-cores plus four E-cores; final scoring may rank
thread counts differently. Twelve threads are within the published allowance.

| Candidate | Mean seconds | Max RSS KiB |
|---|---:|---:|
| Owned-key single-thread CRC32 | 0.742175 | 15168 |
| Private tables, 6 workers | 0.306729 | 187948 |
| Shared table, 1 worker | 0.915493 | 209428 |
| Shared table, 4 workers | 0.322179 | 209448 |
| Shared table, 6 workers | 0.238293 | 209436 |
| Shared table, 12 workers | 0.194735 | 209436 |

Native RSS includes the full file mapping. The main shared table is 262,144
40-byte slots: **10 MiB**, plus region lookup, aggregates, stacks and runtime.
The official harness prewarms input files before creating the run cgroup, so
file-cache ownership differs from RSS. **No official M is inferred from RSS.**
All file pointers remain valid until after worker joins. A bounded probe prefix
and exact-key standard map under a mutex handle overflow without a distinct-name
cap or unbounded allocation of successively larger tables.

### Parser, hashing and table layout

Hypotheses: remove redundant numeric bounds checks where a comma/dot is
guaranteed; align one country's aggregates to one cache line; avoid libc call
overhead for short quote searches and comparisons; reduce hash work; skip an
atomic RMW when a country bit is already set. Table-size alternatives trade
probing against footprint. The following candidates used six workers.

| Candidate | Mean seconds | Max RSS KiB |
|---|---:|---:|
| Previous shared implementation | 0.237270 | 209556 |
| Guaranteed numeric delimiters + aligned aggregates | 0.224838 | 209412 |
| Above + SSE 16-byte quote search | 0.215565 | 209428 |
| Above + AVX2 32-byte quote search | 0.218641 | 209388 |
| Above + length/first/last-word CRC32 | 0.204736 | 209424 |
| Above + skip already-set bit | 0.215819 | 209392 |
| Above + 131,072 slots | 0.253404 | 204244 |
| Above + 524,288 slots | 0.215656 | 219664 |

Retained the smaller SSE search, short CRC hash and bit check in combination.
CRC collisions always receive exact comparisons, including middle bytes ignored
by the fast hash. The forced-collision tests verify this explicitly. Large
fields still work; SIMD loads are bounded by the parser range and long spans
fall back to `memchr`.

### Combined changes, input and region lookup

Hypothesis: combine successful changes and test short comparisons without
assuming SIMD or parallelism must win. For values up to 16 bytes, overlapping
word comparisons cover every byte without overreading. Longer values use
`memcmp`. Dense region lookup was compared with a reserved standard hash map.

The owned-buffer input candidate reads the entire file into anonymous storage,
preserving stable pointers. This measures a copy-based alternative; a reusable
streaming buffer was not implemented because this design retains key pointers.
An owned buffer would also charge roughly the full input size as anonymous
memory, unlike prewarmed file pages. It was slower and rejected.

| Candidate | Mean seconds | Max RSS KiB |
|---|---:|---:|
| First/last CRC only, 6 workers | 0.210466 | 209408 |
| Combined parser/hash/bit changes, 6 workers | 0.193028 | 209360 |
| Combined changes, 12 workers | 0.157525 | 209368 |
| Above + short exact comparisons | 0.146865 | 209428 |
| Owned input buffer, 12 workers, libc comparisons | 0.264164 | 209596 |
| Hashed regions, 12 workers, libc comparisons | 0.209409 | 216280 |

Retained `mmap`, dense regions, twelve workers and short exact comparisons.
Rejected private name tables, owned input buffering, hashed regions, wide quote
search and the larger table. The half-sized table remains a possible K tradeoff
to revisit with actual official memory measurements, not a retained change.

### Compiler options and PGO

All builds are C++23, static and threaded. PGO used GCC atomic profile updates,
trained on generated **sample seed 2**, then recompiled with profile use,
correction and `-Werror=missing-profile`. No input/output constants were embedded.

| Candidate | Five-run mean seconds |
|---|---:|
| `-O3 -flto -march=x86-64-v3` | 0.149329 |
| `-O2` instead | 0.151423 |
| No LTO | 0.159551 |
| `-march=native` instead | 0.156432 |
| Add `-mtune=raptorlake` | 0.143776 |
| PGO | 0.141240 |

The initial PGO/tuning gains did not survive nine-run confirmation:

| Candidate | Nine-run trimmed mean seconds | Max RSS KiB |
|---|---:|---:|
| Base `-O3` + LTO | 0.140905 | 209440 |
| Raptor Lake tuning | 0.146454 | 209428 |
| PGO | 0.146408 | 209600 |
| PGO + tuning | 0.143160 | 209464 |
| Half-sized table, base flags | 0.176237 | 204468 |

**Rejected PGO and CPU-specific tuning.** Final Docker flags remain `-O3
-DNDEBUG -flto -march=x86-64-v3 -static -pthread -s`. No profile generation,
training image, copied generator or profile data is required for the build.

### Final release versus preserved baseline

Final release was rebuilt with the exact Docker flags and reran all seed
comparisons and output hashes. Tested `main.cpp` SHA-256:
`d048f523a3e6c496ab068a2b074fff4d15baf442570d15d461d90775cf8564d7`.
The local file is 449 physical lines; this is **not** the official formatted
code-line S count. `file` confirms the final binary is statically linked and
stripped. In the final interleaved five-run comparison:

| Run | Original baseline seconds | Final seconds |
|---|---:|---:|
| 1 | 0.870494558 | 0.161155469 |
| 2 | 0.835255164 | 0.124509508 |
| 3 | 0.843765111 | 0.136780713 |
| 4 | 0.834593715 | 0.117337287 |
| 5 | 0.838274422 | 0.162946487 |

Baseline middle-three mean **0.839098232 s**; final **0.140815230 s**, approximately
**5.96× faster**. Max RSS: baseline **12,136 KiB**, final **209,440 KiB**.
The increase is primarily the retained input mapping, not a 200 MB owned buffer.

A separate final five-run batch measured 0.139931169, 0.135531820, 0.166158303,
0.136655486 and 0.153527634 s: middle-three mean **0.143371430 s**, maximum RSS
**209,404 KiB**. These are local measurements; no 20 ms result or official K is
claimed. `/sys/fs/cgroup/cpu.max` was unavailable, and no cgroup limit was inferred.

## Optimization and final verification commands

The compiler flags below show the actual experiment builds. Intermediate
source checkpoints are in `data/c/{flat,raw-crc,private-parallel,shared,
all-experiments}-main.cpp`; the current cleaned source contains only retained
paths and the capacity/collision/thread-count test switches. Flags for rejected
paths therefore apply to their historical source, not the cleaned final file.

```sh
perf stat -e cycles,instructions,branches,branch-misses,cache-misses /tmp/track-c-join --data data/c/full-seed1 --out data/c/native-out
lscpu -e=CPU,CORE,MAXMHZ
```

`perf` was absent, so no hardware-counter profile or package installation occurred.
Source/binary snapshots used `cp -n` into ignored `data/c/`; no originals were
deleted. Each experiment compiled with GCC 16.2.1 and common flags
`-std=c++23 -O3 -DNDEBUG -flto -march=x86-64-v3 -static` (adding `-pthread` for
parallel versions). The actual variant flags were:

| Batch | Additional variants |
|---|---|
| Raw keys/hash | `-DNAME_HASH_CRC` versus standard hash |
| Private threads | `-DNAME_HASH_CRC -DJOIN_THREADS=N`, N = 1,2,4,6,12 |
| Shared threads | Same, N = 1,4,6,12 |
| Parser/table | CRC + 6 threads; individually `-DCSV_SIMD_WIDTH=16`, `=32`, `-DNAME_END_HASH`, `-DNAME_SKIP_SET`, `-DNAME_CAPACITY=131072`, `=524288` |
| Combined | `-DNAME_HASH_CRC -DNAME_END_HASH -DNAME_SKIP_SET -DCSV_SIMD_WIDTH=16 -DJOIN_THREADS=12`; compare 6 threads, `-DNAME_INLINE_EQUAL`, `-DBUFFERED_INPUT`, `-DREGION_HASH` |
| Forced collisions | Combined flags plus `-DNAME_FORCE_COLLISIONS -DNAME_CAPACITY=8` |

Each candidate batch ran `python3 benchmarks/track-c/compare.py <binaries>
--results data/c/<batch>-comparison.json`. The private-thread batch alone used
the `taskset -c 0-7` prefix. Actual report names: `flat-comparison.json`,
`raw-crc-comparison.json`, `thread-comparison.json`, `shared-comparison.json`,
`parser-table-comparison.json`, `combined-comparison.json`,
`compiler-comparison.json`, `final-options-comparison.json`,
and `baseline-final-comparison.json`. The final-options batch used `--runs 9`.
All other batches used the default five runs.

The cleaned final source was formatted and compiler/PGO experiments executed as:

```sh
clang-format --style=LLVM -i track-c/main.cpp
g++ -std=c++23 -O3 -DNDEBUG -flto -march=x86-64-v3 -static -pthread -Wall -Wextra -Wpedantic track-c/main.cpp -o /tmp/track-c-final
g++ -std=c++23 -O2 -DNDEBUG -flto -march=x86-64-v3 -static -pthread track-c/main.cpp -o /tmp/track-c-o2
g++ -std=c++23 -O3 -DNDEBUG -march=x86-64-v3 -static -pthread track-c/main.cpp -o /tmp/track-c-no-lto
g++ -std=c++23 -O3 -DNDEBUG -flto -march=native -static -pthread track-c/main.cpp -o /tmp/track-c-native
g++ -std=c++23 -O3 -DNDEBUG -flto -march=x86-64-v3 -mtune=raptorlake -static -pthread track-c/main.cpp -o /tmp/track-c-tuned
mkdir -p data/c/pgo-out
g++ -std=c++23 -O3 -DNDEBUG -flto -march=x86-64-v3 -static -pthread -fprofile-generate=/tmp/track-c-pgo-profile -fprofile-update=atomic track-c/main.cpp -o /tmp/track-c-pgo
/tmp/track-c-pgo --data data/c/sample-seed2 --out data/c/pgo-out
g++ -std=c++23 -O3 -DNDEBUG -flto -march=x86-64-v3 -static -pthread -fprofile-use=/tmp/track-c-pgo-profile -fprofile-correction -Werror=missing-profile track-c/main.cpp -o /tmp/track-c-pgo
```

The combined PGO/tuning build added `-mtune=raptorlake` to the final compile;
the first PGO binary was preserved as `data/c/pgo-base-join`. LTO printed a
serial-LTRANS build warning; it was not a compilation failure.

Final checks actually executed:

```sh
g++ -std=c++23 -O1 -g -march=x86-64-v3 -pthread -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra track-c/main.cpp -o /tmp/track-c-final-sanitized
ASAN_OPTIONS=detect_leaks=0 PYTHONDONTWRITEBYTECODE=1 python3 benchmarks/track-c/validate.py --binary /tmp/track-c-final-sanitized --tests-only
g++ -std=c++23 -O1 -g -march=x86-64-v3 -pthread -fsanitize=thread track-c/main.cpp -o /tmp/track-c-tsan
TSAN_OPTIONS=halt_on_error=1 /tmp/track-c-tsan --data data/c/grammar-cases --out data/c/native-out
cmp data/c/native-out/aggregates.txt data/c/grammar-reference/aggregates.txt
g++ -std=c++23 -O1 -g -march=x86-64-v3 -pthread -fsanitize=thread -DNAME_FORCE_COLLISIONS -DNAME_CAPACITY=8 track-c/main.cpp -o /tmp/track-c-tsan-collisions
TSAN_OPTIONS=halt_on_error=1 PYTHONDONTWRITEBYTECODE=1 python3 benchmarks/track-c/validate.py --binary /tmp/track-c-tsan-collisions --small-only
g++ -std=c++23 -O3 -DNDEBUG -flto -march=x86-64-v3 -static -pthread -s -Wall -Wextra -Wpedantic track-c/main.cpp -o /tmp/track-c-final
PYTHONDONTWRITEBYTECODE=1 python3 benchmarks/track-c/validate.py --binary /tmp/track-c-final
python3 benchmarks/track-c/compare.py data/c/baseline-join /tmp/track-c-final --results data/c/baseline-final-comparison.json
sha256sum data/c/full-seed1/records.csv data/c/full-seed1/regions.csv data/c/sample-seed1/records.csv data/c/sample-seed1/regions.csv
git diff --check
git status --short --untracked-files=all -- track-c benchmarks/track-c
```

No package installation, system configuration change or Git history operation
was performed. This work edited only Track C and `benchmarks/track-c/`. Final
readback showed separate Track B changes to `.speed-demon.toml`, `Dockerfile`
and `PERF.md` that were absent at the initial inspection; none of this work's
commands edited them, and they were left untouched. Tracks A/D and root
`check.py` have no diff. The next required step is the previously shown official
`check.py` command once Docker access is available; leaderboard IDs, official
metrics and approval to commit/push remain pending.
