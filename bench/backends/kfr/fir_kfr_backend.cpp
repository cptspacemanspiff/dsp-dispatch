// KFR FIR backend — BENCHMARK ONLY.
//
// KFR is GPL-2.0/commercial and must never be linked into or packaged with a
// production artifact. This adapter lives under bench/ and is only reachable
// from fir_bench_kfr.
#include <cstddef>
#include <memory>
#include <vector>

#include <kfr/dsp/fir.hpp>

#include "common/fir_backend.h"
#include "fir/fir.h"

namespace fir {
namespace {

struct KfrRealState {
    KfrRealState(const FirPlanConfig& cfg) : block_size(cfg.block_size), batch(cfg.batch) {
        kfr::univector<float> taps(cfg.tap_count);
        const auto* in_taps = static_cast<const float*>(cfg.taps);
        for (std::size_t i = 0; i < cfg.tap_count; ++i) taps[i] = in_taps[i];

        filters.reserve(batch);
        for (std::size_t i = 0; i < batch; ++i) {
            filters.push_back(std::make_unique<kfr::filter_fir<float, float>>(taps));
        }
    }

    std::size_t block_size;
    std::size_t batch;
    std::vector<std::unique_ptr<kfr::filter_fir<float, float>>> filters;
};

Status kfr_real_execute(void* state_ptr, const void* input, void* output) {
    auto& s = *static_cast<KfrRealState*>(state_ptr);
    const auto* in = static_cast<const float*>(input);
    auto* out = static_cast<float*>(output);
    for (std::size_t b = 0; b < s.batch; ++b) {
        s.filters[b]->apply(out + b * s.block_size, in + b * s.block_size, s.block_size);
    }
    return Status::Ok;
}

Status kfr_real_clear(void* state_ptr) {
    auto& s = *static_cast<KfrRealState*>(state_ptr);
    for (auto& filter : s.filters) filter->reset();
    return Status::Ok;
}

void kfr_real_destroy(void* state_ptr) {
    delete static_cast<KfrRealState*>(state_ptr);
}

bool invalid_common(const FirPlanConfig& cfg) {
    return cfg.taps == nullptr || cfg.tap_count == 0 || cfg.block_size == 0 || cfg.batch == 0;
}

}  // namespace

Result<BackendPlan> make_backend_plan(const FirPlanConfig& cfg) {
    if (invalid_common(cfg)) return Status::InvalidArgument;
    if (cfg.type != FirType::Real || cfg.precision != FirPrecision::F32)
        return Status::UnsupportedConfig;

    auto* s = new KfrRealState(cfg);
    BackendPlan bp;
    bp.state = s;
    bp.execute = &kfr_real_execute;
    bp.clear = &kfr_real_clear;
    bp.destroy = &kfr_real_destroy;
    bp.workspace_bytes = cfg.batch * cfg.tap_count * sizeof(float);
    bp.name = "kfr";
    return bp;
}

}  // namespace fir
