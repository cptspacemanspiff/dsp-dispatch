# FIR algorithm selection: direct vs. FFT (overlap-add/save)

Investigation notes + design proposal. Started from "I don't trust the IPP FIR
benchmark numbers." Ends with a proposal to make the direct/FFT algorithm a
per-plan config knob with an `Auto` chooser.

Host for all measured numbers below: `fedora-framework`, 16 CPUs, AVX-512,
release build. Source: `bench_results/fir_results.md`.

---

## 1. Is IPP single-threaded for the FIR filter?

**Yes — definitively single-threaded.** The suspicious benchmark numbers are
*not* a threading artifact. Three independent confirmations:

1. **Linkage.** The bench binary statically links only `libipps.a` (IPP's
   single-threaded core). None of the threading-layer archives that exist in
   `_ipp/lib/` (`libipp*_tl_omp.a`, `_tl_tbb.a`, `libipp_iw.a`) are pulled in.
   `ldd` shows zero OpenMP/TBB/IW dynamic deps; `nm` shows no `ippsTL` / `_T`
   threaded symbols — only the plain `d1_ippsFIRSR_32f` core primitives.
2. **API used.** `ippsFIRSR_*` are core primitives. Intel removed internal
   threading from IPP core back in IPP 9.0; only the separate TL (`_T`-suffixed)
   functions thread, and we don't call them. Our `ipp_execute` loop also just
   processes one contiguous block per call — no parallelism in our own code.
3. **Measured CPU%.** Running the heaviest case (`8192/taps/128`) under
   `/usr/bin/time -v` reports **99% of CPU**. A multithreaded FIR on this 16-core
   box would show many hundred %.

### The real reason the numbers look "too good": algorithm mismatch

IPP is initialized with `ippAlgAuto` (`ipp_fir_backend.cpp:136`), which lets IPP
switch from direct time-domain convolution to **FFT-based (overlap-save)
convolution** once the tap count is large enough. Every other backend here does
straight direct convolution. So at 128 taps we are benchmarking IPP's FFT
convolution against everyone else's direct convolution — apples to oranges.

Evidence: hold block size fixed, go 32 → 128 taps (4× the MACs):

| backend  | 8192×32 | 8192×128 | ratio |
|----------|---------|----------|-------|
| ipp      | 7.67    | 11.22    | **1.46×** |
| cmsis    | 40.1    | 165.2    | 4.12× |
| portable | 70.4    | 300.8    | 4.27× |

4× the work costs everyone ~4×… except IPP, which grows only 1.5×. That
sublinear scaling is the FFT-conv signature, and it's what inflates the "vs
portable" column to 15–27× at 128 taps. The IPP results are real and
single-threaded — just a different algorithm.

To force a like-for-like direct comparison, swap `ippAlgAuto` → `ippAlgDirect`
in the four `ippsFIRSRInit_*` calls; the giant 128-tap speedups should collapse
toward ~2×.

---

## 2. Dynamic block sizes into the IPP FIR

**IPP supports it fully; our wrapper currently does not.**

### IPP API level: yes, freely

`ippsFIRSR_32f(src, dst, numIters, spec, dlySrc, dlyDst, buf)` takes the sample
count `numIters` **per call**. Nothing about block size is baked into the plan:

- `ippsFIRSRGetSize(tapsLen, type, &specSize, &bufSize)` sizes spec and work
  buffer from **`tapsLen` only** (`ipp_fir_backend.cpp:107`) — not block size.
- For the FFT/overlap-save path IPP picks a fixed internal FFT length `L` from
  the tap count and segments the input internally, so any `numIters` just means
  more or fewer internal frames. No re-init, no re-alloc.
- The delay line is always `tapCount-1` samples regardless of `n`, so streaming
  continuity holds across calls of different sizes (n=10 then n=128 produces the
  correct continuous stream, no block-boundary artifacts).

### Our wrapper level: currently blocked

`FirPlan::execute(input, output)` has no length parameter — it always uses the
`block_size` frozen at plan creation (`fir.h:64`, `ipp_fir_backend.cpp:70`). To
support dynamic blocks, plumb a length through:

