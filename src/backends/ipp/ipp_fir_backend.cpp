#include <algorithm>
#include <cstddef>
#include <limits>
#include <vector>

#include <ipp.h>

#include "common/fir_backend.h"
#include "fir/fir.h"

namespace fir {
namespace {

template <class T>
using ExecFn = IppStatus (*)(const T*, T*, int, void*, const T*, T*, Ipp8u*);

struct Ipp32fcLayout {
    float re;
    float im;
};

struct Ipp64fcLayout {
    double re;
    double im;
};

static_assert(sizeof(Ipp32fc) == sizeof(Ipp32fcLayout), "unexpected Ipp32fc layout");
static_assert(sizeof(Ipp64fc) == sizeof(Ipp64fcLayout), "unexpected Ipp64fc layout");

template <class Sample, class Tap>
struct IppFirState {
    std::size_t block_size = 0;
    std::size_t batch = 0;
    std::size_t tap_count = 0;
    std::vector<Ipp8u> spec;
    std::vector<Ipp8u> work;
    std::vector<Sample> delay_a;
    std::vector<Sample> delay_b;
};

bool invalid_common(const FirPlanConfig& cfg) {
    return cfg.taps == nullptr || cfg.tap_count == 0 || cfg.block_size == 0 || cfg.batch == 0;
}

bool exceeds_int(std::size_t v) {
    return v > static_cast<std::size_t>(std::numeric_limits<int>::max());
}

template <class Sample, class Tap>
ExecFn<Sample> ipp_exec();

Status map_status(IppStatus s) {
    if (s == ippStsNoErr) return Status::Ok;
    if (s == ippStsNullPtrErr) return Status::NullPointer;
    if (s == ippStsFIRLenErr || s == ippStsSizeErr) return Status::InvalidArgument;
    if (s == ippStsAlgTypeErr) return Status::UnsupportedConfig;
    return Status::BackendError;
}

template <class Sample, class Tap>
Sample* ptr_or_null(std::vector<Sample>& v, std::size_t offset) {
    return v.empty() ? nullptr : v.data() + offset;
}

template <class Sample, class Tap>
Status ipp_execute(void* state_ptr, const void* input, void* output) {
    auto& s = *static_cast<IppFirState<Sample, Tap>*>(state_ptr);
    const auto* in = static_cast<const Sample*>(input);
    auto* out = static_cast<Sample*>(output);
    const auto n = static_cast<int>(s.block_size);
    const std::size_t delay_len = s.tap_count - 1;
    auto* spec = static_cast<void*>(s.spec.data());
    auto* work = s.work.empty() ? nullptr : s.work.data();

    for (std::size_t b = 0; b < s.batch; ++b) {
        const std::size_t sample_off = b * s.block_size;
        const std::size_t delay_off = b * delay_len;
        IppStatus st = ipp_exec<Sample, Tap>()(
            in + sample_off, out + sample_off, n, spec,
            ptr_or_null<Sample, Tap>(s.delay_a, delay_off),
            ptr_or_null<Sample, Tap>(s.delay_b, delay_off), work);
        Status mapped = map_status(st);
        if (mapped != Status::Ok) return mapped;
    }

    s.delay_a.swap(s.delay_b);
    return Status::Ok;
}

template <class Sample, class Tap>
Status ipp_clear(void* state_ptr) {
    auto& s = *static_cast<IppFirState<Sample, Tap>*>(state_ptr);
    std::fill(s.delay_a.begin(), s.delay_a.end(), Sample{});
    std::fill(s.delay_b.begin(), s.delay_b.end(), Sample{});
    return Status::Ok;
}

template <class Sample, class Tap>
void ipp_destroy(void* state_ptr) {
    delete static_cast<IppFirState<Sample, Tap>*>(state_ptr);
}

template <class Sample, class Tap>
IppStatus get_size(int taps_len, int* spec_size, int* work_size);

template <>
IppStatus get_size<Ipp32f, Ipp32f>(int taps_len, int* spec_size, int* work_size) {
    return ippsFIRSRGetSize(taps_len, ipp32f, spec_size, work_size);
}

template <>
IppStatus get_size<Ipp64f, Ipp64f>(int taps_len, int* spec_size, int* work_size) {
    return ippsFIRSRGetSize(taps_len, ipp64f, spec_size, work_size);
}

template <>
IppStatus get_size<Ipp32fc, Ipp32fc>(int taps_len, int* spec_size, int* work_size) {
    return ippsFIRSRGetSize(taps_len, ipp32fc, spec_size, work_size);
}

template <>
IppStatus get_size<Ipp64fc, Ipp64fc>(int taps_len, int* spec_size, int* work_size) {
    return ippsFIRSRGetSize(taps_len, ipp64fc, spec_size, work_size);
}

template <>
IppStatus get_size<Ipp32fc, Ipp32f>(int taps_len, int* spec_size, int* work_size) {
    return ippsFIRSRGetSize32f_32fc(taps_len, spec_size, work_size);
}

template <class Sample, class Tap>
IppStatus ipp_init(const Tap* taps, int taps_len, void* spec);

template <>
IppStatus ipp_init<Ipp32f, Ipp32f>(const Ipp32f* taps, int taps_len, void* spec) {
    return ippsFIRSRInit_32f(taps, taps_len, ippAlgAuto, static_cast<IppsFIRSpec_32f*>(spec));
}

template <>
IppStatus ipp_init<Ipp64f, Ipp64f>(const Ipp64f* taps, int taps_len, void* spec) {
    return ippsFIRSRInit_64f(taps, taps_len, ippAlgAuto, static_cast<IppsFIRSpec_64f*>(spec));
}

template <>
IppStatus ipp_init<Ipp32fc, Ipp32fc>(const Ipp32fc* taps, int taps_len, void* spec) {
    return ippsFIRSRInit_32fc(taps, taps_len, ippAlgAuto, static_cast<IppsFIRSpec_32fc*>(spec));
}

template <>
IppStatus ipp_init<Ipp64fc, Ipp64fc>(const Ipp64fc* taps, int taps_len, void* spec) {
    return ippsFIRSRInit_64fc(taps, taps_len, ippAlgAuto, static_cast<IppsFIRSpec_64fc*>(spec));
}

template <>
IppStatus ipp_init<Ipp32fc, Ipp32f>(const Ipp32f* taps, int taps_len, void* spec) {
    return ippsFIRSRInit32f_32fc(taps, taps_len, ippAlgAuto,
                                 static_cast<IppsFIRSpec32f_32fc*>(spec));
}

template <>
ExecFn<Ipp32f> ipp_exec<Ipp32f, Ipp32f>() {
    return [](const Ipp32f* src, Ipp32f* dst, int n, void* spec, const Ipp32f* dly_src,
              Ipp32f* dly_dst, Ipp8u* buf) {
        return ippsFIRSR_32f(src, dst, n, static_cast<IppsFIRSpec_32f*>(spec), dly_src,
                             dly_dst, buf);
    };
}

template <>
ExecFn<Ipp64f> ipp_exec<Ipp64f, Ipp64f>() {
    return [](const Ipp64f* src, Ipp64f* dst, int n, void* spec, const Ipp64f* dly_src,
              Ipp64f* dly_dst, Ipp8u* buf) {
        return ippsFIRSR_64f(src, dst, n, static_cast<IppsFIRSpec_64f*>(spec), dly_src,
                             dly_dst, buf);
    };
}

template <>
ExecFn<Ipp32fc> ipp_exec<Ipp32fc, Ipp32fc>() {
    return [](const Ipp32fc* src, Ipp32fc* dst, int n, void* spec, const Ipp32fc* dly_src,
              Ipp32fc* dly_dst, Ipp8u* buf) {
        return ippsFIRSR_32fc(src, dst, n, static_cast<IppsFIRSpec_32fc*>(spec), dly_src,
                              dly_dst, buf);
    };
}

template <>
ExecFn<Ipp64fc> ipp_exec<Ipp64fc, Ipp64fc>() {
    return [](const Ipp64fc* src, Ipp64fc* dst, int n, void* spec, const Ipp64fc* dly_src,
              Ipp64fc* dly_dst, Ipp8u* buf) {
        return ippsFIRSR_64fc(src, dst, n, static_cast<IppsFIRSpec_64fc*>(spec), dly_src,
                              dly_dst, buf);
    };
}

template <>
ExecFn<Ipp32fc> ipp_exec<Ipp32fc, Ipp32f>() {
    return [](const Ipp32fc* src, Ipp32fc* dst, int n, void* spec, const Ipp32fc* dly_src,
              Ipp32fc* dly_dst, Ipp8u* buf) {
        return ippsFIRSR32f_32fc(src, dst, n, static_cast<IppsFIRSpec32f_32fc*>(spec),
                                 dly_src, dly_dst, buf);
    };
}

template <class Sample, class Tap>
Result<BackendPlan> make_typed(const FirPlanConfig& cfg) {
    if (exceeds_int(cfg.tap_count) || exceeds_int(cfg.block_size)) {
        return Status::UnsupportedConfig;
    }

    int spec_size = 0;
    int work_size = 0;
    const int taps_len = static_cast<int>(cfg.tap_count);
    Status st = map_status(get_size<Sample, Tap>(taps_len, &spec_size, &work_size));
    if (st != Status::Ok) return st;
    if (spec_size <= 0 || work_size < 0) return Status::BackendError;

    auto* s = new IppFirState<Sample, Tap>();
    s->block_size = cfg.block_size;
    s->batch = cfg.batch;
    s->tap_count = cfg.tap_count;
    s->spec.resize(static_cast<std::size_t>(spec_size));
    s->work.resize(static_cast<std::size_t>(work_size));
    const std::size_t delay_len = cfg.tap_count - 1;
    s->delay_a.assign(cfg.batch * delay_len, Sample{});
    s->delay_b.assign(cfg.batch * delay_len, Sample{});

    const auto* taps = static_cast<const Tap*>(cfg.taps);
    st = map_status(ipp_init<Sample, Tap>(taps, taps_len, s->spec.data()));
    if (st != Status::Ok) {
        delete s;
        return st;
    }

    BackendPlan bp;
    bp.state = s;
    bp.execute = &ipp_execute<Sample, Tap>;
    bp.clear = &ipp_clear<Sample, Tap>;
    bp.destroy = &ipp_destroy<Sample, Tap>;
    bp.workspace_bytes = s->spec.size() + s->work.size() +
                         (s->delay_a.size() + s->delay_b.size()) * sizeof(Sample);
    bp.name = "ipp";
    return bp;
}

}  // namespace

Result<BackendPlan> make_backend_plan(const FirPlanConfig& cfg) {
    if (invalid_common(cfg)) return Status::InvalidArgument;

    switch (cfg.precision) {
        case FirPrecision::F32:
            switch (cfg.type) {
                case FirType::Real: return make_typed<Ipp32f, Ipp32f>(cfg);
                case FirType::ComplexRealTaps: return make_typed<Ipp32fc, Ipp32f>(cfg);
                case FirType::Complex: return make_typed<Ipp32fc, Ipp32fc>(cfg);
            }
            break;
        case FirPrecision::F64:
            switch (cfg.type) {
                case FirType::Real: return make_typed<Ipp64f, Ipp64f>(cfg);
                case FirType::ComplexRealTaps: return Status::UnsupportedConfig;
                case FirType::Complex: return make_typed<Ipp64fc, Ipp64fc>(cfg);
            }
            break;
    }
    return Status::UnsupportedConfig;
}

}  // namespace fir
