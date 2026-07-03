// Apple Accelerate/vDSP backend (Phase 3). Scaffold only.
//
// When implemented, this must: normalize vDSP's split-complex representation to
// the public interleaved layout, handle its packed real-FFT output (the DC and
// Nyquist real terms packed into one complex slot), apply vDSP's 2x real-FFT
// scaling, and reuse a single committed setup across execute calls.
#error "vdsp backend is not implemented yet. Build with -DFFT_BACKEND=pocketfft."

#include "common/backend.h"

namespace fft {
Result<BackendPlan> make_backend_plan(const FftPlanConfig&) { return Status::UnsupportedConfig; }
}  // namespace fft
