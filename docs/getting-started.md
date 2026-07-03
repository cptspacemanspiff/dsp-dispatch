# Getting started

## Build

```sh
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

By default this builds the `portable` FFT and FIR backends and runs the full
correctness suite. To select a different backend, set `FFT_BACKEND` /
`FIR_BACKEND` at configure time — see [Backends](backends.md).

## FFT usage

```cpp
#include "fft/fft.h"

fft::FftPlanConfig cfg;
cfg.type = fft::FftType::Complex;
cfg.precision = fft::FftPrecision::F32;
cfg.length = 1024;
cfg.direction = fft::FftDirection::Forward;

auto plan = fft::FftPlan::create(cfg);
if (!plan) { /* inspect plan.status() */ }

// Interleaved complex in/out; in == out is allowed for complex transforms.
plan.value().execute(input, output);
```

!!! info "Normalization contract"
    Forward is **unscaled**; inverse and complex-to-real are scaled by `1/N`.
    Real-to-complex output has `N/2 + 1` bins. A single `FftPlan` is **not** safe
    for concurrent `execute` — create one plan per thread.

## FIR usage

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
if (!plan) { /* inspect plan.status() */ }
plan.value().execute(input, output);
```

!!! info "Stateful, single-threaded"
    FIR plans are stateful: each `execute` continues from the previous delay
    line, so streaming just means calling `execute` block after block. A single
    `FirPlan` is **not** safe for concurrent `execute`. Call `clear()` to reset
    the delay line to zero.

## Continuous integration

[`.github/workflows/benchmarks.yml`](https://github.com/cptspacemanspiff/dsp-dispatch/blob/main/.github/workflows/benchmarks.yml)
runs on every push and pull request (and on manual dispatch):

- **Correctness** — builds the library with each implemented FFT and FIR backend
  on both x86 and Arm64 runners and runs the full normalized test suite against
  each.
- **Benchmarks** — builds every `*_bench_*` executable, runs
  `tools/run_benchmarks.py`, publishes tables to the run summary, and uploads
  `bench_results/` (tables, CSV, charts, JSON) as an artifact.
- **Publish** — collates the JSON into the interactive
  [Benchmarks dashboard](benchmarks.md) and deploys this site to GitHub Pages;
  pull requests get a sticky comment with the tables and charts.
