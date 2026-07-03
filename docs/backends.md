# Backends

Every backend implements the same normalized contract. You select one at
configure time; the library links only that backend, and application code is
unchanged.

## FFT backends

| Backend | Flag | Status | Notes |
|---------|------|--------|-------|
| **portable** | `-DFFT_BACKEND=portable` | ✅ implemented | In-tree. Radix-2 for powers of two, Bluestein otherwise. Complex, R2C, C2R in f32/f64, batched. No dependencies. |
| **Intel oneMKL** | `-DFFT_BACKEND=mkl` | ✅ implemented | DFTI descriptors; R2C uses CCE storage (`N/2+1` bins). Fetched on demand via `cmake/FetchMKL.cmake` (Intel PyPI wheels, no system install). |
| **AMD AOCL-FFTZ** | `-DFFT_BACKEND=aocl` | ✅ implemented | AMD's "Zen" FFT, built from source by `cmake/FetchAOCL.cmake`. Complex any length; real excludes 7-non-smooth lengths (reports `UnsupportedLength`). |
| **CMSIS-DSP** | `-DFFT_BACKEND=cmsis` | ✅ implemented | Arm's DSP library. Portable host build auto-vectorizes to NEON at `-O3`; hand-written NEON kernels are opt-in via `-DCMSISDSP_USE_NEON=ON -DCMSISCORE=<path>`. |
| **KFR** | *bench-only* | ⚙️ benchmark | Comparison baseline only — see the packaging note below. |
| **Apple vDSP** | `-DFFT_BACKEND=vdsp` | 🚧 scaffolded | Compile-error stub until implemented. |

`FFT_BACKEND=auto` (the default) resolves to `vdsp` on Apple, `cmsis` on Arm,
`aocl` on x86, else `portable`.

## FIR backends

| Backend | Flag | Status | Notes |
|---------|------|--------|-------|
| **portable** | `-DFIR_BACKEND=portable` | ✅ implemented | In-tree direct FIR. |
| **liquid-dsp** | `-DFIR_BACKEND=liquid` | ✅ implemented | Fetched by `cmake/FetchLiquidDSP.cmake`. |
| **Intel IPP** | `-DFIR_BACKEND=ipp` | ✅ implemented | Resolved by `cmake/FetchIPP.cmake`; can install `ipp-devel` + `ipp-static` wheels with `uv`. |
| **CMSIS-DSP** | `-DFIR_BACKEND=cmsis` | ✅ implemented | Arm DSP library (see FFT note on NEON). |
| **KFR** | *bench-only* | ⚙️ benchmark | Comparison baseline only. |

Unsupported precision/configurations are reported at plan creation via
`plan.status()` rather than failing silently.

## Build options

| Option | Default | Meaning |
|--------|---------|---------|
| `FFT_BACKEND` | `auto` | `auto·vdsp·cmsis·mkl·aocl·portable`. `auto` resolves per platform (see above). |
| `FIR_BACKEND` | `portable` | `portable·liquid·ipp·cmsis`. |
| `FFT_BUILD_TESTS` | `ON` | Build the correctness tests. |
| `FFT_ENABLE_BENCHMARKS` | `OFF` | Build per-backend benchmark executables on Google Benchmark. Always builds `*_bench_portable`. |
| `FFT_ENABLE_KFR_BENCHMARK` | `OFF` | Also build `fft_bench_kfr`. Requires benchmarks; forbidden when `FFT_PACKAGING=ON`. |
| `FFT_ENABLE_MKL_BENCHMARK` | `OFF` | Also build `fft_bench_mkl`. |
| `FFT_ENABLE_AOCL_BENCHMARK` | `OFF` | Also build `fft_bench_aocl`. |
| `FFT_ENABLE_CMSIS_BENCHMARK` | `OFF` | Also build `fft_bench_cmsis`. |
| `FIR_ENABLE_LIQUID_BENCHMARK` | `OFF` | Also build `fir_bench_liquid`. |
| `FIR_ENABLE_KFR_BENCHMARK` | `OFF` | Also build `fir_bench_kfr`. Forbidden when `FFT_PACKAGING=ON`. |
| `FIR_ENABLE_IPP_BENCHMARK` | `OFF` | Also build `fir_bench_ipp`. |
| `FIR_ENABLE_CMSIS_BENCHMARK` | `OFF` | Also build `fir_bench_cmsis`. |
| `FFT_ENABLE_ALL_BENCHMARKS` | `OFF` | Enable the harness plus every backend compatible with this host. What `make benchmarks` uses. |
| `FFT_PACKAGING` | `OFF` | Set in release CI to assert no forbidden (benchmark-only, e.g. KFR) libraries are present. |

!!! warning "KFR is benchmark-only and must never be packaged"
    KFR code lives only under `bench/`; the library never links it and the bench
    target has no install rule, so it can't enter a redistributable artifact.
    Setting `-DFFT_PACKAGING=ON` with KFR enabled is a hard configure error.
