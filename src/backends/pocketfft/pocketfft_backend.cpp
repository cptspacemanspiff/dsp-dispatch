// PocketFFT backend (Phase 3).
//
// PocketFFT (mreineck/pocketfft, the header-only C++ variant) is a compact,
// dependency-free, BSD-3-Clause FFT that builds and runs identically on x86 and
// AArch64 -- so unlike the vendor backends (oneMKL/AOCL on x86, ArmPL on Arm) it
// is a single portable backend for every host, and a redistributable one. It
// supports complex, real-to-complex, and complex-to-real transforms of ANY
// length in f32/f64, which is why the manifest lists it as the broad-length
// portable fallback candidate alongside the in-tree radix-2/Bluestein backend.
//
// The public pocketfft::{c2c,r2c,c2r} API is stride-based and takes the output
// scale factor directly, which maps cleanly onto the normalized contract:
//   - Complex:        c2c over shape {batch, N} along axis 1; forward unscaled,
//                     inverse scaled by 1/N.
//   - RealToComplex:  r2c along axis 1; N/2+1 complex bins (== real_complex_bins).
//   - ComplexToReal:  c2r along axis 1 from N/2+1 bins -> N reals, scaled 1/N.
// Batching is a single call: the {batch, N} shape transforms each row along axis
// 1 with the batch stride as the outer dimension -- no manual loop.
//
// Plan handling: pocketfft's plans (twiddle factors) live in an internal
// length-keyed cache; POCKETFFT_CACHE_SIZE (set below) enables it, and plan
// creation pre-warms it with one throwaway transform so the first execute reuses
// the cached plan. (pocketfft still allocates a small transform scratch inside
// each call -- inherent to the header-only design; the in-tree portable backend
// remains the strictly allocation-free one.) Single-threaded: nthreads is always
// 1 and POCKETFFT_NO_MULTITHREADING compiles out the threading path.
#define POCKETFFT_NO_MULTITHREADING
#define POCKETFFT_CACHE_SIZE 16

#include <complex>
#include <cstddef>
#include <vector>

#include "pocketfft_hdronly.h"

#include "common/backend.h"
#include "fft/fft.h"

