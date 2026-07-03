// Correctness tests (Phase 5). No external framework: each check prints on
// failure and the process exits non-zero so CTest reports the failure.
//
// Backends are compared against a naive reference DFT and against the
// forward/inverse round-trip identity, after normalizing to the public
// convention (forward unscaled, inverse 1/N).
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "fft/fft.h"

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL: %s  (%s:%d)\n", (msg), __FILE__, __LINE__); \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

using cf = std::complex<float>;
using cd = std::complex<double>;

// Reference unscaled DFT in double precision. inverse => +sign, no scaling.
std::vector<cd> ref_dft(const std::vector<cd>& x, bool inverse) {
    const std::size_t n = x.size();
    const double pi = 3.14159265358979323846;
    std::vector<cd> out(n);
    for (std::size_t k = 0; k < n; ++k) {
        cd acc(0, 0);
        for (std::size_t j = 0; j < n; ++j) {
            double ang = (inverse ? 2.0 : -2.0) * pi * double(j) * double(k) / double(n);
            acc += x[j] * cd(std::cos(ang), std::sin(ang));
        }
        out[k] = acc;
    }
    return out;
}

double max_abs_err(const std::vector<cd>& a, const std::vector<cf>& b) {
    double e = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        e = std::max(e, std::abs(a[i] - cd(b[i].real(), b[i].imag())));
    }
    return e;
}

// Relative tolerance scaled by N (DFT magnitudes grow with N) and precision.
double tol_for(std::size_t n) { return 1e-4 * double(n); }

void test_complex_vs_reference(std::size_t n, fft::FftDirection dir) {
    std::mt19937 rng(12345 + unsigned(n) + (dir == fft::FftDirection::Inverse ? 7 : 0));
    std::uniform_real_distribution<float> dist(-1.f, 1.f);

    std::vector<cf> in(n);
    std::vector<cd> ref_in(n);
    for (std::size_t i = 0; i < n; ++i) {
        in[i] = cf(dist(rng), dist(rng));
        ref_in[i] = cd(in[i].real(), in[i].imag());
    }

    fft::FftPlanConfig cfg;
    cfg.type = fft::FftType::Complex;
    cfg.precision = fft::FftPrecision::F32;
    cfg.length = n;
    cfg.direction = dir;

    auto plan = fft::FftPlan::create(cfg);
    // A backend may only support a fixed set of lengths (e.g. CMSIS-DSP does
    // power-of-two 16..4096). Treat an unsupported length as a skip.
    if (plan.status() == fft::Status::UnsupportedLength) return;
    CHECK(plan.ok(), "complex plan create");
    if (!plan.ok()) return;

    std::vector<cf> out(n);
    auto st = plan.value().execute(in.data(), out.data());
    CHECK(st == fft::Status::Ok, "complex execute");

    auto ref = ref_dft(ref_in, dir == fft::FftDirection::Inverse);
    if (dir == fft::FftDirection::Inverse) {
        for (auto& v : ref) v /= double(n);  // public inverse is 1/N scaled
    }
    double err = max_abs_err(ref, out);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "complex n=%zu dir=%d err=%.3g", n, int(dir), err);
    CHECK(err < tol_for(n), buf);
}

void test_roundtrip(std::size_t n) {
    std::mt19937 rng(999 + unsigned(n));
    std::uniform_real_distribution<float> dist(-1.f, 1.f);
    std::vector<cf> in(n), freq(n), back(n);
    for (auto& v : in) v = cf(dist(rng), dist(rng));

    fft::FftPlanConfig fwd;
    fwd.type = fft::FftType::Complex;
    fwd.length = n;
    fwd.direction = fft::FftDirection::Forward;
    fft::FftPlanConfig inv = fwd;
    inv.direction = fft::FftDirection::Inverse;

    auto pf = fft::FftPlan::create(fwd);
    auto pi = fft::FftPlan::create(inv);
    if (pf.status() == fft::Status::UnsupportedLength ||
        pi.status() == fft::Status::UnsupportedLength) {
        return;
    }
    CHECK(pf.ok() && pi.ok(), "roundtrip plans");
    if (!pf.ok() || !pi.ok()) return;

    pf.value().execute(in.data(), freq.data());
    pi.value().execute(freq.data(), back.data());

    double e = 0;
    for (std::size_t i = 0; i < n; ++i) e = std::max(e, double(std::abs(in[i] - back[i])));
    char buf[96];
    std::snprintf(buf, sizeof(buf), "roundtrip n=%zu err=%.3g", n, e);
    CHECK(e < 1e-4, buf);
}

