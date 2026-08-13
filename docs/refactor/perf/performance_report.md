# MiniCyber Performance Report

## Scope

This report records one reproducible Release measurement on the local host. It
compares end-to-end SPSC one-way latency for four mechanisms, with one message
in flight. Every payload contains a 16-byte timestamp/sequence header followed
by the configured payload bytes.

- INTRA: same-process MiniCyber Node writer and reader.
- PIPE: two threads using POSIX pipe byte-stream transfer.
- SHM: two independently initialized child processes using explicit
  `ShmTransmitter` and `ShmReceiver`.
- mutex_cv_queue: two threads using a capacity-one mutex and
  condition-variable queue.

The acknowledgement paths enforce a single in-flight message. They are not
included in individual one-way latency, but they are included in the throughput
measurement window so that the final consumer completion is observed.

## Environment And Reproduction

The final raw files are
[`raw/latency_20260813T152452Z.csv`](raw/latency_20260813T152452Z.csv) and
[`raw/latency_20260813T152452Z.json`](raw/latency_20260813T152452Z.json).
They record Ubuntu Linux 6.8.0-124-generic, x86_64, AMD Ryzen 7 5800H (4 online
logical CPUs), and GCC 11.4.0. The build is `Release`; CPU affinity and machine
load were not controlled.

The raw metadata identifies parent commit
`c9a724a637360847036e5186cbd1457fee61772e` and has `source_dirty: true`.
At collection time, the only project changes relative to that parent were the
MC-407 SHM cleanup call in `benchmark_pingpong.cpp`, this report work, and the
collector's `source_dirty` metadata field. The SHM cleanup was verified by
comparing `/dev/shm/minicyber_*` before and after a benchmark run: no new entry
remained.

```bash
cmake --preset release \
  -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/tmp/minicyber-googletest2
cmake --build build/release -j2 --target benchmark_pingpong
WARMUP=2000 SAMPLES=10000 \
  scripts/collect_benchmark.sh build/release/benchmark_pingpong \
  docs/refactor/perf/raw
```

## Results

Each cell is a nanosecond latency percentile. Throughput is completed messages
per second over the measurement window. These are measurements, not claims of
general ordering across mechanisms or hosts.

| Payload | Backend | p50 ns | p95 ns | p99 ns | Throughput msg/s |
|---:|---|---:|---:|---:|---:|
| 64 B | INTRA | 140 | 151 | 161 | 3,463,710 |
| 64 B | PIPE | 6,181 | 61,816 | 81,541 | 43,942 |
| 64 B | SHM | 515,325 | 1,043,755 | 2,787,259 | 1,373 |
| 64 B | mutex_cv_queue | 7,845 | 63,098 | 117,711 | 23,939 |
| 1 KiB | INTRA | 160 | 200 | 221 | 2,979,170 |
| 1 KiB | PIPE | 6,242 | 11,021 | 83,095 | 49,505 |
| 1 KiB | SHM | 537,976 | 1,365,544 | 4,508,931 | 1,299 |
| 1 KiB | mutex_cv_queue | 15,148 | 79,347 | 140,923 | 13,891 |
| 64 KiB | INTRA | 180 | 191 | 210 | 3,374,030 |
| 64 KiB | PIPE | 39,153 | 129,142 | 263,563 | 16,394 |
| 64 KiB | SHM | 528,269 | 1,548,458 | 3,896,494 | 1,256 |
| 64 KiB | mutex_cv_queue | 12,673 | 79,148 | 152,364 | 13,667 |

## Interpretation And Limits

INTRA is a same-process shared-message callback path, so its figures describe
that local path rather than a copied byte-transfer path. PIPE crosses the
kernel byte-stream interface. The queue is a copied in-process synchronization
baseline. SHM has separate processes, shared-segment block management, polling
notification, and a receive-side copy into the local dispatcher; its current
latency distribution includes the notifier's polling behavior. These different
mechanisms serve different deployment boundaries, so the table must not be used
as a standalone ranking.

The experiment does not measure CPU time, context switches, peak RSS, contention
load, MPSC/MPMC behavior, scheduler-policy variants, or long-run resource growth.
The run is a single sample set per cell, so it does not provide confidence
intervals. A future comparison should add repeated runs under controlled CPU
affinity and record those system counters.

The C++20 standard-coroutine experiment is intentionally excluded from the
table. Its compile/resume probe and the reason it lacks a like-for-like executor
or transport path are recorded in
[`cpp20_coroutine_experiment.md`](cpp20_coroutine_experiment.md).
