#include "fir/fir.h"

#include "common/fir_backend.h"

namespace fir {

const char* to_string(Status s) {
    switch (s) {
        case Status::Ok: return "Ok";
        case Status::NullPointer: return "NullPointer";
        case Status::InvalidArgument: return "InvalidArgument";
        case Status::UnsupportedConfig: return "UnsupportedConfig";
        case Status::BackendError: return "BackendError";
    }
    return "Unknown";
}

FirPlan::FirPlan(void* state, ExecuteFn execute, ClearFn clear, DestroyFn destroy,
                 std::size_t workspace_bytes, const char* backend_name)
    : state_(state, destroy),
      execute_(execute),
      clear_(clear),
      workspace_bytes_(workspace_bytes),
      backend_name_(backend_name) {}

Result<FirPlan> FirPlan::create(const FirPlanConfig& config) {
    auto backend = make_backend_plan(config);
    if (!backend) return backend.status();
    const BackendPlan& bp = backend.value();
    return FirPlan(bp.state, bp.execute, bp.clear, bp.destroy, bp.workspace_bytes, bp.name);
}

}  // namespace fir
