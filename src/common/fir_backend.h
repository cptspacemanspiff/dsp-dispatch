// Internal FIR backend interface. Not part of the public API.
#ifndef DSP_DISPATCH_COMMON_FIR_BACKEND_H
#define DSP_DISPATCH_COMMON_FIR_BACKEND_H

#include <cstddef>

#include "fir/fir.h"

namespace fir {

struct BackendPlan {
    void* state = nullptr;
    Status (*execute)(void* state, const void* input, void* output) = nullptr;
    Status (*clear)(void* state) = nullptr;
    void (*destroy)(void* state) = nullptr;
    std::size_t workspace_bytes = 0;
    const char* name = "";
};

Result<BackendPlan> make_backend_plan(const FirPlanConfig& config);

}  // namespace fir

#endif  // DSP_DISPATCH_COMMON_FIR_BACKEND_H
