// KFR backend — BENCHMARK ONLY. Implements the internal BackendPlan over KFR's
// dft_plan so KFR is measured through the exact same harness, buffers, and
// normalization as every other backend (apples-to-apples).
//
// KFR is GPL-2.0/commercial and must never be linked into or packaged with a
// production artifact. This file lives under bench/ (never src/), the build
// forbids it when FFT_PACKAGING is ON, and the bench target has no install rule.
//
// Complex-to-complex only for now (the standard benchmark exercises c2c f32/f64);
// real transforms return UnsupportedConfig until needed.
#include <complex>
#include <cstddef>
#include <vector>

#include <kfr/dft.hpp>

#include "common/backend.h"
#include "fft/fft.h"

namespace fft {
namespace {

template <class T>
struct KfrState {
    KfrState(std::size_t n, std::size_t batch, bool inverse)
        : n(n), batch(batch), inverse(inverse), plan(n) {
        temp.resize(plan.temp_size);
    }

    std::size_t n;
    std::size_t batch;
    bool inverse;
    kfr::dft_plan<T> plan;
    std::vector<kfr::u8> temp;  // reused per execute (plan is not concurrency-safe)
};

template <class T>
Status kfr_execute(void* state_ptr, const void* input, void* output) {
    using kc = kfr::complex<T>;  // layout-compatible with std::complex<T>
    auto& s = *static_cast<KfrState<T>*>(state_ptr);
    const auto* in = static_cast<const std::complex<T>*>(input);
    auto* out = static_cast<std::complex<T>*>(output);

    for (std::size_t b = 0; b < s.batch; ++b) {
        const kc* bin = reinterpret_cast<const kc*>(in + b * s.n);
        kc* bout = reinterpret_cast<kc*>(out + b * s.n);
        s.plan.execute(bout, bin, s.temp.data(), s.inverse);
        if (s.inverse) {  // public contract: inverse is 1/N scaled (KFR is unscaled)
            const T inv_n = static_cast<T>(1) / static_cast<T>(s.n);
            auto* o = out + b * s.n;
            for (std::size_t i = 0; i < s.n; ++i) o[i] *= inv_n;
        }
    }
    return Status::Ok;
}

template <class T>
void kfr_destroy(void* state_ptr) {
    delete static_cast<KfrState<T>*>(state_ptr);
}

template <class T>
BackendPlan make_typed(const FftPlanConfig& cfg, bool inverse) {
    auto* s = new KfrState<T>(cfg.length, cfg.batch, inverse);
    BackendPlan bp;
    bp.state = s;
    bp.execute = &kfr_execute<T>;
    bp.destroy = &kfr_destroy<T>;
    bp.workspace_bytes = s->temp.size();
    bp.name = "kfr";
    return bp;
}

}  // namespace

Result<BackendPlan> make_backend_plan(const FftPlanConfig& cfg) {
    if (cfg.length == 0 || cfg.batch == 0) return Status::InvalidArgument;
    if (cfg.type != FftType::Complex) return Status::UnsupportedConfig;
    const bool inverse = (cfg.direction == FftDirection::Inverse);
    switch (cfg.precision) {
        case FftPrecision::F32: return make_typed<float>(cfg, inverse);
        case FftPrecision::F64: return make_typed<double>(cfg, inverse);
        default: return Status::UnsupportedConfig;
    }
}

}  // namespace fft
