#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#include "fir/fir.h"

namespace {

const std::size_t kBlockSizes[] = {64, 128, 256, 512, 1024, 2048, 4096, 8192};
const std::size_t kTapCounts[] = {8, 32, 128};

std::vector<float> make_taps(std::size_t tap_count) {
    std::vector<float> taps(tap_count);
    const float inv = 1.0f / static_cast<float>(tap_count);
    for (std::size_t i = 0; i < tap_count; ++i) taps[i] = inv;
    return taps;
}

void run_execute(benchmark::State& state, std::size_t block_size, std::size_t tap_count) {
    auto taps = make_taps(tap_count);
    fir::FirPlanConfig cfg;
    cfg.type = fir::FirType::Real;
    cfg.precision = fir::FirPrecision::F32;
    cfg.taps = taps.data();
    cfg.tap_count = taps.size();
    cfg.block_size = block_size;

    auto plan = fir::FirPlan::create(cfg);
    if (!plan.ok()) {
        state.SkipWithError("plan creation failed");
        return;
    }

    std::vector<float> in(block_size), out(block_size);
    for (std::size_t i = 0; i < block_size; ++i) in[i] = float(int(i % 17) - 8);

    for (int w = 0; w < 8; ++w) plan.value().execute(in.data(), out.data());
    plan.value().clear();

    for (auto _ : state) {
        plan.value().execute(in.data(), out.data());
        benchmark::DoNotOptimize(out.data());
        benchmark::ClobberMemory();
    }

    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(block_size));
    state.SetLabel(plan.value().backend_name());
}

void run_plan_create(benchmark::State& state, std::size_t block_size, std::size_t tap_count) {
    auto taps = make_taps(tap_count);
    fir::FirPlanConfig cfg;
    cfg.type = fir::FirType::Real;
    cfg.precision = fir::FirPrecision::F32;
    cfg.taps = taps.data();
    cfg.tap_count = taps.size();
    cfg.block_size = block_size;

    for (auto _ : state) {
        auto plan = fir::FirPlan::create(cfg);
        benchmark::DoNotOptimize(plan.ok());
    }
}

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
    for (std::size_t block_size : kBlockSizes) {
        for (std::size_t tap_count : kTapCounts) {
            const std::string suffix =
                std::to_string(block_size) + "/taps/" + std::to_string(tap_count);
            benchmark::RegisterBenchmark("execute/real/f32/block/" + suffix,
                                         [block_size, tap_count](benchmark::State& s) {
                                             run_execute(s, block_size, tap_count);
                                         })
                ->Unit(benchmark::kMicrosecond)
                ->Repetitions(15)
                ->DisplayAggregatesOnly(true)
                ->ComputeStatistics("p95", stat_p95)
                ->ComputeStatistics("p99", stat_p99);

            benchmark::RegisterBenchmark("plan_create/real/f32/block/" + suffix,
                                         [block_size, tap_count](benchmark::State& s) {
                                             run_plan_create(s, block_size, tap_count);
                                         })
                ->Unit(benchmark::kMicrosecond);
        }
    }

    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
