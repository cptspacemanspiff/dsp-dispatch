# FFT Dispatch Plan

## Goal

Build a commercially distributable CPU FFT abstraction that provides predictable
behavior across Apple Silicon, generic Arm, Intel x86-64, and AMD x86-64 without
exposing backend-specific layouts or scaling rules to application code.

Initial production backends:

| Platform | Production backend | License |
|---|---|---|
| Apple platforms | Accelerate/vDSP | Apple SDK terms |
| Non-Apple Arm/AArch64 | CMSIS-DSP | Apache-2.0 |
| Intel x86-64 | oneMKL DFT | Intel redistributable terms |
| AMD x86-64 | AOCL-FFTW | Verify the exact packaged version and redistribution terms |
| Other/unsupported CPU | Portable fallback selected by benchmarks | Prefer BSD/MIT/Apache-2.0 |

KFR is benchmark-only. It must never be linked into, packaged with, or enabled in
a production artifact unless a commercial license is acquired.

## Phase 1: Define requirements

Document the workload before selecting final backends:

- Transform types: complex-to-complex, real-to-complex, and complex-to-real.
- Precision: `float32` initially; add `float64` only if required.
- Dimensions: start with 1D unless multidimensional transforms are required.
- Supported lengths, especially whether all lengths are powers of two.
- Typical and maximum batch sizes.
- In-place versus out-of-place requirements.
- Latency, throughput, memory, and initialization-time targets.
- Threading policy and whether FFT execution occurs on real-time threads.
- Supported operating systems and minimum CPU/OS versions.

Deliverable: `docs/requirements.md` containing a fixed benchmark size matrix and
acceptance thresholds.

## Phase 2: Design a normalized API

Expose a small plan-based API independent of backend details:

```cpp
enum class FftDirection { Forward, Inverse };
enum class FftType { Complex, RealToComplex, ComplexToReal };
enum class FftPrecision { F32, F64 };

struct FftPlanConfig {
    FftType type;
    FftPrecision precision;
    size_t length;
    size_t batch;
};

class FftPlan {
public:
    static Result<FftPlan> create(const FftPlanConfig&);
    Status execute(const void* input, void* output) const;
    size_t workspace_bytes() const;
};
```

The public contract must specify:

- Interleaved complex representation.
- Real-FFT output as `N / 2 + 1` complex bins.
- Forward and inverse sign convention.
- A single normalization rule. Prefer an unscaled forward transform and a
  `1/N`-scaled inverse transform.
- Alignment and aliasing requirements.
- Whether plans are thread-safe and whether concurrent execution needs separate
  workspaces.
- No allocation, locks, or plan creation inside `execute`.

Implement explicit adapters for backend-specific packed real formats, scaling,
temporary buffers, and in-place restrictions.

## Phase 3: Backend structure

Use compile-time platform selection rather than loading every vendor library into
one binary:

```text
include/fft/fft.h
src/common/
src/backends/vdsp/
src/backends/cmsis/
src/backends/mkl/
src/backends/aocl/
src/backends/portable/
bench/backends/kfr/
tests/
cmake/
```

Each backend implements the same internal interface:

```cpp
struct BackendPlan {
    virtual ~BackendPlan() = default;
    virtual Status execute(const void*, void*) const = 0;
    virtual size_t workspace_bytes() const = 0;
};
```

Avoid virtual dispatch inside repeated FFT loops if measurements show it matters;
the public plan can store a function pointer and opaque backend state instead.

Backend notes:

- vDSP: normalize split/interleaved representation and packed real output.
- CMSIS-DSP: enable NEON for AArch64 builds, account for its temporary-buffer
  requirements, and reject unsupported lengths during plan creation.
- oneMKL: use DFT descriptors, commit once, and execute repeatedly.
- AOCL-FFTW: create and retain plans; never include planning time in execution
  benchmarks.
- Portable fallback: choose only after representative benchmarking. Candidates
  include PFFFT for constrained power-of-two 1D workloads and PocketFFT for
  broader length support.

## Phase 4: Build and licensing controls

Add explicit CMake options:

```text
FFT_BACKEND=auto|vdsp|cmsis|mkl|aocl|portable
FFT_ENABLE_BENCHMARKS=OFF
FFT_ENABLE_KFR_BENCHMARK=OFF
```

