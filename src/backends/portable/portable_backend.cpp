// Portable backend: a virtual-free BackendPlan over FftEngine<T>.
//
// Data contract (see docs/requirements.md):
//   - Complex:        in/out are N interleaved complex values.
//   - RealToComplex:  in is N reals; out is (N/2+1) interleaved complex bins.
//   - ComplexToReal:  in is (N/2+1) interleaved complex bins; out is N reals.
//   - Normalization:  forward unscaled; inverse and ComplexToReal scaled by 1/N.
//
// State is held in a templated POD struct; execute/destroy are free function
// templates referenced by pointer from the BackendPlan record.
#include <complex>
#include <cstddef>
#include <vector>

#include "backends/portable/fft_engine.h"
#include "common/backend.h"
#include "fft/fft.h"

namespace fft {
namespace {

template <class T>
struct PortableState {
    using Complex = std::complex<T>;

    PortableState(const FftPlanConfig& cfg, bool inverse)
        : type(cfg.type),
          n(cfg.length),
          batch(cfg.batch),
          inverse(inverse),
          engine(cfg.length) {
        cbuf.assign(n, Complex(0, 0));
        scratch.assign(engine.scratch_complex_count(), Complex(0, 0));
    }

    // Unscaled forward, or inverse via the conj-forward-conj identity with the
    // 1/N normalization folded in. Operates in place on cbuf (length n).
    void transform_cbuf() {
        if (!inverse) {
            engine.forward(cbuf.data(), scratch.data());
            return;
        }
        for (std::size_t i = 0; i < n; ++i) cbuf[i] = std::conj(cbuf[i]);
        engine.forward(cbuf.data(), scratch.data());
        const T inv_n = static_cast<T>(1) / static_cast<T>(n);
        for (std::size_t i = 0; i < n; ++i) cbuf[i] = std::conj(cbuf[i]) * inv_n;
    }

    FftType type;
    std::size_t n;
    std::size_t batch;
    bool inverse;
    portable::FftEngine<T> engine;
    std::vector<Complex> cbuf;     // length n; reused per execute (not thread-safe)
    std::vector<Complex> scratch;  // Bluestein convolution buffer (size m or 0)
};

template <class T>
Status portable_execute(void* state_ptr, const void* input, void* output) {
    using Complex = std::complex<T>;
    auto& s = *static_cast<PortableState<T>*>(state_ptr);
    const std::size_t n = s.n;
    const std::size_t bins = n / 2 + 1;

    for (std::size_t b = 0; b < s.batch; ++b) {
        switch (s.type) {
            case FftType::Complex: {
                const Complex* in = static_cast<const Complex*>(input) + b * n;
                Complex* out = static_cast<Complex*>(output) + b * n;
                for (std::size_t i = 0; i < n; ++i) s.cbuf[i] = in[i];
                s.transform_cbuf();
                for (std::size_t i = 0; i < n; ++i) out[i] = s.cbuf[i];
                break;
            }
            case FftType::RealToComplex: {
                const T* in = static_cast<const T*>(input) + b * n;
                Complex* out = static_cast<Complex*>(output) + b * bins;
                for (std::size_t i = 0; i < n; ++i) s.cbuf[i] = Complex(in[i], 0);
                s.transform_cbuf();  // forward
                for (std::size_t i = 0; i < bins; ++i) out[i] = s.cbuf[i];
                break;
            }
            case FftType::ComplexToReal: {
                const Complex* in = static_cast<const Complex*>(input) + b * bins;
                T* out = static_cast<T*>(output) + b * n;
                for (std::size_t i = 0; i < bins; ++i) s.cbuf[i] = in[i];
                for (std::size_t i = bins; i < n; ++i) s.cbuf[i] = std::conj(s.cbuf[n - i]);
                s.transform_cbuf();  // inverse, 1/N applied
                for (std::size_t i = 0; i < n; ++i) out[i] = s.cbuf[i].real();
                break;
            }
        }
    }
    return Status::Ok;
}

template <class T>
void portable_destroy(void* state_ptr) {
    delete static_cast<PortableState<T>*>(state_ptr);
}

template <class T>
BackendPlan make_typed(const FftPlanConfig& cfg, bool inverse) {
    auto* s = new PortableState<T>(cfg, inverse);
    BackendPlan bp;
    bp.state = s;
    bp.execute = &portable_execute<T>;
    bp.destroy = &portable_destroy<T>;
    bp.workspace_bytes = (s->cbuf.size() + s->scratch.size()) * sizeof(std::complex<T>);
    bp.name = "portable";
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
            // A real-to-complex transform is always forward.
            if (cfg.direction == FftDirection::Inverse) return Status::InvalidArgument;
            inverse = false;
            break;
        case FftType::ComplexToReal:
            // A complex-to-real transform is always inverse; direction ignored.
            inverse = true;
            break;
        default:
            return Status::UnsupportedConfig;
    }

    switch (cfg.precision) {
        case FftPrecision::F32: return make_typed<float>(cfg, inverse);
        case FftPrecision::F64: return make_typed<double>(cfg, inverse);
        default: return Status::UnsupportedConfig;
    }
}

}  // namespace fft
