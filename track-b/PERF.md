# Track B performance and correctness checkpoint

## Second optimization round (in progress)

The user supplied an official board baseline of 383 ms / 99 MB, K=16.889,
61.381 points and S=622. No run ID was supplied. The exact source is preserved in
`../benchmarks/track-b-v2/board-baseline.cpp`; earlier measurements below describe
the first round and must not be confused with this board baseline.

A new native-validated checkpoint uses recyclable posting arenas, inline short
lists, batched prefetched lookup and bounded per-document token IDs for phrases.
Five interleaved full-seed1 runs measured 628.504 ms versus 846.143 ms for the
preserved board source on this laptop, with peak total RSS 322132 versus 342492
KiB. All 35 architecture-comparison runs matched the published full hash.
Tiny, sample and full seeds 2/3 also passed (three runs each), plus 9600 random
differential queries and the eight official hand-written methods. Container
validation and a new official board measurement remain outstanding. This is not
a claim of 75–120 ms on the scoring machine. Further experiments are in progress.
Raw records and source alternatives are in `../benchmarks/track-b-v2/`.

## Status

The final C++23 source is ready for review/commit. No commit, push or official
leaderboard run was performed. Native build, correctness, sanitizer and benchmark
checks pass. **Container readiness remains unverified:** Docker daemon access
fails with permission denied both normally and after sandbox escalation.
Consequently the offline Docker build, root `check.py`, official sandbox gates,
exact static S and official cgroup M have not been measured here.

The final source SHA-256 is `be1f6aa36e1bdcf446f57e3448dda64b1dc609e2f46465fc78415301bfa1b5d1`.
All generated datasets and experimental source/binaries are outside `track-b/`.
The original Python starter is preserved byte-for-byte. The harness recognizes
`b_index.py` and `run.py` as exempt published copies. Team identity is unchanged.
Track A/C/D files were not edited.

## Environment and measurement method

Local Intel i5-12450H, 4 P cores plus 4 E cores, 12 hardware threads; GCC 16.2.1.
The scoring machine is different (six P cores / twelve threads, i5-13500).
Final flags: `-O3 -std=c++23 -DNDEBUG -pthread -march=x86-64-v3
-static-libstdc++ -static-libgcc`. No third-party runtime dependencies, LTO or PGO.

Native timing includes process launch/exec, input parsing, planning, indexing,
query evaluation, output and exit. Input files are warmed before the child runs.
Linux `wait4` supplies per-child peak RSS and CPU usage. Timing uses one blocking
wait. Memory polling is a **separate diagnostic run**, excluded from the final
timing comparison. B_PROFILE phase timing and posting-count reporting are on in
the local benchmark; the normal submission does not emit diagnostics.

Local timings vary substantially. The middle-three aggregation used below is
the harness formula, but the local spread would fail its stability gate. These
are development measurements, not official numbers or scoring-machine forecasts.
No CPU governor, system configuration or package changes were made.

## Correctness

All 15 frozen official Track B tests passed. The final native binary passed all
8 official hand-written test methods, 14,400 differential random queries, and
3,600 additional random queries under AddressSanitizer/UndefinedBehaviorSanitizer.
LeakSanitizer was disabled because this environment uses ptrace; leak checking
is not claimed. Random cases cover 17/32-byte words with equal 16-byte prefixes,
control bytes, UTF-8, upper case, repeated words, all operator combinations,
absent words and phrases.

The final partition suite compares against the official Python reference at
N=0, 1, 65,535, 65,536, 65,537, 131,073 and 250,000, with seven requested worker
settings (1, 2, 3, 4, 6, 12, 32). Small inputs deliberately use one worker. The
large cases exercise actual parallel partitions, maximum 16-bit local IDs,
global sums above 32 bits, empty results and top-ten ordering.

Seed-1 sample/full hashes below equal the frozen published hashes. Tiny uses a
locally run official Python reference. Full seeds 2 and 3 were independently
generated and their complete outputs compared byte-for-byte with separate runs
of the official Python reference. Seed-1 full input hashes also equal the spec.

