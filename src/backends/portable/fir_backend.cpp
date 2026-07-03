#include <algorithm>
#include <complex>
#include <cstddef>
#include <vector>

#include "common/fir_backend.h"
#include "fir/fir.h"

namespace fir {
namespace {

template <class Sample, class Tap>
struct PortableState {
    PortableState(const FirPlanConfig& cfg)
        : block_size(cfg.block_size), batch(cfg.batch), tap_count(cfg.tap_count) {
        const auto* in_taps = static_cast<const Tap*>(cfg.taps);
        taps.assign(in_taps, in_taps + tap_count);
        history.assign(batch * history_len(), Sample{});
        combined.reserve(history_len() + block_size);
    }

    std::size_t history_len() const { return tap_count > 0 ? tap_count - 1 : 0; }

    std::size_t block_size;
    std::size_t batch;
    std::size_t tap_count;
    std::vector<Tap> taps;
    std::vector<Sample> history;
    std::vector<Sample> combined;
};

template <class Sample, class Tap>
Status portable_clear(void* state_ptr) {
    auto& s = *static_cast<PortableState<Sample, Tap>*>(state_ptr);
    std::fill(s.history.begin(), s.history.end(), Sample{});
    return Status::Ok;
}

template <class Sample, class Tap>
Status portable_execute(void* state_ptr, const void* input, void* output) {
    auto& s = *static_cast<PortableState<Sample, Tap>*>(state_ptr);
    const auto* in = static_cast<const Sample*>(input);
    auto* out = static_cast<Sample*>(output);
    const std::size_t hist_len = s.history_len();

    for (std::size_t b = 0; b < s.batch; ++b) {
        const Sample* bin = in + b * s.block_size;
        Sample* bout = out + b * s.block_size;
        Sample* hist = s.history.data() + b * hist_len;

        for (std::size_t i = 0; i < s.block_size; ++i) {
            Sample acc{};
            for (std::size_t k = 0; k < s.tap_count; ++k) {
                const Sample x = (i >= k) ? bin[i - k] : hist[hist_len + i - k];
                acc += x * s.taps[k];
            }
            bout[i] = acc;
        }

        if (hist_len == 0) continue;
        s.combined.clear();
        s.combined.insert(s.combined.end(), hist, hist + hist_len);
        s.combined.insert(s.combined.end(), bin, bin + s.block_size);
        std::copy(s.combined.end() - static_cast<std::ptrdiff_t>(hist_len),
                  s.combined.end(), hist);
    }

    return Status::Ok;
}

template <class Sample, class Tap>
void portable_destroy(void* state_ptr) {
    delete static_cast<PortableState<Sample, Tap>*>(state_ptr);
}

template <class Sample, class Tap>
BackendPlan make_typed(const FirPlanConfig& cfg) {
    auto* s = new PortableState<Sample, Tap>(cfg);
    BackendPlan bp;
    bp.state = s;
    bp.execute = &portable_execute<Sample, Tap>;
    bp.clear = &portable_clear<Sample, Tap>;
    bp.destroy = &portable_destroy<Sample, Tap>;
    bp.workspace_bytes = s->history.size() * sizeof(Sample) +
                         s->combined.capacity() * sizeof(Sample) +
                         s->taps.size() * sizeof(Tap);
    bp.name = "portable";
    return bp;
}

bool invalid_common(const FirPlanConfig& cfg) {
    return cfg.taps == nullptr || cfg.tap_count == 0 || cfg.block_size == 0 || cfg.batch == 0;
}

}  // namespace

Result<BackendPlan> make_backend_plan(const FirPlanConfig& cfg) {
    if (invalid_common(cfg)) return Status::InvalidArgument;

    switch (cfg.precision) {
        case FirPrecision::F32:
            switch (cfg.type) {
                case FirType::Real: return make_typed<float, float>(cfg);
                case FirType::ComplexRealTaps:
                    return make_typed<std::complex<float>, float>(cfg);
                case FirType::Complex:
                    return make_typed<std::complex<float>, std::complex<float>>(cfg);
            }
            break;
        case FirPrecision::F64:
            switch (cfg.type) {
                case FirType::Real: return make_typed<double, double>(cfg);
                case FirType::ComplexRealTaps:
                    return make_typed<std::complex<double>, double>(cfg);
                case FirType::Complex:
                    return make_typed<std::complex<double>, std::complex<double>>(cfg);
            }
            break;
    }
    return Status::UnsupportedConfig;
}

}  // namespace fir
