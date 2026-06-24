// Intel oneMKL DFTI backend (Phase 3).
//
// Creates a DFTI descriptor and commits it once at plan creation; execute only
// calls DftiComputeForward/Backward. Batching uses DFTI_NUMBER_OF_TRANSFORMS so
// execute is a single MKL call with no manual loop.
//
// Contract mapping:
//   - Complex:        DFTI_COMPLEX, N interleaved complex in/out.
//   - RealToComplex:  DFTI_REAL forward, CCE complex-complex storage -> N/2+1
//                     complex bins (matches real_complex_bins(N)).
//   - ComplexToReal:  DFTI_REAL backward from N/2+1 bins -> N reals.
//   - Normalization:  forward unscaled; inverse / complex-to-real use
//                     DFTI_BACKWARD_SCALE = 1/N.
#include <complex>
#include <cstddef>

#include <mkl_dfti.h>

#include "common/backend.h"
#include "fft/fft.h"

namespace fft {
namespace {

bool dfti_ok(MKL_LONG status) {
    return status == 0 || DftiErrorClass(status, DFTI_NO_ERROR);
}

struct MklState {
    DFTI_DESCRIPTOR_HANDLE handle = nullptr;
    bool inverse = false;
    ~MklState() {
        if (handle) DftiFreeDescriptor(&handle);
    }
};

Status mkl_execute(void* state_ptr, const void* input, void* output) {
    auto& s = *static_cast<MklState*>(state_ptr);
    void* in = const_cast<void*>(input);
    MKL_LONG st = s.inverse ? DftiComputeBackward(s.handle, in, output)
                            : DftiComputeForward(s.handle, in, output);
    return dfti_ok(st) ? Status::Ok : Status::BackendError;
}

void mkl_destroy(void* state_ptr) { delete static_cast<MklState*>(state_ptr); }

}  // namespace

Result<BackendPlan> make_backend_plan(const FftPlanConfig& cfg) {
    if (cfg.length == 0 || cfg.batch == 0) return Status::InvalidArgument;

    const DFTI_CONFIG_VALUE prec =
        (cfg.precision == FftPrecision::F64) ? DFTI_DOUBLE : DFTI_SINGLE;
    const MKL_LONG n = static_cast<MKL_LONG>(cfg.length);
    const MKL_LONG bins = static_cast<MKL_LONG>(cfg.length / 2 + 1);
    const MKL_LONG batch = static_cast<MKL_LONG>(cfg.batch);

    bool inverse;
    DFTI_CONFIG_VALUE domain;
    MKL_LONG in_dist, out_dist;
    switch (cfg.type) {
        case FftType::Complex:
            domain = DFTI_COMPLEX;
            inverse = (cfg.direction == FftDirection::Inverse);
            in_dist = n;
            out_dist = n;
            break;
        case FftType::RealToComplex:
            if (cfg.direction == FftDirection::Inverse) return Status::InvalidArgument;
            domain = DFTI_REAL;
            inverse = false;
            in_dist = n;
            out_dist = bins;
            break;
        case FftType::ComplexToReal:
            domain = DFTI_REAL;
            inverse = true;
            in_dist = bins;
            out_dist = n;
            break;
        default:
            return Status::UnsupportedConfig;
    }

    DFTI_DESCRIPTOR_HANDLE h = nullptr;
    if (!dfti_ok(DftiCreateDescriptor(&h, prec, domain, 1, n))) return Status::BackendError;

    bool ok = true;
    ok = ok && dfti_ok(DftiSetValue(h, DFTI_PLACEMENT, DFTI_NOT_INPLACE));
    if (domain == DFTI_REAL) {
        ok = ok && dfti_ok(DftiSetValue(h, DFTI_CONJUGATE_EVEN_STORAGE,
                                        DFTI_COMPLEX_COMPLEX));
    }
    ok = ok && dfti_ok(DftiSetValue(h, DFTI_NUMBER_OF_TRANSFORMS, batch));
    ok = ok && dfti_ok(DftiSetValue(h, DFTI_INPUT_DISTANCE, in_dist));
    ok = ok && dfti_ok(DftiSetValue(h, DFTI_OUTPUT_DISTANCE, out_dist));
    if (inverse) {
        ok = ok && dfti_ok(DftiSetValue(h, DFTI_BACKWARD_SCALE,
                                        1.0 / static_cast<double>(cfg.length)));
    }
    ok = ok && dfti_ok(DftiCommitDescriptor(h));
    if (!ok) {
        DftiFreeDescriptor(&h);
        return Status::BackendError;
    }

    auto* state = new MklState();
    state->handle = h;
    state->inverse = inverse;

    BackendPlan bp;
    bp.state = state;
    bp.execute = &mkl_execute;
    bp.destroy = &mkl_destroy;
    bp.workspace_bytes = 0;  // MKL manages its own scratch internally
    bp.name = "mkl";
    return bp;
}

}  // namespace fft