1. `FirPlan::execute(input, output, n)` — add per-call sample count.
2. `BackendPlan::execute` fn-pointer signature — add `n` (touches every backend).
3. In `ipp_execute`, use the passed `n` instead of `s.block_size`, and switch the
   batch stride to `b * n`. The delay-line math (`b * delay_len`) is already
   block-size-independent.

Suggested: add an overload `execute(input, output, n)` defaulting `n` to the
plan's `block_size` so existing callers and other backends keep working.

---

## 3. How IPP routes to overlap-save when called with a small block

**The routing decision is made at Init, not per execute.** `ippAlgAuto` is
consumed by `ippsFIRSRInit_*`, keyed on `tapsLen`, and baked into the spec (for
the FFT path it precomputes and stores the filter spectrum and a fixed FFT length
`L`). `ippsFIRSR_*` at execute just reads the method out of the spec. There is no
per-call re-decision based on `numIters`. That's why `GetSize` is
block-size-independent.

So calling a 128-tap FFT-routed spec with 10 samples does **not** fall back to
direct. Overlap-save runs with the pre-baked `L`:

1. Frame = `M-1` history samples (from `dlySrc`) + the 10 new inputs, zero-padded
   to `L`.
2. Forward FFT (`L`) → multiply by stored filter spectrum → inverse FFT (`L`).
3. Discard the first `M-1` wraparound outputs, keep the 10 valid ones.
4. Write the last `M-1` samples into `dlyDst` for next call.

Consequence: **you pay a full `L`-point FFT-pair to get 10 output samples.**
Filtering 10 samples costs roughly the same wall-time as filtering ~`L-M+1`
(~130) samples, because both are one frame. FFT only wins when `n` is large
enough to amortize the transform.

The benchmark already shows this: at 128 taps, block 64 is ~7× worse per sample
than block 8192 (97 M vs 731 M samples/s) — same filter, same FFT method, just
poor amortization at the small block.

*(Certain/observable: init-time routing, block-size-independent buffer, `M-1`
delay line. Inferred from OLS mechanics: the exact frame assembly — IPP core is
closed-source.)*

---

## 4. Do the other backends use overlap-save?

**No. Every FIR backend wired up here is direct time-domain convolution. IPP is
the only FFT-based one.**

| backend | core call | algorithm |
|---|---|---|
| portable | `acc += x * taps[k]` triple loop (`fir_backend.cpp:51`) | direct |
| cmsis | `arm_fir_f32` (`cmsis_fir_backend.cpp:40`) | direct |
| liquid | `firfilt_rrrf_execute_block` (`liquid_fir_backend.cpp:65`) | direct |
| kfr | `kfr::filter_fir::apply` (`fir_kfr_backend.cpp:40`) | direct |
| **ipp** | `ippsFIRSR` + `ippAlgAuto` | direct ≤~32 taps, FFT above |

(The sublinear tap-scaling visible in liquid/kfr is per-call overhead in those
wrappers dominating small MAC counts — verified direct from source, not OLS.)

### Getting an OLS backend outside IPP — three tiers

1. **Flip on an FFT path in a library already linked:**
   - **liquid-dsp `fftfilt`** — we use `firfilt` (direct); liquid also ships
     `fftfilt_rrrf` (overlap-add). Same dependency, **MIT-licensed → shippable.**
   - **KFR `convolve_filter<T>`** — we use `filter_fir` (direct); KFR also has a
     partitioned overlap-add convolver. GPL → benchmark-only.
2. **Roll our own on the in-tree portable FFT engine** (`fft_engine.h`):
   ~100 lines, no external dep, license-clean, **shippable and portable (x86 +
   ARM)**. This is the standout option. On AArch64 the same OLS driver can point
   at **ArmPL's FFTW3 interface** (already linked) for a vendor-tuned ARM path.
3. **Reuse the other FFT backends** (FFTW / MKL / AOCL) as swappable FFT engines
   under the same OLS driver for more comparison points.

### CMSIS specifically: no OLS

Verified against the fetched headers/sources. The whole filtering surface is
direct: `arm_fir_*` family and `arm_conv_f32 / arm_conv_partial_f32 /
arm_correlate_f32` (direct linear convolution, non-streaming). CMSIS's own source
tells you to build the FFT version yourself:

> Long versions: For convolution of long vectors, those functions are no more
> adapted and will be very slow. An implementation based upon FFTs should be
> used. — `arm_conv_f32.c:90`

