// FFTW backend — BENCHMARK ONLY. Implements the internal BackendPlan over FFTW's
// planner so FFTW is measured through the exact same harness, buffers, and
// normalization as every other backend (apples-to-apples).
//
// FFTW is GPL-2.0-or-later / commercial (dual) and must never be linked into or
// packaged with a production artifact. This file lives under bench/ (never src/),
// the build forbids it when FFT_PACKAGING is ON, and the bench target has no
// install rule.
//
// Single precision (fftw3f), complex-to-complex only — that is what the standard
// benchmark exercises (c2c f32). F64 and real transforms return UnsupportedConfig
// until needed; supporting f64 would require also building FFTW's double library
// (a second, separately-named fetch), which the harness does not currently drive.
//
// Plans are out-of-place and created with FFTW_MEASURE (the fair "best plan" FFTW
// setting; its cost is captured by the separate plan_create benchmark) plus
// FFTW_UNALIGNED so the committed plan can run on the harness's own std::vector
// buffers via the new-array execute (fftwf_execute_dft), which the plan never saw
// at planning time.
#include <complex>
#include <cstddef>

#include <fftw3.h>

#include "common/backend.h"
#include "fft/fft.h"

namespace fft {
namespace {

struct FftwState {
    fftwf_plan plan = nullptr;
    std::size_t n = 0;
    std::size_t batch = 0;
    bool inverse = false;
    // Aligned scratch that FFTW_MEASURE overwrites while probing plans; the real
    // execute runs on the caller's buffers, so this is only planning scratch.
    fftwf_complex* in = nullptr;
    fftwf_complex* out = nullptr;

    ~FftwState() {
        if (plan) fftwf_destroy_plan(plan);
        if (in) fftwf_free(in);
        if (out) fftwf_free(out);
    }
};

Status fftw_execute(void* state_ptr, const void* input, void* output) {
    auto& s = *static_cast<FftwState*>(state_ptr);
    // fftwf_complex is float[2], layout-compatible with std::complex<float>.
    auto* in = reinterpret_cast<fftwf_complex*>(
        const_cast<std::complex<float>*>(static_cast<const std::complex<float>*>(input)));
    auto* out = reinterpret_cast<fftwf_complex*>(static_cast<std::complex<float>*>(output));

    // New-array execute: same plan, caller-provided buffers (FFTW_UNALIGNED made
    // the plan agnostic to their alignment).
    fftwf_execute_dft(s.plan, in, out);

    if (s.inverse) {  // public contract: inverse is 1/N scaled (FFTW is unscaled)
        const float inv_n = 1.0f / static_cast<float>(s.n);
        auto* o = static_cast<std::complex<float>*>(output);
        const std::size_t count = s.n * s.batch;
        for (std::size_t i = 0; i < count; ++i) o[i] *= inv_n;
    }
    return Status::Ok;
}

void fftw_destroy(void* state_ptr) { delete static_cast<FftwState*>(state_ptr); }

}  // namespace

Result<BackendPlan> make_backend_plan(const FftPlanConfig& cfg) {
    if (cfg.length == 0 || cfg.batch == 0) return Status::InvalidArgument;
    if (cfg.type != FftType::Complex) return Status::UnsupportedConfig;
    // This benchmark backend builds only FFTW's single-precision library (fftw3f).
    if (cfg.precision != FftPrecision::F32) return Status::UnsupportedConfig;

    const bool inverse = (cfg.direction == FftDirection::Inverse);
    const std::size_t n = cfg.length;
    const std::size_t batch = cfg.batch;
    const std::size_t count = n * batch;

    auto* st = new FftwState();
    st->n = n;
    st->batch = batch;
    st->inverse = inverse;
    st->in = static_cast<fftwf_complex*>(fftwf_malloc(sizeof(fftwf_complex) * count));
    st->out = static_cast<fftwf_complex*>(fftwf_malloc(sizeof(fftwf_complex) * count));
    if (st->in == nullptr || st->out == nullptr) {
        fftw_destroy(st);
        return Status::BackendError;
    }

    const int rank = 1;
    int dims[1] = {static_cast<int>(n)};
    const int sign = inverse ? FFTW_BACKWARD : FFTW_FORWARD;
    // in/out are contiguous, stride 1, distance N between consecutive transforms.
    st->plan = fftwf_plan_many_dft(rank, dims, static_cast<int>(batch),
                                   st->in, nullptr, 1, static_cast<int>(n),
                                   st->out, nullptr, 1, static_cast<int>(n),
                                   sign, FFTW_MEASURE | FFTW_UNALIGNED);
    if (st->plan == nullptr) {
        fftw_destroy(st);
        return Status::BackendError;
    }

    BackendPlan bp;
    bp.state = st;
    bp.execute = &fftw_execute;
    bp.destroy = &fftw_destroy;
    bp.workspace_bytes = sizeof(fftwf_complex) * count * 2;  // in + out scratch
    bp.name = "fftw";
    return bp;
}

}  // namespace fft