Required safeguards:

- `FFT_ENABLE_KFR_BENCHMARK` defaults to `OFF`.
- KFR code resides only below `bench/` and cannot be referenced by production
  targets.
- Release packaging fails if a release target links KFR.
- CI inspects dynamic dependencies and packaged files for forbidden libraries.
- Vendor redistributable files are copied from explicit allowlists.
- Store license texts, versions, source URLs, and notices in
  `third_party/manifest.yml`.
- Have counsel review AOCL and oneMKL redistribution terms for the exact versions
  shipped.

## Phase 5: Correctness tests

Run the same tests against every backend:

- Impulse, constant, sinusoid, Nyquist-frequency, and random vectors.
- Forward transform compared with a slow reference DFT for small sizes.
- Forward/inverse round-trip error.
- Real and complex transforms.
- Every supported length and representative batches.
- In-place/out-of-place behavior where supported.
- Unaligned inputs if the public contract permits them.
- NaN, infinity, zero-length, overflow, and unsupported-length handling.
- Concurrent execution tests according to the documented thread-safety contract.

Use precision-specific absolute and relative tolerances. Compare output after
normalizing all backend conventions.

## Phase 6: Benchmark harness

Benchmark production candidates plus KFR as an internal comparison:

- Warm up the CPU and backend.
- Pin the benchmark thread where the OS permits it.
- Reuse plans and preallocated buffers.
- Measure plan creation separately from execution.
- Measure median, p95, and p99 latency as well as transforms per second.
- Test cold-cache and warm-cache cases.
- Test single transforms and realistic batches.
- Test one thread first; benchmark backend threading separately.
- Record CPU model, ISA features, OS, compiler, flags, backend version, clock
  policy, and thermal state.

Suggested initial length matrix:

```text
64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384
```

Add actual production lengths and selected non-power-of-two lengths if required.
Run on at least:

- Apple Silicon from the oldest supported generation and one current generation.
- A recent Android flagship Arm CPU.
- A lower-power or older Android Arm CPU.
- Intel x86-64 with AVX2.
- Intel x86-64 with AVX-512 if supported by the product.
- AMD Zen 3 or later.

Do not select a backend from vendor-published benchmark results. Select from this
harness using the production workload.

## Phase 7: Backend decision gates

Keep a backend only if it:

1. Passes all normalized correctness tests.
2. Has acceptable commercial redistribution terms.
3. Supports every required transform shape.
4. Meets binary-size and initialization constraints.
5. Produces a material performance improvement over the portable fallback.

Suggested threshold: a vendor-specific backend must improve the primary workload
by at least 15% to justify its packaging and maintenance cost. Adjust this based
on how much total application time is spent in FFT execution.

If oneMKL or AOCL-FFTW fails this threshold, use one permissively licensed x86
fallback rather than maintaining vendor dispatch.

## Phase 8: Integration and rollout

- Add backend identity and version to diagnostic output.
- Add an environment or development-only override for backend selection.
- Never let production silently fall back after backend initialization errors;
  report the selected backend and failure clearly.
- Run correctness tests on every supported architecture in CI.
- Run shorter performance smoke tests in CI and full benchmarks on dedicated,
  stable hardware.
- Establish regression limits per machine rather than comparing results across
  unrelated hosts.
- Ship behind a feature flag, collect timing telemetry where permitted, then
  remove the previous FFT implementation after validation.

## Initial milestones

1. Requirements and benchmark matrix.
2. Normalized API plus reference DFT tests.
3. vDSP and CMSIS-DSP adapters.
4. Portable x86 backend and benchmark harness.
5. oneMKL, AOCL-FFTW, and benchmark-only KFR integrations.
6. Cross-platform benchmark report and final backend decision.
7. Packaging/license audit and production rollout.

## Open decisions

- Exact transform sizes and batches.
- Whether non-power-of-two lengths are required.
- Whether `float64`, multidimensional transforms, or internal threading are
  required.
- Minimum Android, Apple, Windows, and Linux versions.
- Whether Intel and AMD backends clear the performance threshold versus the
  permissive x86 fallback.
