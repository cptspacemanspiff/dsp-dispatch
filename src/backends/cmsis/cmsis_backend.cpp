// Arm CMSIS-DSP FFT backend.
//
// Wraps CMSIS-DSP's power-of-two transforms: arm_cfft_f32 for complex and
// arm_rfft_fast_f32 for real. CMSIS-DSP is portable C with a scalar host kernel
// (x86_64) and a NEON kernel on AArch64, so this one source compiles for both.
//
// Supported lengths are the fixed radix tables CMSIS ships:
//   - complex: 16, 32, 64, ... 4096
//   - real:    32, 64, ... 4096
// We don't hardcode that table: arm_cfft_init_f32 / arm_rfft_fast_init_f32
// return ARM_MATH_ARGUMENT_ERROR for anything unsupported, which we surface as
// Status::UnsupportedLength. Only float32 is offered (UnsupportedConfig for f64).
//
// Contract mapping (see docs/requirements.md):
//   - Complex:        arm_cfft_f32 in place on 2N interleaved floats. Forward is
//                     unscaled and inverse is 1/N scaled by CMSIS, matching the
//                     contract with no extra scaling.
//   - RealToComplex:  arm_rfft_fast_f32 forward emits CMSIS's packed spectrum
//                     {DC.re, Nyquist.re, X1.re, X1.im, ...}; we expand it to the
//                     public N/2+1 interleaved complex bins.
//   - ComplexToReal:  we pack the public N/2+1 bins back into CMSIS's format
//                     (DC/Nyquist imaginary parts ignored) and run the inverse,
//                     which CMSIS normalizes by 1/N.
#include <algorithm>
#include <cstddef>
#include <vector>

#include "arm_math.h"

#include "common/backend.h"
#include "fft/fft.h"

namespace fft {
namespace {

struct CmsisFftState {
    FftType type = FftType::Complex;
    std::size_t n = 0;
    std::size_t batch = 0;
    std::size_t bins = 0;  // n/2 + 1
    bool inverse = false;  // complex direction only
    arm_cfft_instance_f32 cfft{};
    arm_rfft_fast_instance_f32 rfft{};
    std::vector<float> a;  // complex: 2N interleaved; real: N-float CMSIS buffer
    std::vector<float> b;  // real only: N-float CMSIS output buffer
};

Status cmsis_execute(void* state_ptr, const void* input, void* output) {
    auto& s = *static_cast<CmsisFftState*>(state_ptr);
    const std::size_t n = s.n;
    const std::size_t half = n / 2;

    switch (s.type) {
        case FftType::Complex: {
            const auto* in = static_cast<const float*>(input);
            auto* out = static_cast<float*>(output);
            for (std::size_t b = 0; b < s.batch; ++b) {
                const float* bin = in + b * 2 * n;
                float* bout = out + b * 2 * n;
                std::copy(bin, bin + 2 * n, s.a.data());
                arm_cfft_f32(&s.cfft, s.a.data(), s.inverse ? 1U : 0U, 1U);
                std::copy(s.a.data(), s.a.data() + 2 * n, bout);
            }
            return Status::Ok;
        }
        case FftType::RealToComplex: {
            const auto* in = static_cast<const float*>(input);
            auto* out = static_cast<float*>(output);  // 2*bins floats per batch
            for (std::size_t b = 0; b < s.batch; ++b) {
                const float* bin = in + b * n;
                float* bout = out + b * 2 * s.bins;
                std::copy(bin, bin + n, s.a.data());
                arm_rfft_fast_f32(&s.rfft, s.a.data(), s.b.data(), 0U);
                // CMSIS packed -> public N/2+1 interleaved bins.
                bout[0] = s.b[0];              // DC real
                bout[1] = 0.0f;               // DC imag
                bout[2 * half] = s.b[1];       // Nyquist real
                bout[2 * half + 1] = 0.0f;     // Nyquist imag
                for (std::size_t k = 1; k < half; ++k) {
                    bout[2 * k] = s.b[2 * k];
                    bout[2 * k + 1] = s.b[2 * k + 1];
                }
            }
            return Status::Ok;
        }
        case FftType::ComplexToReal: {
            const auto* in = static_cast<const float*>(input);  // 2*bins floats per batch
            auto* out = static_cast<float*>(output);
            for (std::size_t b = 0; b < s.batch; ++b) {
                const float* bin = in + b * 2 * s.bins;
                float* bout = out + b * n;
                // public bins -> CMSIS packed (DC/Nyquist imaginary parts ignored).
                s.a[0] = bin[0];              // DC real
                s.a[1] = bin[2 * half];        // Nyquist real
                for (std::size_t k = 1; k < half; ++k) {
                    s.a[2 * k] = bin[2 * k];
                    s.a[2 * k + 1] = bin[2 * k + 1];
                }
                arm_rfft_fast_f32(&s.rfft, s.a.data(), s.b.data(), 1U);
                std::copy(s.b.data(), s.b.data() + n, bout);
            }
            return Status::Ok;
        }
    }
    return Status::BackendError;
}

void cmsis_destroy(void* state_ptr) { delete static_cast<CmsisFftState*>(state_ptr); }

}  // namespace

Result<BackendPlan> make_backend_plan(const FftPlanConfig& cfg) {
    if (cfg.length == 0 || cfg.batch == 0) return Status::InvalidArgument;

    // CMSIS FFT is float32 only.
    if (cfg.precision != FftPrecision::F32) return Status::UnsupportedConfig;

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

    // CMSIS length fields are uint16_t; anything larger is unsupported anyway.
    if (cfg.length > 65535U) return Status::UnsupportedLength;
    const auto len = static_cast<uint16_t>(cfg.length);

    auto* s = new CmsisFftState();
    s->type = cfg.type;
    s->n = cfg.length;
    s->batch = cfg.batch;
    s->bins = cfg.length / 2 + 1;
    s->inverse = inverse;

    arm_status st;
    if (cfg.type == FftType::Complex) {
        st = arm_cfft_init_f32(&s->cfft, len);
        s->a.assign(2 * s->n, 0.0f);
    } else {
        st = arm_rfft_fast_init_f32(&s->rfft, len);
        s->a.assign(s->n, 0.0f);
        s->b.assign(s->n, 0.0f);
    }
    if (st != ARM_MATH_SUCCESS) {
        delete s;
        return Status::UnsupportedLength;
    }

    BackendPlan bp;
    bp.state = s;
    bp.execute = &cmsis_execute;
    bp.destroy = &cmsis_destroy;
    bp.workspace_bytes = (s->a.size() + s->b.size()) * sizeof(float);
    bp.name = "cmsis";
    return bp;
}

}  // namespace fft
