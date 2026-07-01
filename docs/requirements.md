# DSP Dispatch Requirements (Phase 1)

This document fixes the workload and the public contract so backend selection
and acceptance testing have a stable target. Values marked **(initial)** are
defaults chosen to unblock implementation; revisit them against a real product
workload before final backend decisions (Phase 7).

## Workload

| Dimension | Decision |
|---|---|
| Transform types | Complex-to-complex, real-to-complex, complex-to-real |
| Precision | `float32` first; `float64` supported by the portable backend, required elsewhere only if a product needs it |
| Dimensions | 1D only **(initial)** |
| Lengths | Arbitrary `N >= 1`. Powers of two are the fast path; other lengths use Bluestein in the portable backend **(initial)** |
| Batch | 1..N independent transforms laid out contiguously |
| In-place | Permitted for complex transforms; real transforms are out-of-place **(initial)** |
| Threading | FFT execution may run on real-time threads; backends must not allocate or lock in `execute` |
| Targets | See "Acceptance thresholds" below |

## Public contract

These rules are backend-independent and enforced by adapters:

- **Complex layout:** interleaved (`re, im, re, im, ...`), `float` or `double`.
- **Real-to-complex output:** `N/2 + 1` complex bins (`real_complex_bins(N)`).
- **Complex-to-real input:** `N/2 + 1` complex bins; the imaginary parts of the
  DC and Nyquist bins are ignored.
- **Sign convention:** forward uses `exp(-2*pi*i*j*k/N)`; inverse uses `+`.
- **Normalization:** forward is unscaled; inverse and complex-to-real are scaled
  by `1/N`. This is the single normalization rule for every backend.
- **Direction:** `Complex` honors `direction`; `RealToComplex` is always forward
  and `ComplexToReal` is always inverse (their `direction` field is ignored, and
  an explicitly contradictory direction is rejected at plan creation).
- **Aliasing:** `input == output` is allowed for complex transforms.
- **Allocation:** all allocation and backend setup happen in `FftPlan::create`;
  `execute` performs no allocation, locking, or planning.
- **Thread-safety:** a single `FftPlan` is **not** safe for concurrent `execute`
  because it owns mutable scratch. Create one plan per thread for concurrency;
  plans are cheap to create from the same config.

## Benchmark size matrix

Standard 1D power-of-two lengths (Phase 6):

```
64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384
```

Add real production lengths and any required non-power-of-two lengths before
final selection.

## Acceptance thresholds

- **Correctness:** every backend passes the normalized test suite (reference DFT
  comparison, forward/inverse round-trip, impulse/constant/sinusoid/Nyquist/
  random, real and complex, every supported length, representative batches,
  error handling) after normalizing all backend conventions.
- **Tolerances:** absolute error below `1e-4 * N` for `float32` forward
  transforms versus the double-precision reference DFT; round-trip error below
  `1e-4`. Tighten for `float64`.
- **Backend retention (Phase 7):** a vendor backend is kept only if it improves
  the primary workload by at least **15%** over the portable fallback, in
  addition to passing correctness, meeting redistribution terms, supporting all
  required shapes, and meeting binary-size/init constraints.

## Open decisions

Tracked in `PLAN.md` ("Open decisions"). The ones blocking final selection:
exact production lengths/batches, whether non-power-of-two lengths are required,
whether `float64`/multidimensional/internal-threading are required, minimum OS
versions, and whether the Intel/AMD backends clear the 15% threshold.
