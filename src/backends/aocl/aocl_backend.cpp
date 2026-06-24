// AMD AOCL-FFTZ backend (Phase 3).
//
// AOCL-FFTZ uses a problem-descriptor model: aoclfftz_setup_{f,d}(desc) commits
// a plan and returns a handle; aoclfftz_execute_io(handle, in, out) runs it on
// caller buffers; aoclfftz_destroy frees it. Batching uses the vecs[] tensor, so
// execute is a single library call with no manual loop.
//
// Contract mapping:
//   - Complex:        fft_type=Complex, N interleaved complex in/out.
//   - RealToComplex:  fft_type=Real forward; AOCL's real spectrum is N/2+1
//                     complex bins (matches real_complex_bins(N)).
//   - ComplexToReal:  fft_type=Real backward from N/2+1 bins -> N reals.
//   - Normalization:  AOCL forward/backward are unscaled; we apply 1/N on
//                     inverse and complex-to-real to match the public contract.
//
// Plans are out-of-place (in != out); the public in-place complex path is not
// offered by this backend.
#include <complex>
#include <cstddef>
#include <vector>

#include "aoclfftz.h"

#include "common/backend.h"
#include "fft/fft.h"

namespace fft {
namespace {

inline void* aoclfftz_setup(aoclfftz_prob_desc_f* p) { return aoclfftz_setup_f(p); }
inline void* aoclfftz_setup(aoclfftz_prob_desc_d* p) { return aoclfftz_setup_d(p); }

template <class Real> struct DescFor;
template <> struct DescFor<float> { using type = aoclfftz_prob_desc_f; };
template <> struct DescFor<double> { using type = aoclfftz_prob_desc_d; };

struct AoclState {
    bool f64 = false;
    bool inverse = false;
    FftType type = FftType::Complex;
    std::size_t n = 0;
    std::size_t batch = 0;
    void* handle = nullptr;
    // Stable storage referenced by the committed plan for its lifetime.
    aoclfftz_dim_t dims[1];
    aoclfftz_dim_t vecs[1];
    // Distinct, full-size buffers so out-of-place validation/planning at setup
    // time has valid memory; execute uses the caller's buffers via execute_io.
    std::vector<char> in_buf, out_buf;

    ~AoclState() {
        if (handle) aoclfftz_destroy(handle);
    }
};

template <class Real>
Status aocl_execute(void* state_ptr, const void* input, void* output) {
    auto& s = *static_cast<AoclState*>(state_ptr);
    if (aoclfftz_execute_io(s.handle, const_cast<void*>(input), output) != AOCLFFTZ_SUCCESS)
        return Status::BackendError;

    if (s.inverse) {  // AOCL is unscaled; apply the contract's 1/N.
        const Real inv = static_cast<Real>(1) / static_cast<Real>(s.n);
        const std::size_t count = s.n * s.batch;  // elements in the output
        if (s.type == FftType::Complex) {
            auto* o = static_cast<std::complex<Real>*>(output);
            for (std::size_t i = 0; i < count; ++i) o[i] *= inv;
        } else {  // ComplexToReal
            auto* o = static_cast<Real*>(output);
            for (std::size_t i = 0; i < count; ++i) o[i] *= inv;
        }
    }
    return Status::Ok;
}

void aocl_destroy(void* state_ptr) { delete static_cast<AoclState*>(state_ptr); }

// AOCL-FFTZ supports real transforms only for 7-smooth lengths (it excludes
// prime sizes greater than 7 and their multiples). Complex transforms have no
// such restriction.
bool aocl_real_length_supported(std::size_t n) {
    for (std::size_t p : {2u, 3u, 5u, 7u})
        while (n % p == 0) n /= p;
    return n == 1;
}

template <class Real>
BackendPlan make_typed(const FftPlanConfig& cfg, bool inverse) {
    using Desc = typename DescFor<Real>::type;
    const std::size_t n = cfg.length;
    const std::size_t batch = cfg.batch;
    const std::size_t bins = n / 2 + 1;
    const bool real = (cfg.type != FftType::Complex);

    // Distances between consecutive transforms, and setup-buffer sizes (in
    // scalars). Complex sides count complex elements (2 scalars each).
    std::size_t in_dist, out_dist, in_scalars, out_scalars;
    if (cfg.type == FftType::Complex) {
        in_dist = n;            out_dist = n;
        in_scalars = n * batch * 2; out_scalars = n * batch * 2;
    } else if (cfg.type == FftType::RealToComplex) {
        in_dist = n;            out_dist = bins;
        in_scalars = n * batch;     out_scalars = bins * batch * 2;
    } else {  // ComplexToReal
        in_dist = bins;         out_dist = n;
        in_scalars = bins * batch * 2; out_scalars = n * batch;
    }

    auto* st = new AoclState();
    st->f64 = (sizeof(Real) == 8);
    st->inverse = inverse;
    st->type = cfg.type;
    st->n = n;
    st->batch = batch;
    st->dims[0] = aoclfftz_dim_t{static_cast<INT32>(n), 1, 1};
    st->vecs[0] = aoclfftz_dim_t{static_cast<INT32>(batch),
                                 static_cast<INT32>(in_dist),
                                 static_cast<INT32>(out_dist)};
    st->in_buf.assign(in_scalars * sizeof(Real), 0);
    st->out_buf.assign(out_scalars * sizeof(Real), 0);

    Desc desc{};
    desc.in = reinterpret_cast<Real*>(st->in_buf.data());
    desc.out = reinterpret_cast<Real*>(st->out_buf.data());
    desc.dim_rank = 1;
    desc.vec_rank = 1;
    desc.dims = st->dims;
    desc.vecs = st->vecs;
    desc.flags.fft_type = real ? 1 : 0;
    desc.flags.fft_direction = inverse ? 1 : 0;
    desc.flags.storage_order = 0;
    desc.flags.fft_placement = 1;  // out-of-place
    desc.flags.transpose_mode = 0;
    desc.flags.bit_reproducibility = 0;
    desc.pthr_fft.num_threads = 1;
    desc.pthr_fft.dynamic_load_model = 0;
    desc.cntrl_params.opt_level = 3;  // cap at AVX512; dispatcher picks best <= CPU
    desc.cntrl_params.opt_off = 0;
    desc.cntrl_params.logger_mode = AOCLFFTZ_LOG_NONE;
    desc.cntrl_params.measure_stats = 0;

    st->handle = aoclfftz_setup(&desc);

    BackendPlan bp;
    bp.state = st;
    bp.execute = &aocl_execute<Real>;
    bp.destroy = &aocl_destroy;
    bp.workspace_bytes = st->in_buf.size() + st->out_buf.size();
    bp.name = "aocl";
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

    if (cfg.type != FftType::Complex && !aocl_real_length_supported(cfg.length))
        return Status::UnsupportedLength;

    BackendPlan bp = (cfg.precision == FftPrecision::F64)
                         ? make_typed<double>(cfg, inverse)
                         : make_typed<float>(cfg, inverse);
    if (bp.state == nullptr || static_cast<AoclState*>(bp.state)->handle == nullptr) {
        if (bp.state) aocl_destroy(bp.state);
        return Status::BackendError;
    }
    return bp;
}

}  // namespace fft
