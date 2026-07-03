#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <vector>

#include "fir/fir.h"

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL: %s  (%s:%d)\n", (msg), __FILE__, __LINE__); \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

template <class Sample, class Tap>
std::vector<Sample> ref_fir(const std::vector<Sample>& input, const std::vector<Tap>& taps) {
    std::vector<Sample> out(input.size(), Sample{});
    for (std::size_t i = 0; i < input.size(); ++i) {
        Sample acc{};
        for (std::size_t k = 0; k < taps.size(); ++k) {
            if (i >= k) acc += input[i - k] * taps[k];
        }
        out[i] = acc;
    }
    return out;
}

void test_real_f32() {
    const std::size_t n = 32;
    const std::vector<float> taps = {0.25f, 0.5f, 0.25f};
    std::vector<float> in(n), out(n);
    for (std::size_t i = 0; i < n; ++i) in[i] = float(int(i % 9) - 4);

    fir::FirPlanConfig cfg;
    cfg.type = fir::FirType::Real;
    cfg.precision = fir::FirPrecision::F32;
    cfg.taps = taps.data();
    cfg.tap_count = taps.size();
    cfg.block_size = n;

    auto plan = fir::FirPlan::create(cfg);
    CHECK(plan.ok(), "real f32 plan");
    if (!plan.ok()) return;
    CHECK(plan.value().execute(in.data(), out.data()) == fir::Status::Ok, "real f32 execute");

    auto ref = ref_fir(in, taps);
    double e = 0;
    for (std::size_t i = 0; i < n; ++i) e = std::max(e, double(std::fabs(out[i] - ref[i])));
    CHECK(e < 1e-6, "real f32 output");
}

void test_real_f64() {
    const std::size_t n = 17;
    const std::vector<double> taps = {0.125, -0.25, 0.5, -0.25, 0.125};
    std::vector<double> in(n), out(n);
    for (std::size_t i = 0; i < n; ++i) in[i] = double(i) / 7.0;

    fir::FirPlanConfig cfg;
    cfg.type = fir::FirType::Real;
    cfg.precision = fir::FirPrecision::F64;
    cfg.taps = taps.data();
    cfg.tap_count = taps.size();
    cfg.block_size = n;

    auto plan = fir::FirPlan::create(cfg);
    if (plan.status() == fir::Status::UnsupportedConfig) return;
    CHECK(plan.ok(), "real f64 plan");
    if (!plan.ok()) return;
    CHECK(plan.value().execute(in.data(), out.data()) == fir::Status::Ok, "real f64 execute");

    auto ref = ref_fir(in, taps);
    double e = 0;
    for (std::size_t i = 0; i < n; ++i) e = std::max(e, std::fabs(out[i] - ref[i]));
    CHECK(e < 1e-12, "real f64 output");
}

void test_complex_real_taps() {
    using cf = std::complex<float>;
    const std::size_t n = 24;
    const std::vector<float> taps = {0.5f, 0.25f, -0.125f};
    std::vector<cf> in(n), out(n);
    for (std::size_t i = 0; i < n; ++i) in[i] = cf(float(i % 5), -float(i % 7));

    fir::FirPlanConfig cfg;
    cfg.type = fir::FirType::ComplexRealTaps;
    cfg.precision = fir::FirPrecision::F32;
    cfg.taps = taps.data();
    cfg.tap_count = taps.size();
    cfg.block_size = n;

    auto plan = fir::FirPlan::create(cfg);
    // Some backends (e.g. CMSIS-DSP) provide real FIR only.
    if (plan.status() == fir::Status::UnsupportedConfig) return;
    CHECK(plan.ok(), "complex real taps plan");
    if (!plan.ok()) return;
    plan.value().execute(in.data(), out.data());

    auto ref = ref_fir(in, taps);
    double e = 0;
    for (std::size_t i = 0; i < n; ++i) e = std::max(e, double(std::abs(out[i] - ref[i])));
    CHECK(e < 1e-6, "complex real taps output");
}

void test_complex_taps() {
    using cf = std::complex<float>;
    const std::size_t n = 19;
    const std::vector<cf> taps = {cf(0.5f, 0.0f), cf(0.0f, 0.25f), cf(-0.125f, 0.125f)};
    std::vector<cf> in(n), out(n);
    for (std::size_t i = 0; i < n; ++i) in[i] = cf(float(i % 3), float(i % 4) - 2.0f);

    fir::FirPlanConfig cfg;
    cfg.type = fir::FirType::Complex;
    cfg.precision = fir::FirPrecision::F32;
    cfg.taps = taps.data();
    cfg.tap_count = taps.size();
    cfg.block_size = n;

    auto plan = fir::FirPlan::create(cfg);
    // Some backends (e.g. CMSIS-DSP) provide real FIR only.
    if (plan.status() == fir::Status::UnsupportedConfig) return;
    CHECK(plan.ok(), "complex taps plan");
    if (!plan.ok()) return;
    plan.value().execute(in.data(), out.data());

    auto ref = ref_fir(in, taps);
    double e = 0;
    for (std::size_t i = 0; i < n; ++i) e = std::max(e, double(std::abs(out[i] - ref[i])));
    CHECK(e < 1e-6, "complex taps output");
}