## Final full-artifact runs

This initial final-artifact batch was hash-checked at every iteration. Timing
variation across batches is preserved rather than selecting only the best run.

| Dataset | Middle mean (3 or 5 runs) | Peak RSS KiB | Exact output SHA-256 |
|---|---:|---:|---|
| tiny-seed1 | 26.514 ms | 21,928 | `dcdad7cedfc07eb66ecced1861cc91d9017e004c56cbe4f38e9413b62b17ff70` |
| sample-seed1 | 132.864 ms | 36,096 | `eeeec1e9319da98a6f3c664c78f93e7078fd603c25c2bb3027174e81e3638a8f` |
| full-seed1 | 1729.093 ms | 342,632 | `f86973c210228141bcb6f1f1b10188ad2a9b65f71e7cd7cf4590b38de4e80927` |
| full-seed2 | 1388.796 ms | 339,644 | `7f224ef65795253d56911fa8f25553a8fd0a2cef4addc7bd411e40e8da990869` |
| full-seed3 | 813.261 ms | 341,648 | `7e004661d6d7b2db15a3171e687d52de1d789f3efe6e32ab4dfe7ad78515a71f` |

## Baseline versus final, interleaved on full seed 1

| Candidate | Middle mean | Observed range | Peak RSS KiB |
|---|---:|---:|---:|
| Correct C++ baseline (3 runs) | 14975.271 ms | 14215.988–15400.505 ms | 388,924 |
| Final exact artifact (5 runs) | 916.693 ms | 710.969–1023.088 ms | 342,732 |

Measured speedup in that comparison: **16.34x**. The earlier
baseline checkpoint measured 11,641.009 ms at full, 422.022 ms at sample and
20.651 ms at tiny; raw original results are preserved. Tiny is not optimized for
speed: sample planning rereads its small corpus and can cost more than baseline.

Final full peak RSS across the reported batches is **342,732 KiB
(334.70 MiB)**. This includes resident mapped input pages. The separate
full diagnostic sampled anonymous RSS at **90,424 KiB
(88.30 MiB)**. Anonymous sampling is approximate, and neither number
is the official memory axis. The harness prewarms data and measures cgroup
`memory.peak`, including kernel/output charges; local RSS must not be labeled M.

The leaderboard target of 74 ms / 47 MB **has not been achieved locally**.

## Architecture and measured experiments

1. The first correct C++ baseline used a query-term inverted index and mapped
   text for rarest-term phrase candidate verification. Full profile was roughly
   6.9 s indexing, 1.3 s Boolean evaluation and 3.3 s phrase evaluation, at the
   original 11.64 s checkpoint. Source is preserved as `baseline.cpp`.
2. Packed exact keys, manual query parsing, query-aware Boolean lists/bitmaps,
   streaming anchored phrase verification and parallel document partitions
   reduced an initial candidate to 3,377 ms at one worker and 775 ms at twelve.
   Initial sweep: four workers 1,094 ms, six 998 ms, eight 885 ms. These were
   earlier local batches, not isolated effect sizes or official measurements.
3. Fully streaming Boolean evaluation used anchors for disjunctive clauses and
   term-presence stamps, eliminating Boolean postings. It was correct and saved
   roughly 80 MiB total RSS, but measured 1,313 ms at twelve workers versus the
   earlier 775 ms index candidate. Rejected for added query work and poorer K.
4. A 32-byte block tokenizer was correct on generated/fuzz inputs, but measured
   about 1,236 ms at twelve workers in its batch. It did not establish a speed
   improvement over the simpler scanner; rejected. Later final tests added
   arbitrary control separators; only the final scanner is certified for them.
5. Compact hash slots, hot term metadata and Boolean-only posting descriptors
   measured 744 ms versus 849 ms for the preceding candidate in adjacent batches.
   Sampled anonymous RSS dropped from about 127 MiB to 121 MiB. Retained; the
   small timing effect is less certain than the memory effect.
