// Arm CMSIS-DSP backend (Phase 3). Scaffold only.
//
// When implemented, this must: enable NEON for AArch64 builds, satisfy
// CMSIS-DSP's temporary-buffer requirements, reject unsupported lengths during
// plan creation (CMSIS supports a restricted set of radix-2/4 lengths), and map
// its packed real output to the public N/2+1 layout.
#error "cmsis backend is not implemented yet. Build with -DFFT_BACKEND=portable."

#include "common/backend.h"

namespace fft {
Result<BackendPlan> make_backend_plan(const FftPlanConfig&) { return Status::UnsupportedConfig; }
}  // namespace fft
