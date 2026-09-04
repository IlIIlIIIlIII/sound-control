#include "DSP.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <iterator>
#include <numbers>

namespace macsound {
namespace {

std::complex<double> response(const BiquadCoefficients& c, double omega) {
    const auto z1 = std::exp(std::complex<double>(0.0, -omega));
    const auto z2 = z1 * z1;
    return (c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2);
}

std::vector<double> flatten(const std::vector<PEQFilter>& filters, double sampleRate) {
    std::vector<double> coefficients;
    coefficients.reserve(filters.size() * 5);
    for (const auto& filter : filters) {
        const auto c = peakingCoefficients(filter, sampleRate);
        coefficients.insert(coefficients.end(), {c.b0, c.b1, c.b2, c.a1, c.a2});
    }
    return coefficients;
}

}  // namespace

BiquadCoefficients peakingCoefficients(const PEQFilter& filter, double sampleRate) {
    const double a = std::pow(10.0, filter.gainDB / 40.0);
    const double omega = 2.0 * std::numbers::pi * filter.frequencyHz / sampleRate;
    const double alpha = std::sin(omega) / (2.0 * filter.q);
    const double cosine = std::cos(omega);
    const double a0 = 1.0 + alpha / a;

    return {
        (1.0 + alpha * a) / a0,
        (-2.0 * cosine) / a0,
        (1.0 - alpha * a) / a0,
        (-2.0 * cosine) / a0,
        (1.0 - alpha / a) / a0,
    };
}

double responseDB(const std::vector<PEQFilter>& filters, double frequencyHz, double sampleRate) {
    std::complex<double> total(1.0, 0.0);
    const double omega = 2.0 * std::numbers::pi * frequencyHz / sampleRate;
    for (const auto& filter : filters) total *= response(peakingCoefficients(filter, sampleRate), omega);
    return 20.0 * std::log10(std::max(std::abs(total), 1e-15));
}

double automaticPreampDB(const std::vector<PEQFilter>& left,
                         const std::vector<PEQFilter>& right,
                         double sampleRate) {
    constexpr std::size_t points = 32768;
    const double low = 10.0;
    const double high = std::min(20000.0, sampleRate * 0.499);
    if (high <= low) return 0.0;

    double peak = 0.0;
    const double ratio = high / low;
    for (std::size_t i = 0; i < points; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(points - 1);
        const double frequency = low * std::pow(ratio, t);
        peak = std::max({peak,
                         responseDB(left, frequency, sampleRate),
                         responseDB(right, frequency, sampleRate)});
    }
    return -std::max(0.0, peak);
}

StereoDSP::~StereoDSP() { destroy(); }

void StereoDSP::destroy() {
    if (leftSetup_) vDSP_biquad_DestroySetupD(leftSetup_);
    if (rightSetup_) vDSP_biquad_DestroySetupD(rightSetup_);
    leftSetup_ = nullptr;
    rightSetup_ = nullptr;
    leftDelay_.clear();
    rightDelay_.clear();
    configured_ = false;
}

bool StereoDSP::configure(const std::vector<PEQFilter>& left,
                          const std::vector<PEQFilter>& right,
                          double sampleRate,
                          std::string& error) {
    if (sampleRate <= 0.0 || left.empty() || right.empty()) {
        error = "A positive sample rate and both channel filters are required";
        return false;
    }
    const auto validate = [sampleRate](const std::vector<PEQFilter>& filters) {
        return std::all_of(filters.begin(), filters.end(), [sampleRate](const PEQFilter& filter) {
            return filter.frequencyHz > 0.0 && filter.frequencyHz < sampleRate * 0.5 &&
                   filter.q > 0.0 && std::isfinite(filter.gainDB);
        });
    };
    if (!validate(left) || !validate(right)) {
        error = "A filter is outside the valid range for the current sample rate";
        return false;
    }

    std::vector<PEQFilter> effectiveLeft;
    std::vector<PEQFilter> effectiveRight;
    std::copy_if(left.begin(), left.end(), std::back_inserter(effectiveLeft),
                 [](const PEQFilter& filter) { return std::abs(filter.gainDB) > 1e-12; });
    std::copy_if(right.begin(), right.end(), std::back_inserter(effectiveRight),
                 [](const PEQFilter& filter) { return std::abs(filter.gainDB) > 1e-12; });

    const auto leftCoefficients = flatten(effectiveLeft, sampleRate);
    const auto rightCoefficients = flatten(effectiveRight, sampleRate);
    auto newLeft = effectiveLeft.empty()
        ? nullptr
        : vDSP_biquad_CreateSetupD(leftCoefficients.data(), effectiveLeft.size());
    auto newRight = effectiveRight.empty()
        ? nullptr
        : vDSP_biquad_CreateSetupD(rightCoefficients.data(), effectiveRight.size());
    if ((!effectiveLeft.empty() && !newLeft) || (!effectiveRight.empty() && !newRight)) {
        if (newLeft) vDSP_biquad_DestroySetupD(newLeft);
        if (newRight) vDSP_biquad_DestroySetupD(newRight);
        error = "vDSP failed to allocate filter state";
        return false;
    }

    destroy();
    leftSetup_ = newLeft;
    rightSetup_ = newRight;
    leftDelay_.assign(effectiveLeft.empty() ? 0 : 2 * effectiveLeft.size() + 2, 0.0);
    rightDelay_.assign(effectiveRight.empty() ? 0 : 2 * effectiveRight.size() + 2, 0.0);
    preampDB_ = automaticPreampDB(left, right, sampleRate);
    linearPreamp_ = std::pow(10.0, preampDB_ / 20.0);
    configured_ = true;
    error.clear();
    return true;
}

void StereoDSP::reset() {
    std::fill(leftDelay_.begin(), leftDelay_.end(), 0.0);
    std::fill(rightDelay_.begin(), rightDelay_.end(), 0.0);
}

void StereoDSP::processPlanar(double* left, double* right, std::size_t frames) {
    if (!configured() || !left || !right || frames == 0) return;
    if (leftSetup_) vDSP_biquadD(leftSetup_, leftDelay_.data(), left, 1, left, 1, frames);
    if (rightSetup_) vDSP_biquadD(rightSetup_, rightDelay_.data(), right, 1, right, 1, frames);
    if (linearPreamp_ != 1.0) {
        vDSP_vsmulD(left, 1, &linearPreamp_, left, 1, frames);
        vDSP_vsmulD(right, 1, &linearPreamp_, right, 1, frames);
    }
}

void StereoDSP::processInterleaved(double* stereo, std::size_t frames) {
    if (!configured() || !stereo || frames == 0) return;
    if (leftSetup_) vDSP_biquadD(leftSetup_, leftDelay_.data(), stereo, 2, stereo, 2, frames);
    if (rightSetup_) vDSP_biquadD(rightSetup_, rightDelay_.data(), stereo + 1, 2, stereo + 1, 2, frames);
    if (linearPreamp_ != 1.0) {
        vDSP_vsmulD(stereo, 2, &linearPreamp_, stereo, 2, frames);
        vDSP_vsmulD(stereo + 1, 2, &linearPreamp_, stereo + 1, 2, frames);
    }
}

}  // namespace macsound
