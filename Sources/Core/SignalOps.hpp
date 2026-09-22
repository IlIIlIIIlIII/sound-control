#pragma once

// The small subset of vector, FFT and biquad operations used by the core.
// Apple retains Accelerate. Other platforms use allocation-free C++ processing;
// only setup creation allocates. FFT scaling matches Accelerate's complex FFT.
#ifdef __APPLE__
#include <Accelerate/Accelerate.h>
namespace personaltools::signal {
using BiquadSetup = vDSP_biquad_SetupD;
using FFTSetup = ::FFTSetup;
using SplitComplex = DSPSplitComplex;
inline constexpr auto radix2 = kFFTRadix2;
inline constexpr auto forward = FFT_FORWARD;
inline constexpr auto inverse = FFT_INVERSE;
inline constexpr auto createBiquad = vDSP_biquad_CreateSetupD;
inline constexpr auto destroyBiquad = vDSP_biquad_DestroySetupD;
inline constexpr auto biquad = vDSP_biquadD;
inline constexpr auto createFFT = vDSP_create_fftsetup;
inline constexpr auto destroyFFT = vDSP_destroy_fftsetup;
inline constexpr auto fft = vDSP_fft_zip;
inline constexpr auto scale = vDSP_vsmul;
inline constexpr auto scaleDouble = vDSP_vsmulD;
inline constexpr auto add = vDSP_vadd;
inline constexpr auto multiplyComplex = vDSP_zvmul;
}
#else
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <vector>

namespace personaltools::signal {
struct BiquadState { std::vector<double> coefficients; };
using BiquadSetup = BiquadState*;
inline BiquadSetup createBiquad(const double* coefficients, std::size_t count) {
    try { return new BiquadState{{coefficients, coefficients + count * 5}}; }
    catch (...) { return nullptr; }
}
inline void destroyBiquad(BiquadSetup setup) { delete setup; }
inline void biquad(BiquadSetup setup, double* delay, const double* input,
                   std::ptrdiff_t inputStride, double* output,
                   std::ptrdiff_t outputStride, std::size_t frames) {
    for (std::size_t i = 0; i < frames; ++i) {
        double value = input[i * inputStride];
        for (std::size_t s = 0; s < setup->coefficients.size() / 5; ++s) {
            const double* c = setup->coefficients.data() + 5 * s;
            double* z = delay + 2 * s;
            const double y = c[0] * value + z[0];
            z[0] = c[1] * value - c[3] * y + z[1];
            z[1] = c[2] * value - c[4] * y;
            value = y;
        }
        output[i * outputStride] = value;
    }
}
struct FFTState {
    std::vector<std::size_t> reversed;
    std::vector<float> cosine, sine;
};
using FFTSetup = FFTState*;
struct SplitComplex { float* realp; float* imagp; };
inline constexpr int radix2 = 2, forward = 1, inverse = -1;
inline FFTSetup createFFT(unsigned log2Size, int) {
    if (log2Size == 0 || log2Size > 20) return nullptr;
    try {
        const std::size_t n = std::size_t{1} << log2Size;
        FFTState state;
        state.reversed.resize(n);
        state.cosine.resize(n / 2);
        state.sine.resize(n / 2);
        for (std::size_t i = 0; i < n; ++i) {
            std::size_t x = i, r = 0;
            for (unsigned b = 0; b < log2Size; ++b) { r = (r << 1) | (x & 1); x >>= 1; }
            state.reversed[i] = r;
        }
        for (std::size_t i = 0; i < n / 2; ++i) {
            const double a = -2 * std::numbers::pi * static_cast<double>(i) / n;
            state.cosine[i] = static_cast<float>(std::cos(a));
            state.sine[i] = static_cast<float>(std::sin(a));
        }
        return new FFTState(std::move(state));
    } catch (...) { return nullptr; }
}
inline void destroyFFT(FFTSetup setup) { delete setup; }
inline void fft(FFTSetup setup, SplitComplex* data, std::ptrdiff_t stride,
                unsigned log2Size, int direction) {
    const std::size_t n = std::size_t{1} << log2Size;
    for (std::size_t i = 0; i < n; ++i) {
        const auto j = setup->reversed[i];
        if (j > i) {
            std::swap(data->realp[i * stride], data->realp[j * stride]);
            std::swap(data->imagp[i * stride], data->imagp[j * stride]);
        }
    }
    for (std::size_t width = 2; width <= n; width *= 2) {
        for (std::size_t base = 0; base < n; base += width) {
            for (std::size_t j = 0; j < width / 2; ++j) {
                const auto a = (base + j) * stride, b = (base + j + width / 2) * stride;
                const auto k = j * n / width;
                const float c = setup->cosine[k], s = setup->sine[k] * direction;
                const float r = c * data->realp[b] - s * data->imagp[b];
                const float im = s * data->realp[b] + c * data->imagp[b];
                data->realp[b] = data->realp[a] - r;
                data->imagp[b] = data->imagp[a] - im;
                data->realp[a] += r;
                data->imagp[a] += im;
            }
        }
    }
}
template<class T>
inline void scaleVector(const T* input, std::ptrdiff_t si, const T* gain,
                        T* output, std::ptrdiff_t so, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) output[i * so] = input[i * si] * *gain;
}
inline constexpr auto scale = scaleVector<float>;
inline constexpr auto scaleDouble = scaleVector<double>;
inline void add(const float* a, std::ptrdiff_t sa, const float* b, std::ptrdiff_t sb,
                float* out, std::ptrdiff_t so, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) out[i * so] = a[i * sa] + b[i * sb];
}
inline void multiplyComplex(const SplitComplex* a, std::ptrdiff_t sa,
                            const SplitComplex* b, std::ptrdiff_t sb,
                            SplitComplex* out, std::ptrdiff_t so, std::size_t n, int sign) {
    for (std::size_t i = 0; i < n; ++i) {
        const float ar = a->realp[i * sa], ai = a->imagp[i * sa] * sign;
        const float br = b->realp[i * sb], bi = b->imagp[i * sb];
        out->realp[i * so] = ar * br - ai * bi;
        out->imagp[i * so] = ar * bi + ai * br;
    }
}
}
#endif
