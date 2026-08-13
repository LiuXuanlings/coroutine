# C++20 Coroutine Optional Experiment

## Result

`benchmark_cpp20_coroutine` is a standalone C++20 compile and resume probe. It
uses only the standard `<coroutine>` header and verifies a coroutine can suspend
and resume with the project compiler.

## Exclusion From Latency Comparison

The probe is deliberately excluded from `collect_benchmark.sh` and from the
shared latency CSV schema. Standard C++20 coroutines define frame and suspend
semantics, but provide neither an executor nor a cross-thread scheduling,
notification, back-pressure, or cross-process transport mechanism. Adding any
of those requires a separate runtime, whose queue and wake-up policy would be
the object measured rather than the language feature.

The established baselines are end-to-end SPSC transport paths with an actual
consumer and one in-flight message. A same-thread coroutine resume loop would
not preserve that semantics; a custom threaded executor would duplicate the
mutex/condition-variable baseline under a new abstraction. It therefore cannot
be a stable, like-for-like result for this project and is recorded as excluded.

## Reproduction

```bash
cmake -S . -B build-cleanup \
  -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/tmp/minicyber-googletest2
cmake --build build-cleanup -j2 --target benchmark_cpp20_coroutine
build-cleanup/benchmark_cpp20_coroutine
```
