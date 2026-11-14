# Local runtime and file-tool measurements

## Local measurement (2026-11-03)

Apple M2, macOS 26.5.2, Apple Clang 21, Release `-O3`, system libcurl 8.7.1,
SQLite 3.51.0, yyjson 0.12.0. `bench_runtime` performs 100 sequential synthetic
text-only turns, with growing history, no observer, no network and no tool process.
The measured interval includes input submission and execution/receipt persistence.

| Store | P50 | P95 | Maximum |
| --- | ---: | ---: | ---: |
| In-memory SQLite | 0.126 ms | 0.150 ms | 0.254 ms |
| SQLite file, DELETE/FULL | 1.498 ms | 3.144 ms | 3.467 ms |

This text-only baseline was recorded at commit `5a49093`. It is one local sample,
not a model-latency benchmark or a cross-harness ranking.
Run your own sample with `-DAH_BENCHMARK=ON` and `./build/bench_runtime`.

The seeded stream fuzzer completed 50,000 runs with upstream LLVM/libFuzzer + UBSan.
The full suite and a separate mutation driver also ran with Apple ASan/UBSan.
On this macOS version the installed Homebrew ASan runtime stalled during its own
initialization, so it was not used for the coverage-guided run. Apple Clang's
ASan test builds worked. Use compatible compiler/runtime combinations; a sanitizer
startup failure is not a passed test or an application finding.

A separately authorized live subscription run covered text/tool execution on
`gpt-6.1-sol`: both this harness and Codex produced passing outputs on three fixed
synthetic tasks. Initial harness requests exposed an absent Content-Type header,
now handled by strict SSE validation. Codex had one timeout with correct output;
a labeled rerun completed. Generated reports and outputs were removed afterward.
Live Ask, cancellation and other models remain separate validation work.


## File tool measurement (2026-11-03)

`bench_files` invokes the production callbacks directly. This comparison uses the
same 4 KiB/64 KiB fixtures, 512 KiB capture budget, 20 warm-ups and 200 measured
runs with Release `-O3`. Reads return the complete requested fixture, with a warm
OS cache; writes include atomic rename plus file and directory `fsync`. Times
include tool argument validation and JSON construction, but exclude engine/SQLite,
network and model inference. The updated reader uses 1000-line pages here so its
returned byte counts match the old whole-file reader.

| Callback | Prior P50/P95 | Updated P50/P95 | Content bytes |
| --- | ---: | ---: | ---: |
| Read 4 KiB | 30 / 47 µs | 24 / 25 µs | 4096 |
| Read 64 KiB | 104 / 150 µs | 96 / 125 µs | 65536 |
| Atomic write 4 KiB | 177 / 203 µs | 176 / 198 µs | 4096 written |

The prior source is commit `5a49093`. This is one local sample; cache, scheduler,
filesystem and durability semantics affect the result. It does not establish a
universal speedup. Run `./build/bench_files [runs]` after configuring
`-DAH_BENCHMARK=ON`. The benchmark removes its temporary workspace.


## Read batch measurement (2026-11-03)

The November 3 build of `bench_parallel --file-store` compared concurrency
1 and 4 on the same runtime. The provider is a mock; every run starts a fresh session and completes
one model/tool/model cycle with four read calls. The timed interval includes
submit, scheduling, SQLite and ordered receipts, plus correctness checks in the
mock provider. Result projection is disabled equally in both modes. There are two
warm-ups, 100 measured file workloads and 40 measured synthetic latency workloads.

| Store / workload | Serial P50/P95 | Four workers P50/P95 |
| --- | ---: | ---: |
| In-memory / four reads of one 4 KiB file | 0.731 / 1.340 ms | 0.518 / 0.548 ms |
| DELETE/FULL / same file reads | 8.187 / 11.613 ms | 4.179 / 5.080 ms |
| In-memory / four synthetic 20 ms reads | 107.649 / 118.667 ms | 29.135 / 31.750 ms |
| DELETE/FULL / same synthetic reads | 127.149 / 138.579 ms | 36.782 / 39.201 ms |

The synthetic workload requests a 20 ms sleep per callback; OS scheduling can
overshoot. It is not a remote-filesystem or network measurement. Batching also
reduces durability transactions, so these numbers do not isolate thread speedup.
The benchmark checks exact returned file contents, all four receipts and their
order; it prints raw sample arrays and removes its temporary files. It establishes
neither model-latency improvements nor performance against other harnesses.