void test_real_roundtrip(std::size_t n) {
    std::mt19937 rng(555 + unsigned(n));
    std::uniform_real_distribution<float> dist(-1.f, 1.f);
    const std::size_t bins = n / 2 + 1;
    std::vector<float> in(n), back(n);
    std::vector<cf> spec(bins);
    for (auto& v : in) v = dist(rng);

    fft::FftPlanConfig r2c;
    r2c.type = fft::FftType::RealToComplex;
    r2c.length = n;
    fft::FftPlanConfig c2r;
    c2r.type = fft::FftType::ComplexToReal;
    c2r.length = n;

    auto pr = fft::FftPlan::create(r2c);
    auto pc = fft::FftPlan::create(c2r);
    // A backend may not support real transforms at every length (e.g. AOCL-FFTZ
    // excludes large primes). Treat that as a skip, not a failure.
    if (pr.status() == fft::Status::UnsupportedLength ||
        pc.status() == fft::Status::UnsupportedLength) {
        return;
    }
    CHECK(pr.ok() && pc.ok(), "real plans");
    if (!pr.ok() || !pc.ok()) return;

    pr.value().execute(in.data(), spec.data());

    // Verify the forward real spectrum against the reference DFT bins.
    std::vector<cd> ref_in(n);
    for (std::size_t i = 0; i < n; ++i) ref_in[i] = cd(in[i], 0);
    auto ref = ref_dft(ref_in, false);
    double spec_err = 0;
    for (std::size_t i = 0; i < bins; ++i)
        spec_err = std::max(spec_err, std::abs(ref[i] - cd(spec[i].real(), spec[i].imag())));
    char b1[96];
    std::snprintf(b1, sizeof(b1), "r2c spectrum n=%zu err=%.3g", n, spec_err);
    CHECK(spec_err < tol_for(n), b1);

    pc.value().execute(spec.data(), back.data());
    double e = 0;
    for (std::size_t i = 0; i < n; ++i) e = std::max(e, double(std::fabs(in[i] - back[i])));
    char b2[96];
    std::snprintf(b2, sizeof(b2), "real roundtrip n=%zu err=%.3g", n, e);
    CHECK(e < 1e-4, b2);
}

void test_impulse(std::size_t n) {
    // FFT of a unit impulse at index 0 is all ones.
    std::vector<cf> in(n, cf(0, 0)), out(n);
    in[0] = cf(1, 0);
    fft::FftPlanConfig cfg;
    cfg.length = n;
    auto plan = fft::FftPlan::create(cfg);
    if (plan.status() == fft::Status::UnsupportedLength) return;
    CHECK(plan.ok(), "impulse plan");
    if (!plan.ok()) return;
    plan.value().execute(in.data(), out.data());
    double e = 0;
    for (auto& v : out) e = std::max(e, double(std::abs(v - cf(1, 0))));
    char buf[96];
    std::snprintf(buf, sizeof(buf), "impulse n=%zu err=%.3g", n, e);
    CHECK(e < 1e-4, buf);
}

void test_error_handling() {
    fft::FftPlanConfig cfg;
    cfg.length = 0;
    CHECK(!fft::FftPlan::create(cfg).ok(), "zero length rejected");

    cfg.length = 16;
    cfg.batch = 0;
    CHECK(!fft::FftPlan::create(cfg).ok(), "zero batch rejected");

    cfg.batch = 1;
    cfg.type = fft::FftType::RealToComplex;
    cfg.direction = fft::FftDirection::Inverse;
    CHECK(!fft::FftPlan::create(cfg).ok(), "inverse r2c rejected");

    cfg.type = fft::FftType::Complex;
    cfg.direction = fft::FftDirection::Forward;
    auto plan = fft::FftPlan::create(cfg);
    CHECK(plan.ok(), "valid plan");
    std::vector<cf> buf(16);
    CHECK(plan.value().execute(nullptr, buf.data()) == fft::Status::NullPointer,
          "null input rejected");
}

void test_batch(std::size_t n, std::size_t batch) {
    std::mt19937 rng(31 + unsigned(n) * 7 + unsigned(batch));
    std::uniform_real_distribution<float> dist(-1.f, 1.f);
    std::vector<cf> in(n * batch), out(n * batch);
    for (auto& v : in) v = cf(dist(rng), dist(rng));

    fft::FftPlanConfig cfg;
    cfg.length = n;
    cfg.batch = batch;
    auto plan = fft::FftPlan::create(cfg);
    if (plan.status() == fft::Status::UnsupportedLength) return;
    CHECK(plan.ok(), "batch plan");
    if (!plan.ok()) return;
    plan.value().execute(in.data(), out.data());

    for (std::size_t b = 0; b < batch; ++b) {
        std::vector<cd> ref_in(n);
        for (std::size_t i = 0; i < n; ++i)
            ref_in[i] = cd(in[b * n + i].real(), in[b * n + i].imag());
        auto ref = ref_dft(ref_in, false);
        std::vector<cf> slice(out.begin() + b * n, out.begin() + (b + 1) * n);
        double e = max_abs_err(ref, slice);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "batch n=%zu b=%zu err=%.3g", n, b, e);
        CHECK(e < tol_for(n), buf);
    }
}

}  // namespace

int main() {
    // Power-of-two and non-power-of-two lengths (Bluestein path).
    const std::size_t lens[] = {1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 17, 31, 32, 64, 100, 256, 1000};

    for (std::size_t n : lens) {
        test_complex_vs_reference(n, fft::FftDirection::Forward);
        test_complex_vs_reference(n, fft::FftDirection::Inverse);
        test_roundtrip(n);
        test_impulse(n);
        if (n >= 2) test_real_roundtrip(n);
    }
    test_batch(16, 4);
    test_batch(17, 3);
    test_error_handling();

    if (g_failures == 0) {
        std::printf("All FFT correctness tests passed.\n");
        return 0;
    }
    std::printf("%d check(s) failed.\n", g_failures);
    return 1;
}