void test_streaming_and_clear() {
    const std::vector<float> taps = {1.0f, -0.5f, 0.25f};
    const std::vector<float> in = {1, 2, 3, 4, 5, 6, 7, 8};
    std::vector<float> out_a(4), out_b(4), once_out(8), combined(8);

    fir::FirPlanConfig cfg;
    cfg.taps = taps.data();
    cfg.tap_count = taps.size();
    cfg.block_size = 4;

    auto plan = fir::FirPlan::create(cfg);
    CHECK(plan.ok(), "streaming plan");
    if (!plan.ok()) return;
    plan.value().execute(in.data(), out_a.data());
    plan.value().execute(in.data() + 4, out_b.data());
    for (std::size_t i = 0; i < 4; ++i) {
        combined[i] = out_a[i];
        combined[4 + i] = out_b[i];
    }

    cfg.block_size = 8;
    auto once = fir::FirPlan::create(cfg);
    CHECK(once.ok(), "single block plan");
    if (!once.ok()) return;
    once.value().execute(in.data(), once_out.data());

    double e = 0;
    for (std::size_t i = 0; i < 8; ++i) e = std::max(e, double(std::fabs(combined[i] - once_out[i])));
    CHECK(e < 1e-6, "streaming equals single block");

    CHECK(plan.value().clear() == fir::Status::Ok, "clear");
    plan.value().execute(in.data(), out_a.data());
    e = 0;
    for (std::size_t i = 0; i < 4; ++i) e = std::max(e, double(std::fabs(out_a[i] - once_out[i])));
    CHECK(e < 1e-6, "clear resets state");
}

void test_batch() {
    const std::size_t n = 16;
    const std::size_t batch = 3;
    const std::vector<float> taps = {0.2f, 0.3f, 0.5f};
    std::vector<float> in(n * batch), out(n * batch);
    for (std::size_t i = 0; i < in.size(); ++i) in[i] = float(int(i % 11) - 5);

    fir::FirPlanConfig cfg;
    cfg.taps = taps.data();
    cfg.tap_count = taps.size();
    cfg.block_size = n;
    cfg.batch = batch;

    auto plan = fir::FirPlan::create(cfg);
    CHECK(plan.ok(), "batch plan");
    if (!plan.ok()) return;
    plan.value().execute(in.data(), out.data());

    for (std::size_t b = 0; b < batch; ++b) {
        std::vector<float> slice(in.begin() + b * n, in.begin() + (b + 1) * n);
        auto ref = ref_fir(slice, taps);
        double e = 0;
        for (std::size_t i = 0; i < n; ++i)
            e = std::max(e, double(std::fabs(out[b * n + i] - ref[i])));
        CHECK(e < 1e-6, "batch output");
    }
}

void test_error_handling() {
    fir::FirPlanConfig cfg;
    std::vector<float> taps = {1.0f};
    cfg.taps = taps.data();
    cfg.tap_count = taps.size();
    cfg.block_size = 8;

    auto plan = fir::FirPlan::create(cfg);
    CHECK(plan.ok(), "valid plan");
    std::vector<float> buf(8);
    CHECK(plan.value().execute(nullptr, buf.data()) == fir::Status::NullPointer,
          "null input rejected");
    CHECK(plan.value().execute(buf.data(), nullptr) == fir::Status::NullPointer,
          "null output rejected");

    cfg.taps = nullptr;
    CHECK(!fir::FirPlan::create(cfg).ok(), "null taps rejected");
    cfg.taps = taps.data();
    cfg.tap_count = 0;
    CHECK(!fir::FirPlan::create(cfg).ok(), "zero taps rejected");
    cfg.tap_count = taps.size();
    cfg.block_size = 0;
    CHECK(!fir::FirPlan::create(cfg).ok(), "zero block rejected");
}

}  // namespace

int main() {
    test_real_f32();
    test_real_f64();
    test_complex_real_taps();
    test_complex_taps();
    test_streaming_and_clear();
    test_batch();
    test_error_handling();

    if (g_failures == 0) {
        std::printf("All FIR correctness tests passed.\n");
        return 0;
    }
    std::printf("%d FIR check(s) failed.\n", g_failures);
    return 1;
}
