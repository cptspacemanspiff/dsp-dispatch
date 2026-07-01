// dsp-dispatch: a normalized, backend-independent CPU FFT abstraction.
//
// This public header is intentionally free of any backend-specific layout,
// scaling, or packing rules. See docs/requirements.md for the fixed contract.
#ifndef DSP_DISPATCH_FFT_H
#define DSP_DISPATCH_FFT_H

#include <cstddef>
#include <memory>
#include <optional>
#include <utility>

namespace fft {

enum class FftDirection { Forward, Inverse };
enum class FftType { Complex, RealToComplex, ComplexToReal };
enum class FftPrecision { F32, F64 };

// Result/error status for plan creation and execution. There is exactly one
// success value (Ok); everything else describes why a call could not proceed.
enum class Status {
    Ok,
    NullPointer,        // input or output pointer was null
    InvalidArgument,    // e.g. length == 0, batch == 0, contradictory config
    UnsupportedLength,  // length not supported by the selected backend
    UnsupportedConfig,  // type/precision/direction combination not supported
    BackendError,       // backend reported a failure during commit/execute
};

const char* to_string(Status);

// Public, contract-stable plan configuration.
//
//   - Complex transforms use `direction` (Forward or Inverse).
//   - RealToComplex is always a forward transform; ComplexToReal is always an
//     inverse transform. `direction` is ignored for those types.
//   - `length` is the logical transform length N (not the packed bin count).
//   - `batch` independent transforms are laid out contiguously, back to back.
struct FftPlanConfig {
    FftType type = FftType::Complex;
    FftPrecision precision = FftPrecision::F32;
    std::size_t length = 0;
    std::size_t batch = 1;
    FftDirection direction = FftDirection::Forward;
};

// Lightweight success-or-status result. Move-only payloads are supported.
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

// A committed FFT plan. Creating a plan performs all allocation and backend
// setup; `execute` performs no allocation, locking, or planning.
//
// Dispatch is virtual-free: the plan holds a function pointer into the
// backend's monomorphized code and an opaque state pointer. `execute` is a
// single indirect call that can be hoisted out of a caller's loop.
//
// Threading contract: a single FftPlan instance is NOT safe for concurrent
// `execute` calls because it owns mutable scratch. For concurrent execution,
// create one plan per thread (cheap to create from the same config).
class FftPlan {
public:
    static Result<FftPlan> create(const FftPlanConfig& config);

    // input/output point to interleaved data per the contract in
    // docs/requirements.md. In-place (input == output) is permitted for
    // Complex transforms. Returns Status::Ok on success.
    Status execute(const void* input, void* output) const {
        if (input == nullptr || output == nullptr) return Status::NullPointer;
        return execute_(state_.get(), input, output);
    }

    // Size, in bytes, of the internal scratch this plan owns.
    std::size_t workspace_bytes() const { return workspace_bytes_; }

    // Stable identifier of the backend that produced this plan, e.g.
    // "portable", "vdsp", "mkl". Useful for diagnostics.
    const char* backend_name() const { return backend_name_; }

    FftPlan(FftPlan&&) noexcept = default;
    FftPlan& operator=(FftPlan&&) noexcept = default;
    ~FftPlan() = default;

    FftPlan(const FftPlan&) = delete;
    FftPlan& operator=(const FftPlan&) = delete;

private:
    using ExecuteFn = Status (*)(void* state, const void* input, void* output);
    using DestroyFn = void (*)(void* state);

    FftPlan(void* state, ExecuteFn execute, DestroyFn destroy,
            std::size_t workspace_bytes, const char* backend_name);

    std::unique_ptr<void, DestroyFn> state_;
    ExecuteFn execute_ = nullptr;
    std::size_t workspace_bytes_ = 0;
    const char* backend_name_ = "";
};

// Number of interleaved-complex output bins for a real-to-complex transform of
// length N: N/2 + 1. Provided as a convenience for buffer sizing.
inline std::size_t real_complex_bins(std::size_t length) { return length / 2 + 1; }

}  // namespace fft

#endif  // DSP_DISPATCH_FFT_H
