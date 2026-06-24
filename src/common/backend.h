// Internal backend interface. Not part of the public API.
//
// The shared interface is deliberately virtual-free. A backend produces a
// BackendPlan: an opaque state pointer plus plain function pointers. The public
// FftPlan calls `execute` through a function pointer (a single indirect call,
// hoistable out of any batch loop), and the backend's own code is monomorphized
// templates with no dispatch inside the transform.
//
// Exactly one backend is compiled into a given build, selected at configure
// time via FFT_BACKEND, and provides make_backend_plan().
#ifndef FFT_DISPATCH_COMMON_BACKEND_H
#define FFT_DISPATCH_COMMON_BACKEND_H

#include <cstddef>

#include "fft/fft.h"

namespace fft {

// Dispatch record returned by a backend factory. POD; copyable by value.
struct BackendPlan {
    void* state = nullptr;  // opaque, backend-owned; freed via destroy(state)
    Status (*execute)(void* state, const void* input, void* output) = nullptr;
    void (*destroy)(void* state) = nullptr;
    std::size_t workspace_bytes = 0;
    const char* name = "";
};

// Validates the configuration and returns a committed dispatch record, or a
// Status describing why the plan could not be built.
Result<BackendPlan> make_backend_plan(const FftPlanConfig& config);

}  // namespace fft

#endif  // FFT_DISPATCH_COMMON_BACKEND_H
