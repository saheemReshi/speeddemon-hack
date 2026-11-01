# Track A: Ingest

The [frozen specification](../../speed-demon-2026/specs/a-ingest.md) defines the input and exact output bytes.

## Approach

`main.cpp` maps `/data/requests.log` read-only and splits it into six ranges at newline boundaries (one worker for files under 8 MiB). Each worker parses fixed-width timestamps directly, skips methods, interns endpoint byte strings as IDs, and collects latency vectors by `(hour, endpoint)`. Worker state is private, so parsing needs no locks. A 72-entry table per endpoint gives a direct group index. String views refer to the mapping, which stays alive until all output has been written. The main thread merges matching groups, moving vectors where possible.

Endpoint IDs are sorted once by exact byte order. Six workers then process separate hours, using full sorting for groups smaller than 64 and successive `nth_element` partitions for the three exact nearest-rank percentiles in larger groups. They format independent hour buffers with integer ppm arithmetic. Finally the main thread writes those buffers in hour order to `/out/summary.txt`. A group belongs to one output worker; there are no concurrent writes to the file.

The 72-hour index uses the dates fixed by the spec. It does not depend on input order or a seed-specific endpoint catalogue. Duplicate lines remain separate requests. All arithmetic used for output is integer arithmetic.

The Docker build is offline and uses pinned allowlisted GCC 16 and Debian 13 images. The executable has no third-party runtime dependencies. Running it without arguments uses the sandbox paths; two positional paths are available for local testing.

## Local validation

Generate data outside this directory with `../speed-demon-2026/generators/a_ingest.py`. Compile locally with `g++ -O3 -std=c++20 -DNDEBUG -pthread -static-libstdc++ -static-libgcc -o /tmp/sd-a-ingest track-a/main.cpp`, then run `/tmp/sd-a-ingest INPUT OUTPUT`. Seed-1 tiny, sample, and full outputs matched the published SHA256 values; a seed-2 sample matched the official Python reference. See [PERF.md](PERF.md) for hashes and timing.

The official Docker `check.py` could not complete on this workstation: access to `/var/run/docker.sock` was denied, including through the escalated command, and noninteractive sudo required a password. The checker did confirm both `FROM` digests are allowlisted before Docker failed. Run the official checker on a Docker-enabled host before board submission.

## With another 24 hours

Measure a full run on the scoring box, then profile endpoint lookup and latency-vector growth, which dominate parsing time locally. Test any parser or allocation change against the full hash and a second seed before retaining it.
