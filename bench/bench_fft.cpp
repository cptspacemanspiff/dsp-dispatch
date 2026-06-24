// Benchmark harness (Phase 6), built on Google Benchmark.
//
// Plan creation is measured separately from execution. Execution benchmarks
// reuse one committed plan and preallocated buffers, warm up the backend, and
// report median plus custom p95/p99 statistics (over repetitions) and a
// transforms/sec rate. Google Benchmark records the CPU and environment.
//
// Single-threaded by design; backend internal threading must be benchmarked
// separately. Pin the process (taskset/numactl) and fix the clock policy for
// stable numbers.
#include <algorithm>
#include <complex>
#include <cstddef>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#include "fft/fft.h"

// Backend-agnostic harness: it benchmarks whatever backend is linked into this
// executable through the public API, and self-identifies via backend_name().
// One executable is built per backend (fft_bench_portable, fft_bench_kfr, ...).
namespace {

// Standard length matrix (Phase 6). Extend with real production lengths and any
// required non-power-of-two lengths.
const std::size_t kLengths[] = {64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384};

void run_execute(benchmark::State& state, std::size_t n) {
    fft::FftPlanConfig cfg;
    cfg.type = fft::FftType::Complex;
    cfg.precision = fft::FftPrecision::F32;
    cfg.length = n;

    auto plan = fft::FftPlan::create(cfg);
    if (!plan.ok()) {
        state.SkipWithError("plan creation failed");
        return;
    }

    std::vector<std::complex<float>> in(n), out(n);
    for (std::size_t i = 0; i < n; ++i) in[i] = {float(i % 7) - 3.f, float(i % 5) - 2.f};

    for (int w = 0; w < 8; ++w) plan.value().execute(in.data(), out.data());  // warm up

    for (auto _ : state) {
        plan.value().execute(in.data(), out.data());
        benchmark::DoNotOptimize(out.data());
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(state.iterations());  // -> items_per_second == transforms/s
    state.SetLabel(plan.value().backend_name());
}

void run_plan_create(benchmark::State& state, std::size_t n) {
    fft::FftPlanConfig cfg;
    cfg.type = fft::FftType::Complex;
    cfg.precision = fft::FftPrecision::F32;
    cfg.length = n;
    for (auto _ : state) {
        auto plan = fft::FftPlan::create(cfg);
        benchmark::DoNotOptimize(plan.ok());
    }
}

// Percentiles over per-repetition aggregated times (requires repetitions > 1).
// ComputeStatistics needs a plain function pointer, so these are free functions.
double percentile(const std::vector<double>& v, double p) {
    if (v.empty()) return 0.0;
    std::vector<double> s(v);
    std::sort(s.begin(), s.end());
    return s[static_cast<std::size_t>(p * (s.size() - 1))];
}
double stat_p95(const std::vector<double>& v) { return percentile(v, 0.95); }
double stat_p99(const std::vector<double>& v) { return percentile(v, 0.99); }

}  // namespace

int main(int argc, char** argv) {
    for (std::size_t n : kLengths) {
        benchmark::RegisterBenchmark("execute/c2c/f32/" + std::to_string(n),
                                     [n](benchmark::State& s) { run_execute(s, n); })
            ->Unit(benchmark::kMicrosecond)
            ->Repetitions(15)
            ->DisplayAggregatesOnly(true)
            ->ComputeStatistics("p95", stat_p95)
            ->ComputeStatistics("p99", stat_p99);

        benchmark::RegisterBenchmark("plan_create/c2c/f32/" + std::to_string(n),
                                     [n](benchmark::State& s) { run_plan_create(s, n); })
            ->Unit(benchmark::kMicrosecond);
    }

    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
