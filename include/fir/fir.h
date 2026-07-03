// dsp-dispatch: normalized FIR filter abstraction.
//
// A FirPlan owns filter state and coefficients. Calls to execute() process a
// fixed-size block and continue from the plan's previous delay-line state.
#ifndef DSP_DISPATCH_FIR_H
#define DSP_DISPATCH_FIR_H

#include <cstddef>
#include <memory>
#include <optional>
#include <utility>

namespace fir {

enum class FirPrecision { F32, F64 };

// Real:            real samples, real taps, real output.
// ComplexRealTaps: complex samples, real taps, complex output.
// Complex:         complex samples, complex taps, complex output.
enum class FirType { Real, ComplexRealTaps, Complex };

enum class Status {
    Ok,
    NullPointer,
    InvalidArgument,
    UnsupportedConfig,
    BackendError,
};

const char* to_string(Status);

struct FirPlanConfig {
    FirType type = FirType::Real;
    FirPrecision precision = FirPrecision::F32;
    const void* taps = nullptr;
    std::size_t tap_count = 0;
    std::size_t block_size = 0;
    std::size_t batch = 1;
};

template <class T>
class Result {
public:
    Result(T value) : status_(Status::Ok), value_(std::move(value)) {}
    Result(Status s) : status_(s) {}

    bool ok() const { return status_ == Status::Ok; }
    explicit operator bool() const { return ok(); }
    Status status() const { return status_; }

    T& value() { return *value_; }
    const T& value() const { return *value_; }
    T release() { return std::move(*value_); }

private:
    Status status_;
    std::optional<T> value_;
};

class FirPlan {
public:
    static Result<FirPlan> create(const FirPlanConfig& config);

    Status execute(const void* input, void* output) const {
        if (input == nullptr || output == nullptr) return Status::NullPointer;
        return execute_(state_.get(), input, output);
    }

    Status clear() const { return clear_(state_.get()); }

    std::size_t workspace_bytes() const { return workspace_bytes_; }
    const char* backend_name() const { return backend_name_; }

    FirPlan(FirPlan&&) noexcept = default;
    FirPlan& operator=(FirPlan&&) noexcept = default;
    ~FirPlan() = default;

    FirPlan(const FirPlan&) = delete;
    FirPlan& operator=(const FirPlan&) = delete;

private:
    using ExecuteFn = Status (*)(void* state, const void* input, void* output);
    using ClearFn = Status (*)(void* state);
    using DestroyFn = void (*)(void* state);

    FirPlan(void* state, ExecuteFn execute, ClearFn clear, DestroyFn destroy,
            std::size_t workspace_bytes, const char* backend_name);

    std::unique_ptr<void, DestroyFn> state_;
    ExecuteFn execute_ = nullptr;
    ClearFn clear_ = nullptr;
    std::size_t workspace_bytes_ = 0;
    const char* backend_name_ = "";
};

}  // namespace fir

#endif  // DSP_DISPATCH_FIR_H
