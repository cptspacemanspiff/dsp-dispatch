# dsp-dispatch

A commercially distributable **CPU FFT/FIR abstraction** with one normalized
contract across backends. Application code never sees backend-specific layouts,
packing, or scaling rules — you write against a single API and pick the fastest
available backend at build time.

[Get started](getting-started.md){ .md-button .md-button--primary }
[View benchmarks](benchmarks.md){ .md-button }

## Why

Every vendor FFT (oneMKL, AOCL, IPP, KFR, CMSIS-DSP, liquid-dsp, vDSP) has its
own storage layout, packing scheme, and scaling convention. dsp-dispatch pins
down **one contract** — see [`docs/requirements.md`](requirements.md) — and
implements it on top of each backend, so switching backends is a build flag, not
a rewrite.

- **Normalized plan-based API** — `include/fft/fft.h` and `include/fir/fir.h`.
- **Virtual-free dispatch.** A plan holds an opaque state pointer and a function
  pointer into monomorphized backend code; `execute` is a single indirect call
  hoistable out of a caller's loop. No virtual calls, no dispatch inside the
  transform.
- **Consistent normalization.** Forward is unscaled; inverse and complex-to-real
  are scaled by `1/N`; real-to-complex output has `N/2 + 1` bins — regardless of
  backend.
- **Correctness tests** against a reference DFT (round-trip, impulse, batch,
  error handling), run against every backend in CI.

## Backends at a glance

| Domain | Backends |
|--------|----------|
| **FFT** | PocketFFT (portable, in every build), Intel oneMKL, AMD AOCL-FFTZ, CMSIS-DSP, KFR *(bench-only)* |
| **FIR** | portable (in-tree), liquid-dsp, Intel IPP, CMSIS-DSP, KFR *(bench-only)* |

See [Backends](backends.md) for the full support matrix and build flags.

!!! note "CI benchmark numbers are a smoke signal"
    The charts on the [Benchmarks](benchmarks.md) page come from shared GitHub
    runners of unknown CPU vendor and load. They confirm each backend
    **compiles and runs correctly** — they are *not* regression-grade
    performance measurements. Run on dedicated hardware for real numbers.
