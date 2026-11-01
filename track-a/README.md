# Track A: Ingest

The [frozen specification](../../speed-demon-2026/specs/a-ingest.md) defines the input and exact output bytes.

## Approach

`main.cpp` maps `/data/requests.log` read-only and splits it into six ranges at newline boundaries (one worker for files under 8 MiB). The first pass counts requests and errors by `(hour, endpoint)`. Each worker has a private flat endpoint table; hardware CRC hashing is only a lookup accelerator, and collisions compare exact endpoint bytes. Endpoint IDs and strings are discovered from each input. Word scans locate endpoint delimiters without reading beyond the range. Methods use the seven fixed grammar lengths, and decimal latency parsing combines up to eight digits at a time with a bounded scalar fallback.

The main thread combines small count tables and assigns disjoint slices of one exact-size contiguous latency array. Workers fill those slices in a second pass, without locks or per-group allocations. The count tables are released before allocating the latency array, and cursors are released before rendering. This trades a second input scan for substantially less allocation and merging work.

Endpoint IDs are sorted once by byte order. Output workers process separate hours, sorting groups smaller than 64 and using successive `nth_element` partitions for larger groups. Each worker keeps only one hour buffer. A condition variable orders file writes; selection and formatting happen outside its lock. Completed whole latency pages are discarded with `madvise`, protecting pages that cross hour boundaries. All arithmetic is integer arithmetic; duplicates count separately and input order is irrelevant.

The 72-hour index uses the dates fixed by the spec. It does not depend on input order or a seed-specific endpoint catalogue. Duplicate lines remain separate requests. All arithmetic used for output is integer arithmetic.

The Docker build is offline and uses pinned allowlisted GCC 16 and Debian 13 images. There are no third-party runtime dependencies. The target is the specified x86-64 i5-13500; `-msse4.2` enables hardware CRC. LTO, native compilation and PGO were tested locally and rejected for lack of measured benefit. Running without arguments uses the sandbox paths; two positional paths are available for local testing.

## Local validation

Generate data outside this directory with `../speed-demon-2026/generators/a_ingest.py`. Compile locally with `g++ -O3 -std=c++20 -DNDEBUG -pthread -msse4.2 -static-libstdc++ -static-libgcc -o /tmp/sd-a-ingest track-a/main.cpp`, then run `/tmp/sd-a-ingest INPUT OUTPUT`. Seed-1 tiny/sample/full, seed-2 sample, seed-3 full, seeds 7 and 17, shuffled input, empty input, and an adversarial page-boundary input passed. See [PERF.md](PERF.md) for hashes, environment and measurements. Reproducible scripts, raw results, and the preserved official baseline are outside the scored folder under `benchmarks/`.

The prior implementation passed the board in run `67e7631`: 150 ms, 58 MB, 200 source lines, K 15.888. This candidate has no board result yet. Local Docker access was previously denied; native RSS is not the board's memory measurement. Judge the new version's K only after measuring its time, cgroup memory and formatted source size on the board.

## With another 24 hours

Measure this candidate and worker counts on the six target P-cores. Two-pass parsing now dominates; investigate eliminating repeated endpoint lookup without increasing live storage enough to lose on K. Test any change against multiple full seeds. The 20–30 ms / 15–20 MB goal remains unproven.
