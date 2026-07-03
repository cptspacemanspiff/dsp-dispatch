// Arm Performance Libraries (ArmPL) FFT backend (Phase 3).
//
// ArmPL is the Arm counterpart to Intel oneMKL (x86) and AMD AOCL-FFTZ (x86):
// a vendor-tuned CPU math library for AArch64. Its FFT component is exposed
// through the standard FFTW3 interface -- ArmPL ships an <fftw3.h> and provides
// the fftwf_*/fftw_* symbols in libarmpl -- so this backend is written against
// the FFTW3 API but links ArmPL, not GPL FFTW. (ArmPL is redistributable under
// Arm's license, so unlike the benchmark-only FFTW backend under bench/, this is
// a production backend and lives under src/.)
//
// A plan is created and (for FFTW_MEASURE) committed once at plan creation over
// dedicated planning scratch; execute only calls the new-array execute entry
// (fftwf_execute_dft*) on the caller's buffers. The plan is created FFTW_UNALIGNED
// so it can run on arbitrary caller memory. Batching uses the "many" planner
// (fftw*_plan_many_dft*) so execute is a single library call with no manual loop.
//
// Contract mapping:
//   - Complex:        c2c many-DFT, N interleaved complex in/out, forward/inverse.
//   - RealToComplex:  r2c many-DFT, forward only; FFTW's half-complex spectrum is
//                     N/2+1 complex bins (matches real_complex_bins(N)).
//   - ComplexToReal:  c2r many-DFT, inverse only, from N/2+1 bins -> N reals.
//   - Normalization:  FFTW/ArmPL are unscaled; the public contract's forward is
//                     unscaled, so we apply 1/N on inverse c2c and on c2r.
//
// Plans are out-of-place (in != out); the public in-place complex path is not
// offered by this backend, matching the mkl/aocl vendor backends.
#include <complex>
#include <cstddef>

#include <fftw3.h>

#include "common/backend.h"
#include "fft/fft.h"

namespace fft {
namespace {

// Precision traits mapping FftPrecision::{F32,F64} onto the two FFTW symbol sets
// ArmPL provides (fftwf_* for float, fftw_* for double). Only the entry points
// this backend uses are wrapped.
template <class Real>
struct Fftw;

template <>
struct Fftw<float> {
    using Real = float;
    using Complex = fftwf_complex;
    using Plan = fftwf_plan;
    static Plan plan_many_dft(int rank, const int* n, int howmany, Complex* in, Complex* out,
                              int idist, int odist, int sign, unsigned flags) {
        return fftwf_plan_many_dft(rank, n, howmany, in, nullptr, 1, idist, out, nullptr, 1,
                                   odist, sign, flags);
    }
    static Plan plan_many_dft_r2c(int rank, const int* n, int howmany, Real* in, Complex* out,
                                  int idist, int odist, unsigned flags) {
        return fftwf_plan_many_dft_r2c(rank, n, howmany, in, nullptr, 1, idist, out, nullptr, 1,
                                       odist, flags);
    }
    static Plan plan_many_dft_c2r(int rank, const int* n, int howmany, Complex* in, Real* out,
                                  int idist, int odist, unsigned flags) {
        return fftwf_plan_many_dft_c2r(rank, n, howmany, in, nullptr, 1, idist, out, nullptr, 1,
                                       odist, flags);
    }
    static void execute_dft(Plan p, Complex* in, Complex* out) { fftwf_execute_dft(p, in, out); }
    static void execute_dft_r2c(Plan p, Real* in, Complex* out) { fftwf_execute_dft_r2c(p, in, out); }
    static void execute_dft_c2r(Plan p, Complex* in, Real* out) { fftwf_execute_dft_c2r(p, in, out); }
    static void destroy_plan(Plan p) { fftwf_destroy_plan(p); }
    static void* malloc(std::size_t n) { return fftwf_malloc(n); }
    static void free(void* p) { fftwf_free(p); }
};

template <>
struct Fftw<double> {
    using Real = double;
    using Complex = fftw_complex;
    using Plan = fftw_plan;
    static Plan plan_many_dft(int rank, const int* n, int howmany, Complex* in, Complex* out,
                              int idist, int odist, int sign, unsigned flags) {
        return fftw_plan_many_dft(rank, n, howmany, in, nullptr, 1, idist, out, nullptr, 1,
                                  odist, sign, flags);
    }
    static Plan plan_many_dft_r2c(int rank, const int* n, int howmany, Real* in, Complex* out,
                                  int idist, int odist, unsigned flags) {
        return fftw_plan_many_dft_r2c(rank, n, howmany, in, nullptr, 1, idist, out, nullptr, 1,
                                      odist, flags);
    }
    static Plan plan_many_dft_c2r(int rank, const int* n, int howmany, Complex* in, Real* out,
                                  int idist, int odist, unsigned flags) {
        return fftw_plan_many_dft_c2r(rank, n, howmany, in, nullptr, 1, idist, out, nullptr, 1,
                                      odist, flags);
    }
    static void execute_dft(Plan p, Complex* in, Complex* out) { fftw_execute_dft(p, in, out); }
    static void execute_dft_r2c(Plan p, Real* in, Complex* out) { fftw_execute_dft_r2c(p, in, out); }
    static void execute_dft_c2r(Plan p, Complex* in, Real* out) { fftw_execute_dft_c2r(p, in, out); }
    static void destroy_plan(Plan p) { fftw_destroy_plan(p); }
    static void* malloc(std::size_t n) { return fftw_malloc(n); }
    static void free(void* p) { fftw_free(p); }
};

template <class Real>
struct ArmplState {
    using F = Fftw<Real>;
    typename F::Plan plan = nullptr;
    FftType type = FftType::Complex;
    bool inverse = false;
    std::size_t n = 0;
    std::size_t batch = 0;
    // Aligned scratch that FFTW_MEASURE overwrites while probing plans; the real
    // execute runs on the caller's buffers via the new-array execute entry.
    void* in = nullptr;
    void* out = nullptr;

