// Portable FFT engine: a self-contained, dependency-free complex DFT used by
// the portable backend. Supports arbitrary lengths:
//
//   - Powers of two use an in-place iterative radix-2 Cooley-Tukey transform.
//   - All other lengths use Bluestein's algorithm (chirp-z), which reduces the
//     length-N DFT to a power-of-two convolution.
//
// The engine computes the UNSCALED forward DFT:
//     X[k] = sum_{j} x[j] * exp(-2*pi*i*j*k/N)
// Inverse and normalization are applied by the backend via the identity
//     IDFT(x) = (1/N) * conj(DFT(conj(x))).
//
// This file is templated on the scalar type so the backend can instantiate it
// for both float32 and float64.
#ifndef DSP_DISPATCH_PORTABLE_FFT_ENGINE_H
#define DSP_DISPATCH_PORTABLE_FFT_ENGINE_H

#include <cmath>
#include <complex>
#include <cstddef>
#include <utility>
#include <vector>

namespace fft::portable {

template <class T>
class FftEngine {
public:
    using Complex = std::complex<T>;

    // Builds the precomputed tables for a forward DFT of length n (n >= 1).
    explicit FftEngine(std::size_t n) : n_(n) {
        bluestein_ = !is_pow2(n_) && n_ > 1;
        if (!bluestein_) {
            m_ = (n_ == 0) ? 1 : n_;
            build_twiddles(m_);
        } else {
            // Smallest power of two that fits the linear convolution of two
            // length-n sequences without circular wraparound.
            m_ = next_pow2(2 * n_ - 1);
            build_twiddles(m_);
            build_chirp();
        }
    }

    std::size_t length() const { return n_; }
    bool uses_bluestein() const { return bluestein_; }

    // Bytes of scratch the caller must provide to forward() (see scratch()).
    std::size_t scratch_complex_count() const { return bluestein_ ? m_ : 0; }

    // In-place unscaled forward DFT of `data` (length n_). For the Bluestein
    // path, `scratch` must point to at least scratch_complex_count() complex
    // values; it is ignored for the power-of-two path (may be null).
    void forward(Complex* data, Complex* scratch) const {
        if (n_ <= 1) return;
        if (!bluestein_) {
            fft_pow2(data, m_, /*inverse=*/false);
        } else {
            forward_bluestein(data, scratch);
        }
    }

private:
    static bool is_pow2(std::size_t v) { return v != 0 && (v & (v - 1)) == 0; }

    static std::size_t next_pow2(std::size_t v) {
        std::size_t p = 1;
        while (p < v) p <<= 1;
        return p;
    }

    void build_twiddles(std::size_t m) {
        // tw_[k] = exp(-2*pi*i*k/m) for k in [0, m/2).
        const double pi = 3.14159265358979323846264338327950288;
        tw_.resize(m / 2);
        for (std::size_t k = 0; k < m / 2; ++k) {
            double ang = -2.0 * pi * static_cast<double>(k) / static_cast<double>(m);
            tw_[k] = Complex(static_cast<T>(std::cos(ang)), static_cast<T>(std::sin(ang)));
        }
    }

    void build_chirp() {
        const double pi = 3.14159265358979323846264338327950288;
        chirp_.resize(n_);
        // chirp_[j] = exp(-i*pi*j^2/n). Reduce j^2 modulo 2n before forming the
        // angle to preserve precision for large n.
        const std::size_t twoN = 2 * n_;
        for (std::size_t j = 0; j < n_; ++j) {
            std::size_t jm = j % twoN;
            std::size_t jj = (jm * jm) % twoN;
            double ang = -pi * static_cast<double>(jj) / static_cast<double>(n_);
            chirp_[j] = Complex(static_cast<T>(std::cos(ang)), static_cast<T>(std::sin(ang)));
        }
        // b[j] = conj(chirp_[j]); symmetric about m: b[m-j] = b[j] for j in [1,n).
        std::vector<Complex> b(m_, Complex(0, 0));
        for (std::size_t j = 0; j < n_; ++j) {
            Complex v = std::conj(chirp_[j]);
            b[j] = v;
            if (j != 0) b[m_ - j] = v;
        }
        fft_pow2(b.data(), m_, /*inverse=*/false);
        bfreq_ = std::move(b);
    }

    // In-place iterative radix-2 over a power-of-two array of size m.
    void fft_pow2(Complex* a, std::size_t m, bool inverse) const {
        // Bit-reversal permutation.
        for (std::size_t i = 1, j = 0; i < m; ++i) {
            std::size_t bit = m >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(a[i], a[j]);
        }
        for (std::size_t len = 2; len <= m; len <<= 1) {
            const std::size_t half = len >> 1;
            const std::size_t step = m / len;
            for (std::size_t i = 0; i < m; i += len) {
                std::size_t k = 0;
                for (std::size_t j = 0; j < half; ++j, k += step) {
                    Complex w = inverse ? std::conj(tw_[k]) : tw_[k];
                    Complex u = a[i + j];
                    Complex v = a[i + j + half] * w;
                    a[i + j] = u + v;
                    a[i + j + half] = u - v;
                }
            }
        }
    }

    void forward_bluestein(Complex* data, Complex* A) const {
        // A[j] = data[j] * chirp[j], zero-padded to m.
        for (std::size_t j = 0; j < n_; ++j) A[j] = data[j] * chirp_[j];
        for (std::size_t j = n_; j < m_; ++j) A[j] = Complex(0, 0);
        // Circular convolution with b via the frequency domain.
        fft_pow2(A, m_, /*inverse=*/false);
        for (std::size_t k = 0; k < m_; ++k) A[k] *= bfreq_[k];
        fft_pow2(A, m_, /*inverse=*/true);
        const T inv_m = static_cast<T>(1) / static_cast<T>(m_);
        for (std::size_t k = 0; k < m_; ++k) A[k] *= inv_m;
        // De-chirp.
        for (std::size_t k = 0; k < n_; ++k) data[k] = A[k] * chirp_[k];
    }

    std::size_t n_;
    std::size_t m_ = 1;
    bool bluestein_ = false;
    std::vector<Complex> tw_;     // size m_/2
    std::vector<Complex> chirp_;  // size n_ (Bluestein only)
    std::vector<Complex> bfreq_;  // size m_ (Bluestein only)
};

}  // namespace fft::portable

#endif  // DSP_DISPATCH_PORTABLE_FFT_ENGINE_H
