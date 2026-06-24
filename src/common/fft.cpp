// Public FftPlan implementation: a thin, virtual-free owner of a backend
// dispatch record. The hot path (execute) lives in the header; this file only
// handles construction, teardown, and status strings.
#include "fft/fft.h"

#include "common/backend.h"

namespace fft {

const char* to_string(Status s) {
    switch (s) {
        case Status::Ok: return "Ok";
        case Status::NullPointer: return "NullPointer";
        case Status::InvalidArgument: return "InvalidArgument";
        case Status::UnsupportedLength: return "UnsupportedLength";
        case Status::UnsupportedConfig: return "UnsupportedConfig";
        case Status::BackendError: return "BackendError";
    }
    return "Unknown";
}

FftPlan::FftPlan(void* state, ExecuteFn execute, DestroyFn destroy,
                 std::size_t workspace_bytes, const char* backend_name)
    : state_(state, destroy),
      execute_(execute),
      workspace_bytes_(workspace_bytes),
      backend_name_(backend_name) {}

Result<FftPlan> FftPlan::create(const FftPlanConfig& config) {
    auto backend = make_backend_plan(config);
    if (!backend) return backend.status();
    const BackendPlan& bp = backend.value();
    return FftPlan(bp.state, bp.execute, bp.destroy, bp.workspace_bytes, bp.name);
}

}  // namespace fft