    ~ArmplState() {
        if (plan) F::destroy_plan(plan);
        if (in) F::free(in);
        if (out) F::free(out);
    }
};

// Apply the contract's 1/N on the paths FFTW leaves unscaled (inverse c2c, c2r).
template <class Real>
void normalize_inverse(ArmplState<Real>& s, void* output) {
    const Real inv = static_cast<Real>(1) / static_cast<Real>(s.n);
    const std::size_t count = s.n * s.batch;  // logical elements in the output
    if (s.type == FftType::Complex) {
        auto* o = static_cast<std::complex<Real>*>(output);
        for (std::size_t i = 0; i < count; ++i) o[i] *= inv;
    } else {  // ComplexToReal
        auto* o = static_cast<Real*>(output);
        for (std::size_t i = 0; i < count; ++i) o[i] *= inv;
    }
}

template <class Real>
Status armpl_execute(void* state_ptr, const void* input, void* output) {
    using F = Fftw<Real>;
    auto& s = *static_cast<ArmplState<Real>*>(state_ptr);
    // FFTW complex is Real[2], layout-compatible with std::complex<Real>.
    switch (s.type) {
        case FftType::Complex:
            F::execute_dft(s.plan,
                           reinterpret_cast<typename F::Complex*>(const_cast<void*>(input)),
                           reinterpret_cast<typename F::Complex*>(output));
            break;
        case FftType::RealToComplex:
            F::execute_dft_r2c(s.plan, reinterpret_cast<Real*>(const_cast<void*>(input)),
                               reinterpret_cast<typename F::Complex*>(output));
            break;
        case FftType::ComplexToReal:
            F::execute_dft_c2r(s.plan,
                               reinterpret_cast<typename F::Complex*>(const_cast<void*>(input)),
                               reinterpret_cast<Real*>(output));
            break;
        default:
            return Status::UnsupportedConfig;
    }
    if (s.inverse) normalize_inverse(s, output);
    return Status::Ok;
}

template <class Real>
void armpl_destroy(void* state_ptr) {
    delete static_cast<ArmplState<Real>*>(state_ptr);
}

template <class Real>
Result<BackendPlan> make_typed(const FftPlanConfig& cfg, bool inverse) {
    using F = Fftw<Real>;
    const std::size_t n = cfg.length;
    const std::size_t batch = cfg.batch;
    const std::size_t bins = n / 2 + 1;
    const int rank = 1;
    int dims[1] = {static_cast<int>(n)};
    // FFTW_MEASURE picks the best plan (its cost is captured by the separate
    // plan_create benchmark); FFTW_UNALIGNED lets the committed plan run on the
    // caller's own buffers via the new-array execute entry at execute time.
    const unsigned flags = FFTW_MEASURE | FFTW_UNALIGNED;

    // Planning-scratch element counts and per-transform distances, per type.
    std::size_t in_scratch, out_scratch;  // scalar (Real) counts
    int idist, odist;
    switch (cfg.type) {
        case FftType::Complex:
            idist = static_cast<int>(n);
            odist = static_cast<int>(n);
            in_scratch = n * batch * 2;
            out_scratch = n * batch * 2;
            break;
        case FftType::RealToComplex:
            idist = static_cast<int>(n);
            odist = static_cast<int>(bins);
            in_scratch = n * batch;
            out_scratch = bins * batch * 2;
            break;
        case FftType::ComplexToReal:
            idist = static_cast<int>(bins);
            odist = static_cast<int>(n);
            in_scratch = bins * batch * 2;
            out_scratch = n * batch;
            break;
        default:
            return Status::UnsupportedConfig;
    }

    auto* st = new ArmplState<Real>();
    st->type = cfg.type;
    st->inverse = inverse;
    st->n = n;
    st->batch = batch;
    st->in = F::malloc(sizeof(Real) * in_scratch);
    st->out = F::malloc(sizeof(Real) * out_scratch);
    if (st->in == nullptr || st->out == nullptr) {
        armpl_destroy<Real>(st);
        return Status::BackendError;
    }

    switch (cfg.type) {
        case FftType::Complex:
            st->plan = F::plan_many_dft(rank, dims, static_cast<int>(batch),
                                        static_cast<typename F::Complex*>(st->in),
                                        static_cast<typename F::Complex*>(st->out), idist, odist,
                                        inverse ? FFTW_BACKWARD : FFTW_FORWARD, flags);
            break;
        case FftType::RealToComplex:
            st->plan = F::plan_many_dft_r2c(rank, dims, static_cast<int>(batch),
                                            static_cast<Real*>(st->in),
                                            static_cast<typename F::Complex*>(st->out), idist,
                                            odist, flags);
            break;
        case FftType::ComplexToReal:
            st->plan = F::plan_many_dft_c2r(rank, dims, static_cast<int>(batch),
                                            static_cast<typename F::Complex*>(st->in),
                                            static_cast<Real*>(st->out), idist, odist, flags);
            break;
        default:
            break;
    }
    if (st->plan == nullptr) {
        armpl_destroy<Real>(st);
        return Status::BackendError;
    }

    BackendPlan bp;
    bp.state = st;
    bp.execute = &armpl_execute<Real>;
    bp.destroy = &armpl_destroy<Real>;
    bp.workspace_bytes = sizeof(Real) * (in_scratch + out_scratch);
    bp.name = "armpl";
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