CMSIS gives the ingredients (`arm_rfft_fast_f32`, `arm_cmplx_mult_cmplx_f32`) but
no packaged overlap-save filter.

### Any ARM library with a turnkey OLS?

**No ARM vendor library ships a turnkey overlap-save FIR the way IPP does.**
IPP's `ippAlgAuto` auto-routing is genuinely unusual.

- **Arm Performance Libraries (ArmPL)** — the AArch64 analog of IPP/MKL. Scope is
  BLAS/LAPACK/FFT/sparse. Strong FFT (the FFTW3-compatible one we already link),
  but **no packaged FIR/OLS.** Best FFT engine to *build* OLS on for ARM.
- **Apple Accelerate / vDSP** (Apple Silicon = ARM64) — direct `vDSP_conv` and
  FFTs, but no packaged streaming OLS FIR; assemble from `vDSP_fft` + `vDSP_zvmul`.
- **Ne10** — FFT + *direct* FIR, no OLS, effectively unmaintained.
- **Arm Compute Library** — FFT/Winograd conv, but for 2D CNN layers, not 1D
  streaming FIR.
- Cross-platform libs that run on ARM/NEON and *do* ship FFT-conv: KFR
  `convolve_filter`, liquid `fftfilt`.

Pattern holds across both ISAs: IPP is the outlier that bundles the FFT-FIR.
Reinforces building our own OLS on `fft_engine.h` with a swappable FFT underneath.

---

## 5. Proposed design: algorithm as a per-plan config knob

### Why unify (not two APIs)

Streaming FIR and FFT block-convolution compute the *identical* thing — an LTI
convolution with carried state. Overlap-add/save/direct are three ways to
evaluate one operation, not three capabilities. Collapse them behind one
`FirPlan` with an algorithm knob; `Auto` delegates the "how."

```cpp
enum class FirAlgorithm { Auto, Direct, Fft };   // see add/save note
struct FirPlanConfig {
    ...
    FirAlgorithm algorithm = FirAlgorithm::Auto;
    std::size_t  expected_block_size = 0;  // advisory hint, 0 = unknown
};
// execute(in, out, n)          // dynamic n
// plan.resolved_algorithm()    // what Auto actually picked — observable
```

### Subtlety 1: dynamic block sizes break Auto's information model

OLS only wins by amortizing one FFT-pair across a block; the value of Auto
depends on knowing the block size — which we're deliberately removing as a static
parameter. Two places the decision can live:

- **Plan-time (what IPP does):** decide once from `tap_count`, commit. Simple, but
  it's why IPP tanks at n=10 on a 128-tap plan — it can't back out.
- **Per-execute adaptive:** the plan holds *both* a direct kernel and the FFT/OLS
  engine, dispatching on the actual `n` per call. This is what makes "Auto +
  dynamic blocks" good, and it's a place we can beat IPP (which can't).

OLS setup needs only `tap_count` (`L` derives from the filter, not the block), so
building both engines at plan time costs no per-call allocation.

**Recommendation:** make "Auto delegates the decision" the contract; let
plan-time-vs-per-call be a hidden backend detail. Use `expected_block_size` as
tie-breaker. When block size is unknown (`0`), **bias Auto toward Direct** unless
`tap_count` is large enough that FFT wins for modest blocks — Direct is never
pathological; FFT-at-small-n is. That makes our Auto strictly safer than IPP's.

### Subtlety 2: don't promise add-vs-save control backends can't honor

| backend | Direct | Fft (add) | Fft (save) | Auto |
|---|---|---|---|---|
| portable | triple loop | build on `fft_engine.h` | build on `fft_engine.h` | threshold |
| ipp | `ippAlgDirect` | `ippAlgFFT` (add/save internal, **not selectable**) | ← same | `ippAlgAuto` |
| cmsis | `arm_fir` | build on `arm_rfft` | build on `arm_rfft` | threshold |
| liquid | `firfilt` | `fftfilt` (OLA) | ✗ **no OLS** | threshold |
| kfr | `filter_fir` | `convolve_filter` (OLA) | ✗ | threshold |
| armpl | build | build on ArmPL FFT | build on ArmPL FFT | threshold |