The subsequent Terra 6.1 xhigh comparison includes three parallel paired trials,
reported token/cache breakdowns, and a separate result-quality audit. See
[the full evaluation](endurance-evaluation.md).

The Sol 6.1 xhigh follow-up exposed a streamed-item assembly gap, now covered by
`stream_items` and the real C loopback tool-cycle test. A captured Sol stream
replayed correctly; the updated parser completed 20,000 additional libFuzzer/UBSan
iterations. Both macOS and Linux CI passed the 19-suite build and standalone install.
See [the comparison](sol-xhigh-evaluation.md) for all attempts and graded outcomes.

## Retained-context probe after layout cleanup (2026-11-04)

Release build on the same Apple M2 host. Three independent process repetitions,
100 sequential turns per store/size, mock text-only provider, no network or file
process tools. A single repeated-character user item is submitted before timing;
subsequent turns retain it while adding small messages. These byte sizes are not
token estimates and do not represent every real conversation shape.

| Seed bytes | Store | P50 range | P95 range | Maximum across repetitions |
| --- | --- | ---: | ---: | ---: |
| 0 | memory | 0.106–0.118 ms | 0.130–0.274 ms | 0.329 ms |
| 0 | SQLite/FULL | 1.354–1.728 ms | 1.668–4.418 ms | 23.486 ms |
| 65,536 | memory | 0.151–0.154 ms | 0.177–0.179 ms | 0.189 ms |
| 65,536 | SQLite/FULL | 1.399–1.473 ms | 1.718–1.856 ms | 2.296 ms |
| 1,048,576 | memory | 0.860–0.861 ms | 0.889–0.910 ms | 2.156 ms |
| 1,048,576 | SQLite/FULL | 2.313–2.351 ms | 2.635–2.769 ms | 5.137 ms |

The pre-refactor short-history SQLite/FULL P50 range was 1.345–1.549 ms across
three runs. The post-refactor first run had a visible tail outlier. These samples
establish neither a speedup nor a causal regression from the layout change.
Reproduce with `-DAH_BENCHMARK=ON` and `./build/bench_runtime [seed_bytes]` (0–8 MiB).
Initial seeding is excluded from the timed interval; context assembly and ordinary
submit/run durability remain included. All temporary stores are removed by the tool.

## Fresh-context subagents (2026-11-14)

Measured source is `b75c3b2`, version 0.5, on Apple M2 with macOS 26.5.2 and
Apple Clang 21 in Release mode. `bench_agents` gives each synthetic model request
20 ms of latency and uses an in-memory SQLite database. The timed interval covers
submit through tree completion. Each run must return all four child results
exactly once and record seven model responses and five coordination receipts.

Three separate process invocations each measured concurrency 1 followed by 4,
with one warmup and eight timed runs per setting. The computer was shared.

| Invocation | Active requests | P50 ms | P95 ms |
| --- | ---: | ---: | ---: |
| 1 | 1 | 180.864 | 202.662 |
| 1 | 4 | 104.400 | 119.168 |
| 2 | 1 | 191.777 | 199.699 |
| 2 | 4 | 114.782 | 123.824 |
| 3 | 1 | 192.444 | 203.306 |
| 3 | 4 | 114.271 | 125.386 |

The median block P50 was 191.777 ms at concurrency 1 and 114.271 ms at concurrency
4, a 40.4% reduction for this fixture. Each block reports its lower central order
statistic as P50 and its maximum of eight samples as P95. The observed peak request
count matched the configured limit in every run. The root's model requests share
that limit with its children.

The independent HTTP fixture requires eight simultaneous requests to reach a
server barrier. Its public CLI case also verifies fresh child inputs, shared host
instructions, independent routing, join receipts and follow-up tasks against the
native concurrent transport. These tests exercise loopback HTTP and synthetic
responses. Subscription-model task latency and quality require separate measurement.

Build with `-AH_BENCHMARK=ON` and run `./build/bench_agents` to reproduce. The
measured binary SHA-256 was
`2d7ee4cb9e7c783187e176963dc4b1a0db83e02c72f490b4b2afb8d375c39bb7`.