namespace fft {
namespace {

using pocketfft::shape_t;
using pocketfft::stride_t;

template <class Real>
struct PocketState {
    FftType type = FftType::Complex;
    bool inverse = false;
    std::size_t n = 0;
    std::size_t batch = 0;
    // Precomputed at plan creation; execute only reads these.
    shape_t shape;          // {batch, N} for every type (input real shape / output real shape)
    shape_t axes;           // {1} -- the transform axis (axis 0 is the batch)
    stride_t stride_in;     // byte strides of the input buffer
    stride_t stride_out;    // byte strides of the output buffer
};

// One transform through pocketfft's public API. Shared by execute and the
// plan-warming call so both take the identical path.
template <class Real>
Status pocket_run(PocketState<Real>& s, const void* input, void* output) {
    using C = std::complex<Real>;
    const Real fct = s.inverse ? Real(1) / static_cast<Real>(s.n) : Real(1);
    try {
        switch (s.type) {
            case FftType::Complex:
                pocketfft::c2c(s.shape, s.stride_in, s.stride_out, s.axes, /*forward=*/!s.inverse,
                               static_cast<const C*>(input), static_cast<C*>(output), fct);
                break;
            case FftType::RealToComplex:
                pocketfft::r2c(s.shape, s.stride_in, s.stride_out, /*axis=*/1, /*forward=*/true,
                               static_cast<const Real*>(input), static_cast<C*>(output), fct);
                break;
            case FftType::ComplexToReal:
                pocketfft::c2r(s.shape, s.stride_in, s.stride_out, /*axis=*/1, /*forward=*/false,
                               static_cast<const C*>(input), static_cast<Real*>(output), fct);
                break;
            default:
                return Status::UnsupportedConfig;
        }
    } catch (...) {
        return Status::BackendError;  // pocketfft throws on invalid configurations
    }
    return Status::Ok;
}

template <class Real>
Status pocket_execute(void* state_ptr, const void* input, void* output) {
    return pocket_run(*static_cast<PocketState<Real>*>(state_ptr), input, output);
}

template <class Real>
void pocket_destroy(void* state_ptr) {
    delete static_cast<PocketState<Real>*>(state_ptr);
}

template <class Real>
Result<BackendPlan> make_typed(const FftPlanConfig& cfg, bool inverse) {
    using C = std::complex<Real>;
    const std::size_t n = cfg.length;
    const std::size_t batch = cfg.batch;
    const std::size_t bins = n / 2 + 1;
    const auto cb = static_cast<std::ptrdiff_t>(sizeof(C));
    const auto rb = static_cast<std::ptrdiff_t>(sizeof(Real));

    auto* st = new PocketState<Real>();
    st->type = cfg.type;
    st->inverse = inverse;
    st->n = n;
    st->batch = batch;
    st->shape = shape_t{batch, n};
    st->axes = shape_t{1};

    // Byte strides for the {batch, N} layout: outer = one whole transform, inner
    // = one element. Complex sides step by sizeof(complex); real sides by
    // sizeof(Real); the real spectrum's transform axis has N/2+1 complex bins.
    std::size_t in_bytes, out_bytes;  // throwaway warm-buffer sizes
    switch (cfg.type) {
        case FftType::Complex:
            st->stride_in = stride_t{static_cast<std::ptrdiff_t>(n) * cb, cb};
            st->stride_out = st->stride_in;
            in_bytes = out_bytes = n * batch * sizeof(C);
            break;
        case FftType::RealToComplex:
            st->stride_in = stride_t{static_cast<std::ptrdiff_t>(n) * rb, rb};
            st->stride_out = stride_t{static_cast<std::ptrdiff_t>(bins) * cb, cb};
            in_bytes = n * batch * sizeof(Real);
            out_bytes = bins * batch * sizeof(C);
            break;
        case FftType::ComplexToReal:
            st->stride_in = stride_t{static_cast<std::ptrdiff_t>(bins) * cb, cb};
            st->stride_out = stride_t{static_cast<std::ptrdiff_t>(n) * rb, rb};
            in_bytes = bins * batch * sizeof(C);
            out_bytes = n * batch * sizeof(Real);
            break;
        default:
            delete st;
            return Status::UnsupportedConfig;
    }

    // Pre-warm the length-keyed plan cache so the first real execute reuses the
    // committed plan instead of building twiddles on the hot path.
    std::vector<char> warm_in(in_bytes, 0), warm_out(out_bytes, 0);
    if (pocket_run(*st, warm_in.data(), warm_out.data()) != Status::Ok) {
        delete st;
        return Status::BackendError;
    }

    BackendPlan bp;
    bp.state = st;
    bp.execute = &pocket_execute<Real>;
    bp.destroy = &pocket_destroy<Real>;
    bp.workspace_bytes = 0;  // pocketfft owns its per-call transform scratch internally
    bp.name = "pocketfft";
    return bp;
}

}  // namespace

Result<BackendPlan> make_backend_plan(const FftPlanConfig& cfg) {
    if (cfg.length == 0 || cfg.batch == 0) return Status::InvalidArgument;

    bool inverse;
    switch (cfg.type) {
        case FftType::Complex:
            inverse = (cfg.direction == FftDirection::Inverse);
            break;
        case FftType::RealToComplex:
            if (cfg.direction == FftDirection::Inverse) return Status::InvalidArgument;
            inverse = false;
            break;
        case FftType::ComplexToReal:
            inverse = true;
            break;
        default:
            return Status::UnsupportedConfig;
    }

    return (cfg.precision == FftPrecision::F64) ? make_typed<double>(cfg, inverse)
                                                : make_typed<float>(cfg, inverse);
}

}  // namespace fft
