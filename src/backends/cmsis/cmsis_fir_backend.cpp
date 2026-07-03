// Arm CMSIS-DSP FIR backend.
//
// Wraps CMSIS-DSP's arm_fir_f32 / arm_fir_f64 single-rate FIR. CMSIS-DSP is
// portable C with a scalar host kernel (x86_64) and a NEON kernel on AArch64,
// so this one source compiles for both. CMSIS-DSP has no complex FIR, so only
// FirType::Real (f32/f64) is supported; other configs report UnsupportedConfig.
//
// CMSIS conventions handled here:
//   - Coefficients are stored in TIME-REVERSED order {b[N-1], ..., b[0]}; the
//     public API hands us forward taps {b[0], ..., b[N-1]}, so we reverse them.
//   - Each filter instance needs a persistent state buffer of length
//     (numTaps + blockSize - 1). Reusing the instance across execute() calls is
//     what carries the delay line, giving the streaming semantics the API wants.
//   - numTaps is a uint16_t in CMSIS, so tap_count must fit in 16 bits.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "arm_math.h"

#include "common/fir_backend.h"
#include "fir/fir.h"

namespace fir {
namespace {

template <class T>
struct CmsisTraits;

template <>
struct CmsisTraits<float> {
    using Instance = arm_fir_instance_f32;
    static void init(Instance* s, uint16_t taps, const float* coeffs, float* state,
                     uint32_t block) {
        arm_fir_init_f32(s, taps, coeffs, state, block);
    }
    static void exec(const Instance* s, const float* src, float* dst, uint32_t block) {
        arm_fir_f32(s, src, dst, block);
    }
};

template <>
struct CmsisTraits<double> {
    using Instance = arm_fir_instance_f64;
    static void init(Instance* s, uint16_t taps, const double* coeffs, double* state,
                     uint32_t block) {
        arm_fir_init_f64(s, taps, coeffs, state, block);
    }
    static void exec(const Instance* s, const double* src, double* dst, uint32_t block) {
        arm_fir_f64(s, src, dst, block);
    }
};

template <class T>
struct CmsisFirState {
    using Instance = typename CmsisTraits<T>::Instance;

    std::size_t block_size = 0;
    std::size_t batch = 0;
    std::size_t tap_count = 0;
    std::vector<T> coeffs;            // reversed taps, shared by every instance
    std::vector<T> state;            // batch * state_len(), sliced per instance
    std::vector<Instance> inst;      // one instance per batch element

    std::size_t state_len() const { return tap_count + block_size - 1; }
};

template <class T>
Status cmsis_execute(void* state_ptr, const void* input, void* output) {
    auto& s = *static_cast<CmsisFirState<T>*>(state_ptr);
    const auto* in = static_cast<const T*>(input);
    auto* out = static_cast<T*>(output);
    const auto block = static_cast<uint32_t>(s.block_size);

    for (std::size_t b = 0; b < s.batch; ++b) {
        const std::size_t off = b * s.block_size;
        CmsisTraits<T>::exec(&s.inst[b], in + off, out + off, block);
    }
    return Status::Ok;
}

template <class T>
Status cmsis_clear(void* state_ptr) {
    auto& s = *static_cast<CmsisFirState<T>*>(state_ptr);
    // The instances hold pointers into s.state; zeroing the buffer resets the
    // delay line without invalidating those pointers.
    std::fill(s.state.begin(), s.state.end(), T{});
    return Status::Ok;
}

template <class T>
void cmsis_destroy(void* state_ptr) {
    delete static_cast<CmsisFirState<T>*>(state_ptr);
}

template <class T>
Result<BackendPlan> make_typed(const FirPlanConfig& cfg) {
    if (cfg.tap_count > static_cast<std::size_t>(std::numeric_limits<uint16_t>::max())) {
        return Status::UnsupportedConfig;
    }
    if (cfg.block_size > static_cast<std::size_t>(std::numeric_limits<uint32_t>::max())) {
        return Status::UnsupportedConfig;
    }

    auto* s = new CmsisFirState<T>();
    s->block_size = cfg.block_size;
    s->batch = cfg.batch;
    s->tap_count = cfg.tap_count;

    // Reverse forward taps {b[0..N-1]} into CMSIS's {b[N-1..0]} order.
    const auto* taps = static_cast<const T*>(cfg.taps);
    s->coeffs.resize(cfg.tap_count);
    for (std::size_t i = 0; i < cfg.tap_count; ++i) {
        s->coeffs[i] = taps[cfg.tap_count - 1 - i];
    }

    s->state.assign(s->batch * s->state_len(), T{});
    s->inst.resize(s->batch);
    for (std::size_t b = 0; b < s->batch; ++b) {
        CmsisTraits<T>::init(&s->inst[b], static_cast<uint16_t>(cfg.tap_count),
                             s->coeffs.data(), s->state.data() + b * s->state_len(),
                             static_cast<uint32_t>(cfg.block_size));
    }

    BackendPlan bp;
    bp.state = s;
    bp.execute = &cmsis_execute<T>;
    bp.clear = &cmsis_clear<T>;
    bp.destroy = &cmsis_destroy<T>;
    bp.workspace_bytes = (s->coeffs.size() + s->state.size()) * sizeof(T) +
                         s->inst.size() * sizeof(typename CmsisFirState<T>::Instance);
    bp.name = "cmsis";
    return bp;
}

bool invalid_common(const FirPlanConfig& cfg) {
    return cfg.taps == nullptr || cfg.tap_count == 0 || cfg.block_size == 0 || cfg.batch == 0;
}

}  // namespace

Result<BackendPlan> make_backend_plan(const FirPlanConfig& cfg) {
    if (invalid_common(cfg)) return Status::InvalidArgument;

    // CMSIS-DSP provides real single-rate FIR only (no complex kernels).
    if (cfg.type != FirType::Real) return Status::UnsupportedConfig;

    switch (cfg.precision) {
        case FirPrecision::F32: return make_typed<float>(cfg);
        case FirPrecision::F64: return make_typed<double>(cfg);
    }
    return Status::UnsupportedConfig;
}

}  // namespace fir