6. Bounded partitions make 16-bit local postings exact at any tested worker count.
   Sparse payload is 22,280,796 bytes for 11,140,398 pairs on full seed 1, versus
   twice that with 32-bit postings. There are 360 frequent-term bitmaps, about
   11.25 MB total. Allocator capacity, descriptors and query metadata add overhead.
   The final separate diagnostic is about 88 MiB anonymous RSS.
7. Query string views, short-key storage, SIMD boundary detection and skipping
   separators reduce allocations and lookup metadata. The interleaved candidate
   comparison selected lean/no-LTO at 979.378 ms versus compact/32-bit at
   1,050.047 ms. The same trial measured lean/LTO at 1,157.911 ms, speculative
   reserve at 1,100.276 ms, and a 512 KiB sample at 1,496.135 ms. No-LTO with a
   1 MiB sample won; smaller samples and speculative reserve were rejected.
8. Bitmap thresholds 8/16/32/64, sample sizes 128/256/512/1,024 KiB, worker counts
   and LTO were explored. Hashes passed, but the exploratory sweep had large
   timing variation and partly included memory polling. It is diagnostic only;
   final selection uses the later interleaved comparisons. PGO was not adopted:
   an offline reproducible build/training workflow was not established here.
9. Output is explicitly closed and workers joined before `_Exit(0)`. The kernel
   reclaims index memory without walking every temporary allocation. This removes
   allocator teardown work; no standalone, stable percentage speedup is claimed.

Sampling only chooses representation and anchor order, never excludes matches.
Hash collisions always receive full equality checks, including suffix bytes.
The final SIMD tokenizer tests digits against original bytes to keep all
non-alphanumeric bytes, including 0x10–0x19, as separators. Phrase bounds are
per-document. Repeated term occurrences and query/document matches are deduped
with local stamps. Mutable indexes are owned by workers, then queried read-only.

## K estimates (illustrative only)

Frozen official calibration: T_ref=56.352376 s, M_ref=2,199,961,600 bytes,
S_ref=91, D_ref=1. The actual formula caps memory improvement at 50x and source
improvement at 1x. The harness dependency helper reports D=1, declared/unverified.

Local clang-format was applied; a simple nonblank/noncomment count estimates
616 C++ lines plus 6 Dockerfile lines:
**S≈622**, pending exact scc/static-tools verification.

Using the final interleaved local T, estimated S and **total local RSS as an M
proxy** gives **K≈8.483**. Substituting sampled anonymous RSS instead gives
**K≈11.074**, but that omits other cgroup charges. These are illustrative
mixed-hardware calculations, **not official K values, bounds or predictions**.
No official run IDs exist.

## Remaining work and bottlenecks

The main scan, exact term lookup, posting construction and streaming phrase
checks dominate (roughly 80–85% of profiled execution); Boolean query work is
next. Sparse vector capacity/descriptors still consume meaningful private memory.
Future experiments should target cache locality and allocations, verify effects
on a quiet P-core-only machine, and consider contiguous/compressed posting
storage without paying for another full corpus scan. Sampling/classification is
not the dominant remaining cost.

On a Docker-capable machine, run the concrete official check:

```bash
python3 check.py b --data benchmarks/track-b/data/full-seed1 --expected f86973c210228141bcb6f1f1b10188ad2a9b65f71e7cd7cf4590b38de4e80927
```

The Dockerfile's six FROM/COPY/RUN/ENTRYPOINT lines pass the frozen allowlist
check. The build invokes GCC only, copies only main.cpp, and has no network or
package installation step. Still required: actual offline image build, runtime
sandbox check, exact static axes, then an authorized official leaderboard run.
No Git history change or external submission was made.

See `../benchmarks/track-b/COMMANDS.md` for the shell command record, scripts for
every experiment, `final-summary.json`, `final-compare.json`, `paired-summary.json`,
`submission-audit.json`, and the per-run JSON files for raw evidence.
