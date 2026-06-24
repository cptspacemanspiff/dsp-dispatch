# fft-dispatch

A commercially distributable CPU FFT abstraction with one normalized contract
across backends. Application code never sees backend-specific layouts, packing,
or scaling rules. See [`PLAN.md`](PLAN.md) for the full roadmap and
[`docs/requirements.md`](docs/requirements.md) for the fixed contract.

## Status

Implemented and tested here:

- **Normalized plan-based API** (`include/fft/fft.h`) — Phase 2.
- **Virtual-free dispatch.** `FftPlan` holds an opaque state pointer and a
  function pointer into monomorphized backend code; `execute` is a single
  indirect call hoistable out of a caller's loop. No virtual calls, and no
  dispatch inside the transform itself (Phase 3 note).
- **Portable backend** (`src/backends/portable/`) — radix-2 for powers of two,
  Bluestein for all other lengths; complex, real-to-complex, and complex-to-real
  in `float32` and `float64`, with batching.
- **Correctness tests** (`tests/`) against a reference DFT, round-trip, impulse,
  batch, and error handling — Phase 5.
- **Benchmark harness** (`bench/`) on Google Benchmark: plan creation timed
  separately from execution, median + p95/p99, transforms/sec — Phase 6.
- **Build & licensing controls** (Phase 4): backend selection, the KFR
  benchmark gate, and `third_party/manifest.yml`.

- **Intel oneMKL backend** (`src/backends/mkl/`): DFTI descriptors for complex,
  real-to-complex (CCE storage → `N/2+1` bins), and complex-to-real, f32/f64,
  batched. oneMKL is fetched on demand by `cmake/FetchMKL.cmake` (downloads and
  extracts Intel's PyPI wheels — no system install). Passes the full suite.
- **AMD AOCL-FFTZ backend** (`src/backends/aocl/`): AMD's "Zen" FFT library (the
  AMD counterpart to oneMKL), built from source into a static library by
  `cmake/FetchAOCL.cmake`. Complex + real, f32/f64, batched. Complex transforms
  of any length; real transforms exclude 7-non-smooth lengths (the backend
  reports `UnsupportedLength`). Passes the full suite for supported shapes.

Scaffolded (compile-error stubs until implemented): the `vdsp` and `cmsis`
backends. Cross-hardware benchmarking and legal review (Phases 6–7) require
physical devices, vendor SDKs, and counsel.

## Build

```sh
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

### Options

| Option | Default | Meaning |
|---|---|---|
| `FFT_BACKEND` | `auto` | `auto\|vdsp\|cmsis\|mkl\|aocl\|portable`. `auto` resolves to `vdsp` on Apple, `cmsis` on Arm, else `portable`. `portable`, `mkl`, and `aocl` are implemented (`mkl` fetches oneMKL via `cmake/FetchMKL.cmake`; `aocl` builds AOCL-FFTZ via `cmake/FetchAOCL.cmake`). |
| `FFT_BUILD_TESTS` | `ON` | Build the correctness tests. |
| `FFT_ENABLE_BENCHMARKS` | `OFF` | Build per-backend benchmark executables on Google Benchmark (fetched if not installed). Always builds `fft_bench_portable`. |
| `FFT_ENABLE_KFR_BENCHMARK` | `OFF` | Also build `fft_bench_kfr` (KFR fetched via FetchContent). Requires benchmarks; forbidden when `FFT_PACKAGING=ON`. |
| `FFT_ENABLE_MKL_BENCHMARK` | `OFF` | Also build `fft_bench_mkl` (Intel oneMKL fetched via `cmake/FetchMKL.cmake`). Requires benchmarks. |
| `FFT_ENABLE_AOCL_BENCHMARK` | `OFF` | Also build `fft_bench_aocl` (AMD AOCL-FFTZ fetched + compiled via `cmake/FetchAOCL.cmake`). Requires benchmarks. |
| `FFT_PACKAGING` | `OFF` | Set in release/packaging CI to assert no forbidden (benchmark-only) libraries are present. |

## Benchmarks

Each backend is benchmarked by its **own executable** (`fft_bench_<backend>`),
built from one backend-agnostic harness so the comparison is apples-to-apples
(same buffers, same normalization, same statistics). Each binary self-identifies
via the `label` column.

```sh
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DFFT_ENABLE_BENCHMARKS=ON \
      -DFFT_ENABLE_KFR_BENCHMARK=ON -DFFT_ENABLE_MKL_BENCHMARK=ON \
      -DFFT_ENABLE_AOCL_BENCHMARK=ON -S . -B build-bench
cmake --build build-bench --target \
      fft_bench_portable fft_bench_kfr fft_bench_mkl fft_bench_aocl

./build-bench/bench/fft_bench_portable        # in-tree portable backend
./build-bench/bench/fft_bench_kfr             # KFR    (fetched via FetchContent)
./build-bench/bench/fft_bench_mkl             # oneMKL (fetched via cmake/FetchMKL.cmake)
./build-bench/bench/fft_bench_aocl            # AOCL-FFTZ (built via cmake/FetchAOCL.cmake)
```

The harness times plan creation separately from execution and reports median,
p95, and p99 (over 15 repetitions) plus a transforms/sec rate.

### Runner script (recommended)

`tools/run_benchmarks.py` runs every `fft_bench_*` executable, collates their
JSON, and writes tables, a CSV, and a comparison graph:

```sh
tools/run_benchmarks.py --build-dir build-bench
# -> bench_results/{<backend>.json, results.md, results.csv, latency.png}
```

It prints execution-latency and throughput tables (with speedup vs a baseline,
`--baseline portable` by default) and a `latency.png` with log-log latency and
throughput vs N. Useful flags: `--filter execute`, `--min-time 0.2s`,
`--repetitions 30`, `--backends portable kfr`, and `--no-run` to re-collate
existing JSON without re-running.

### Raw export

Or use Google Benchmark's built-in flags directly — JSON or CSV, stdout or file:

```sh
./build-bench/bench/fft_bench_kfr \
    --benchmark_out=kfr.json --benchmark_out_format=json
./build-bench/bench/fft_bench_portable --benchmark_format=csv > portable.csv
./build-bench/bench/fft_bench_portable --benchmark_filter='1024'
```

### Fair comparison (ISA)

KFR defaults to a conservative `sse2` baseline. Raise it (and match the portable
side) for a meaningful comparison:

```sh
cmake -DKFR_ARCH=avx2 build-bench          # avx2 | avx512 | ...
cmake --build build-bench --target fft_bench_kfr
```

KFR may be benchmarked in a Release build to validate performance fairly; it
must never be **packaged**. The KFR code lives only under `bench/`, the library
never links it, and the bench target has no install rule, so it can never enter
a redistributable artifact. Setting `-DFFT_PACKAGING=ON` with KFR enabled is a
hard configure error.

## Usage

```cpp
#include "fft/fft.h"

fft::FftPlanConfig cfg;
cfg.type = fft::FftType::Complex;
cfg.precision = fft::FftPrecision::F32;
cfg.length = 1024;
cfg.direction = fft::FftDirection::Forward;

auto plan = fft::FftPlan::create(cfg);
if (!plan) { /* plan.status() */ }

// interleaved complex in/out; in == out allowed for complex transforms.
plan.value().execute(input, output);
```

Normalization: forward is unscaled, inverse and complex-to-real are scaled by
`1/N`. Real-to-complex output has `N/2 + 1` bins. A single `FftPlan` is not safe
for concurrent `execute`; create one plan per thread.
