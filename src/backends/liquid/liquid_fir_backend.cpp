#include <complex>
#include <cstddef>
#include <vector>

#if __has_include(<liquid/liquid.h>)
#include <liquid/liquid.h>
#else
#include <liquid.h>
#endif

#include "common/fir_backend.h"
#include "fir/fir.h"

namespace fir {
namespace {

struct LiquidRealState {
    LiquidRealState(const FirPlanConfig& cfg) : block_size(cfg.block_size), batch(cfg.batch) {
        const auto* taps = static_cast<const float*>(cfg.taps);
        filters.reserve(batch);
        for (std::size_t i = 0; i < batch; ++i) {
            filters.push_back(firfilt_rrrf_create(const_cast<float*>(taps),
                                                  static_cast<unsigned int>(cfg.tap_count)));
        }
    }
    std::size_t block_size;
    std::size_t batch;
    std::vector<firfilt_rrrf> filters;
};

struct LiquidComplexRealTapsState {
    LiquidComplexRealTapsState(const FirPlanConfig& cfg)
        : block_size(cfg.block_size), batch(cfg.batch) {
        const auto* taps = static_cast<const float*>(cfg.taps);
        filters.reserve(batch);
        for (std::size_t i = 0; i < batch; ++i) {
            filters.push_back(firfilt_crcf_create(const_cast<float*>(taps),
                                                  static_cast<unsigned int>(cfg.tap_count)));
        }
    }
    std::size_t block_size;
    std::size_t batch;
    std::vector<firfilt_crcf> filters;
};

struct LiquidComplexState {
    LiquidComplexState(const FirPlanConfig& cfg) : block_size(cfg.block_size), batch(cfg.batch) {
        const auto* taps = static_cast<const std::complex<float>*>(cfg.taps);
        filters.reserve(batch);
        for (std::size_t i = 0; i < batch; ++i) {
            filters.push_back(firfilt_cccf_create(const_cast<std::complex<float>*>(taps),
                                                  static_cast<unsigned int>(cfg.tap_count)));
        }
    }
    std::size_t block_size;
    std::size_t batch;
    std::vector<firfilt_cccf> filters;
};

Status liquid_real_execute(void* state_ptr, const void* input, void* output) {
    auto& s = *static_cast<LiquidRealState*>(state_ptr);
    const auto* in = static_cast<const float*>(input);
    auto* out = static_cast<float*>(output);
    for (std::size_t b = 0; b < s.batch; ++b) {
        firfilt_rrrf_execute_block(s.filters[b],
                                   const_cast<float*>(in + b * s.block_size),
                                   static_cast<unsigned int>(s.block_size),
                                   out + b * s.block_size);
    }
    return Status::Ok;
}

Status liquid_cr_execute(void* state_ptr, const void* input, void* output) {
    auto& s = *static_cast<LiquidComplexRealTapsState*>(state_ptr);
    const auto* in = static_cast<const std::complex<float>*>(input);
    auto* out = static_cast<std::complex<float>*>(output);
    for (std::size_t b = 0; b < s.batch; ++b) {
        firfilt_crcf_execute_block(s.filters[b],
                                   const_cast<std::complex<float>*>(in + b * s.block_size),
                                   static_cast<unsigned int>(s.block_size),
                                   out + b * s.block_size);
    }
    return Status::Ok;
}

Status liquid_complex_execute(void* state_ptr, const void* input, void* output) {
    auto& s = *static_cast<LiquidComplexState*>(state_ptr);
    const auto* in = static_cast<const std::complex<float>*>(input);
    auto* out = static_cast<std::complex<float>*>(output);
    for (std::size_t b = 0; b < s.batch; ++b) {
        firfilt_cccf_execute_block(s.filters[b],
                                   const_cast<std::complex<float>*>(in + b * s.block_size),
                                   static_cast<unsigned int>(s.block_size),
                                   out + b * s.block_size);
    }
    return Status::Ok;
}

Status liquid_real_clear(void* state_ptr) {
    auto& s = *static_cast<LiquidRealState*>(state_ptr);
    for (auto q : s.filters) firfilt_rrrf_reset(q);
    return Status::Ok;
}

Status liquid_cr_clear(void* state_ptr) {
    auto& s = *static_cast<LiquidComplexRealTapsState*>(state_ptr);
    for (auto q : s.filters) firfilt_crcf_reset(q);
    return Status::Ok;
}

Status liquid_complex_clear(void* state_ptr) {
    auto& s = *static_cast<LiquidComplexState*>(state_ptr);
    for (auto q : s.filters) firfilt_cccf_reset(q);
    return Status::Ok;
}

void liquid_real_destroy(void* state_ptr) {
    auto* s = static_cast<LiquidRealState*>(state_ptr);
    for (auto q : s->filters) firfilt_rrrf_destroy(q);
    delete s;
}

void liquid_cr_destroy(void* state_ptr) {
    auto* s = static_cast<LiquidComplexRealTapsState*>(state_ptr);
    for (auto q : s->filters) firfilt_crcf_destroy(q);
    delete s;
}

void liquid_complex_destroy(void* state_ptr) {
    auto* s = static_cast<LiquidComplexState*>(state_ptr);
    for (auto q : s->filters) firfilt_cccf_destroy(q);
    delete s;
}

bool invalid_common(const FirPlanConfig& cfg) {
    return cfg.taps == nullptr || cfg.tap_count == 0 || cfg.block_size == 0 || cfg.batch == 0;
}

}  // namespace

Result<BackendPlan> make_backend_plan(const FirPlanConfig& cfg) {
    if (invalid_common(cfg)) return Status::InvalidArgument;
    if (cfg.precision != FirPrecision::F32) return Status::UnsupportedConfig;
    if (cfg.tap_count > static_cast<std::size_t>(~0u)) return Status::InvalidArgument;
    if (cfg.block_size > static_cast<std::size_t>(~0u)) return Status::InvalidArgument;

    BackendPlan bp;
    bp.name = "liquid";
    switch (cfg.type) {
        case FirType::Real: {
            auto* s = new LiquidRealState(cfg);
            bp.state = s;
            bp.execute = &liquid_real_execute;
            bp.clear = &liquid_real_clear;
            bp.destroy = &liquid_real_destroy;
            bp.workspace_bytes = cfg.batch * cfg.tap_count * sizeof(float);
            return bp;
        }
        case FirType::ComplexRealTaps: {
            auto* s = new LiquidComplexRealTapsState(cfg);
            bp.state = s;
            bp.execute = &liquid_cr_execute;
            bp.clear = &liquid_cr_clear;
            bp.destroy = &liquid_cr_destroy;
            bp.workspace_bytes = cfg.batch * cfg.tap_count * sizeof(std::complex<float>);
            return bp;
        }
        case FirType::Complex: {
            auto* s = new LiquidComplexState(cfg);
            bp.state = s;
            bp.execute = &liquid_complex_execute;
            bp.clear = &liquid_complex_clear;
            bp.destroy = &liquid_complex_destroy;
            bp.workspace_bytes = cfg.batch * cfg.tap_count * sizeof(std::complex<float>);
            return bp;
        }
    }
    return Status::UnsupportedConfig;
}

}  // namespace fir