Forcing `OverlapSave` as a hard mode makes IPP, liquid, and KFR reject it. For a
user, direct-vs-FFT is the meaningful axis; add-vs-save is an implementation
detail with identical math. **Expose `{Auto, Direct, Fft}`;** if finer control is
wanted, add an optional `prefer_overlap = {Add, Save, DontCare}` hint that a
backend honors if it can and ignores otherwise — not a hard constraint.

### Capability negotiation policy

- **Explicit mode a backend can't do → `Status::UnsupportedConfig`** (predictable).
- **`Auto` never fails** — resolves to something, falling back to Direct worst case.
- **Always expose `resolved_algorithm()`** — needed to trust Auto and for the bench.

### Virtuous loop

The knob is what lets the benchmark generate direct-vs-FFT curves per backend and
*calibrate Auto's crossover empirically* instead of hardcoding a threshold.

---

## 6. Where is the direct → FFT crossover (how many taps)?

**It's a ratio, not a constant:** direct is O(M) per output, FFT/OLS is ~O(log M)
per output, so break-even is wherever `direct_cost(M) ≈ fft_cost(M)`:

> crossover ∝ (your FFT throughput) / (your direct-kernel throughput)

Better direct kernel → higher crossover. Better FFT → lower.

### Number for a *good* kernel — from our own data (block 8192, ns/output)

| backend | 8 taps | 32 taps | 128 taps | 32→128 scaling |
|---|---|---|---|---|
| ipp | 0.292 | 0.936 | 1.369 | **1.46×** (FFT) |
| cmsis | 1.510 | 4.899 | 20.169 | 4.12× (direct) |
| portable | 2.112 | 8.596 | 36.717 | 4.27× (direct) |

IPP stays direct through 32 taps, switches to FFT by 128. Extrapolating IPP's own
direct slope and meeting it against its FFT cost gives a **break-even ≈ 48 taps**
on this AVX-512 box — roughly block-size-independent for N ≥ 64. Matches the
textbook ~30–60 taps; IPP sits high because its direct kernel is excellent (a
great direct kernel pushes crossover *up*).

### The catch for *our* library

Our `portable`/`cmsis` direct kernels are 5–15× slower per MAC than IPP's
(cmsis ≈ 0.157 ns/out/tap; IPP ≈ 0.027). Against a weaker direct kernel a decent
FFT wins at far fewer taps:

- vs cmsis-direct: an FFT floor even 2× worse than IPP's beats it by **~16–24 taps**.
- vs portable-direct: crossover drops to **~10–16 taps**.

So the answer is bimodal:
- **Ship current direct backends** → OLS pays off at **~16–32 taps**.
- **Write an IPP-class SIMD/FMA direct kernel** → crossover rises to **~48–64 taps**.

### Block size still gates it

All of the above assumes a block large enough to amortize the FFT. For small
blocks (n ≪ M — the n=10 pathology), the crossover shoots up or FFT never wins.
Auto's real predicate is 2-D: `taps ≥ T_cross AND block ≳ 2·taps`.

### Practical default for Auto (pending measurement)

Ship a placeholder: **FFT when `taps ≥ 64` and `block ≥ max(256, 2·taps)`, else
Direct** — conservative, safe against the small-block trap. Then calibrate
`T_cross` per backend from a sweep (it's ~16 for the weak portable kernel, ~48 for
a strong one).

---

## 7. Next steps

- [ ] Add `execute(in, out, n)` overload; plumb `n` through `BackendPlan` and each
      backend (default `n = block_size`).
- [ ] Add `FirAlgorithm { Auto, Direct, Fft }` + `expected_block_size` to
      `FirPlanConfig`; add `resolved_algorithm()`; define the
      UnsupportedConfig-vs-fallback contract.
- [ ] Build a portable OLS backend on `fft_engine.h` (swappable FFT engine).
- [ ] Add intermediate tap counts to the FIR bench (16, 24, 32, 48, 64, 96, 128)
      × existing block sizes; locate the actual per-backend crossover.
- [ ] Calibrate Auto's `T_cross` per backend from that sweep.
- [ ] (Optional) Add an `ippAlgDirect` variant to the IPP backend to show the
      honest direct-vs-FFT curve side by side.
</content>
</invoke>
