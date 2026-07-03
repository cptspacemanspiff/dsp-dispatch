# dsp-dispatch

A commercially distributable CPU FFT abstraction with one normalized contract
across backends. Application code never sees backend-specific layouts, packing,
or scaling rules. See [`PLAN.md`](PLAN.md) for the full roadmap and
[`docs/requirements.md`](docs/requirements.md) for the fixed contract.

📖 **Documentation & live benchmark dashboard:**
<https://cptspacemanspiff.github.io/dsp-dispatch/>
(built from `docs/` with MkDocs Material; see [`mkdocs.yml`](mkdocs.yml). Preview
locally with `pip install mkdocs-material && mkdocs serve`.)

## Status

Implemented and tested here:

- **Normalized plan-based API** (`include/fft/fft.h`) — Phase 2.
- **Normalized FIR plan API** (`include/fir/fir.h`) with stateful block
  execution, batch support, and portable/liquid-dsp/IPP backend selection.
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
- **FIR backends**: an in-tree portable direct FIR backend, optional
  **liquid-dsp** (`-DFIR_BACKEND=liquid`), and optional **Intel IPP**
  (`-DFIR_BACKEND=ipp`). liquid-dsp is fetched by `cmake/FetchLiquidDSP.cmake`;
  IPP is resolved by `cmake/FetchIPP.cmake`, which can install Intel's
  `ipp-devel` + `ipp-static` wheels with `uv`. Unsupported
  precision/configurations are reported at plan creation.

Scaffolded (compile-error stubs until implemented): the `vdsp` and `cmsis`
backends. Cross-hardware benchmarking and legal review (Phases 6–7) require
physical devices, vendor SDKs, and counsel.

## Build

```sh
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## CI

`.github/workflows/benchmarks.yml` runs on push/PR (and manual dispatch):

- **backend-tests** — builds the production library with each backend
  (`portable`, `mkl`, `aocl`) and runs the full test suite against it.
- **benchmarks** — builds every `fft_bench_*`, runs `tools/run_benchmarks.py`,
  posts the results table to the run summary, and uploads `bench_results/`
  (tables, CSV, graph, JSON) as an artifact.

CI benchmark numbers are a **smoke signal** only: shared GitHub runners are
noisy and of unknown CPU vendor, so they confirm the backends compile and run,
not their relative performance. Run on dedicated hardware for real numbers.

### Options

| Option | Default | Meaning |
|---|---|---|
| `FFT_BACKEND` | `auto` | `auto\|vdsp\|cmsis\|mkl\|aocl\|portable`. `auto` resolves to `vdsp` on Apple, `cmsis` on Arm, `aocl` on x86, else `portable`. `portable`, `mkl`, and `aocl` are implemented (`mkl` fetches oneMKL via `cmake/FetchMKL.cmake`; `aocl` builds AOCL-FFTZ via `cmake/FetchAOCL.cmake`). |
| `FIR_BACKEND` | `portable` | `portable\|liquid\|ipp`. `portable` is the in-tree direct FIR backend. `liquid` fetches and links liquid-dsp. `ipp` uses Intel IPP from an installed package or from `ipp-devel` + `ipp-static` wheels installed with `uv`. |
| `FFT_BUILD_TESTS` | `ON` | Build the correctness tests. |
| `FFT_ENABLE_BENCHMARKS` | `OFF` | Build per-backend benchmark executables on Google Benchmark (fetched if not installed). Always builds `fft_bench_portable`. |
| `FFT_ENABLE_KFR_BENCHMARK` | `OFF` | Also build `fft_bench_kfr` (KFR fetched via FetchContent). Requires benchmarks; forbidden when `FFT_PACKAGING=ON`. |
| `FFT_ENABLE_MKL_BENCHMARK` | `OFF` | Also build `fft_bench_mkl` (Intel oneMKL fetched via `cmake/FetchMKL.cmake`). Requires benchmarks. |
| `FFT_ENABLE_AOCL_BENCHMARK` | `OFF` | Also build `fft_bench_aocl` (AMD AOCL-FFTZ fetched + compiled via `cmake/FetchAOCL.cmake`). Requires benchmarks. |
| `FFT_ENABLE_FFTW_BENCHMARK` | `OFF` | Also build `fft_bench_fftw` (FFTW fetched + compiled via `cmake/FetchFFTW.cmake`; x86 + Arm). Requires benchmarks; forbidden when `FFT_PACKAGING=ON` (GPL). |
| `FIR_ENABLE_LIQUID_BENCHMARK` | `OFF` | Also build `fir_bench_liquid` (liquid-dsp fetched via `cmake/FetchLiquidDSP.cmake`). Requires benchmarks. |
| `FIR_ENABLE_KFR_BENCHMARK` | `OFF` | Also build `fir_bench_kfr` (KFR fetched via FetchContent). Requires benchmarks; forbidden when `FFT_PACKAGING=ON`. |
| `FIR_ENABLE_IPP_BENCHMARK` | `OFF` | Also build `fir_bench_ipp` (Intel IPP resolved by `cmake/FetchIPP.cmake`; can install `ipp-static` with `uv`). Requires benchmarks. |
| `FFT_ENABLE_ALL_BENCHMARKS` | `OFF` | Enable the benchmark harness plus every backend compatible with this host (FFT: `kfr`, `cmsis`, `fftw` plus x86-only `mkl`/`aocl`; FIR: `kfr`, `cmsis`, `liquid` plus x86-only `ipp`). Equivalent to setting the individual flags for compatible backends. This is what `make benchmarks` uses. |
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
# -> bench_results/{fft_<backend>.json, fft_results.md, fft_results.csv, fft_latency.png}
tools/run_benchmarks.py --build-dir build-bench --suite fir
# -> bench_results/{fir_<backend>.json, fir_results.md, fir_results.csv,
#                   fir_latency.png}
```

It prints execution-latency and throughput tables (with speedup vs a baseline,
`--baseline portable` by default). FFT graphs include latency and throughput;
FIR graphs use one latency subplot per tap count, with block size on the x-axis.
Useful flags: `--filter execute`, `--min-time 0.2s`, `--repetitions 30`,
`--backends portable kfr ipp`, `--suite fir`, and `--no-run` to re-collate
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

FIR usage:

```cpp
#include "fir/fir.h"

float taps[] = {0.25f, 0.5f, 0.25f};
float input[128] = {};
float output[128] = {};

fir::FirPlanConfig cfg;
cfg.type = fir::FirType::Real;
cfg.precision = fir::FirPrecision::F32;
cfg.taps = taps;
cfg.tap_count = 3;
cfg.block_size = 128;

auto plan = fir::FirPlan::create(cfg);
if (!plan) { /* plan.status() */ }
plan.value().execute(input, output);
```

FIR plans are single-threaded and stateful: each `execute` continues from the
previous delay line, and a single `FirPlan` is not safe for concurrent
`execute` calls. Call `clear()` to reset the delay line to zero.
