#include "AEC.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <numbers>

namespace macsound {
namespace {

constexpr std::array<double, 4> kReferenceRates{44100.0, 48000.0, 96000.0, 192000.0};

float sinc(double value) {
    if (std::abs(value) < 1e-12) return 1.0f;
    const double argument = std::numbers::pi * value;
    return static_cast<float>(std::sin(argument) / argument);
}

std::size_t nearestRateIndex(double rate) {
    std::size_t result = 0;
    double distance = std::abs(rate - kReferenceRates[0]);
    for (std::size_t index = 1; index < kReferenceRates.size(); ++index) {
        const double candidate = std::abs(rate - kReferenceRates[index]);
        if (candidate < distance) {
            distance = candidate;
            result = index;
        }
    }
    return result;
}

inline std::size_t spectrumOffset(std::size_t partition) {
    return partition * EchoCanceller::fftSize;
}

}  // namespace

const char* echoProfileName(EchoProfile profile) {
    switch (profile) {
        case EchoProfile::adaptive: return "adaptive";
        case EchoProfile::balanced: return "balanced";
        case EchoProfile::strong: return "strong";
        case EchoProfile::quality:
        default: return "quality";
    }
}

EchoProfile echoProfileFromName(const char* name) {
    (void)name;
    // Old quality/balanced/strong preferences all migrate to the one public
    // mode. Keeping the legacy enum values above is useful only for offline
    // profile-regression fixtures.
    return EchoProfile::adaptive;
}

const char* echoConvergenceName(EchoConvergenceState state) {
    switch (state) {
        case EchoConvergenceState::converged: return "converged";
        case EchoConvergenceState::tracking: return "tracking";
        case EchoConvergenceState::learning:
        default: return "learning";
    }
}

StereoReferenceTimeline::StereoReferenceTimeline()
    : left_(capacity, 0.0f), right_(capacity, 0.0f) {
    for (std::size_t rateIndex = 0; rateIndex < kReferenceRates.size(); ++rateIndex) {
        auto& table = coefficients_[rateIndex];
        table.resize(phases * taps);
        const double rate = kReferenceRates[rateIndex];
        const double cutoff = 0.95 * std::min(1.0, 48000.0 / rate);
        for (std::size_t phase = 0; phase < phases; ++phase) {
            const double fraction = static_cast<double>(phase) / phases;
            double sum = 0.0;
            for (std::size_t tap = 0; tap < taps; ++tap) {
                const double position = static_cast<double>(tap) - (taps / 2 - 1) - fraction;
                const double window = 0.5 - 0.5 * std::cos(
                    2.0 * std::numbers::pi * (static_cast<double>(tap) + 0.5) / taps);
                const float coefficient = static_cast<float>(cutoff * sinc(cutoff * position) * window);
                table[phase * taps + tap] = coefficient;
                sum += coefficient;
            }
            if (std::abs(sum) > 1e-12) {
                for (std::size_t tap = 0; tap < taps; ++tap) {
                    table[phase * taps + tap] = static_cast<float>(
                        table[phase * taps + tap] / sum);
                }
            }
        }
    }
}

void StereoReferenceTimeline::reset() {
    generation_.fetch_add(1, std::memory_order_acq_rel);
    anchorGeneration_.store(0, std::memory_order_release);
    previousStartIndex_ = 0;
    previousHostTime_ = 0;
    smoothedRate_ = 0.0;
    previousNominalRate_ = 0.0;
}

void StereoReferenceTimeline::push(const float* left, const float* right,
                                   std::size_t frames, double nominalSampleRate,
                                   std::uint64_t firstHostTime,
                                   double hostTicksPerSecond) {
    if (!left || !right || frames == 0 || nominalSampleRate <= 0.0 ||
        firstHostTime == 0 || hostTicksPerSecond <= 0.0) return;

    const std::uint64_t start = writeIndex_.load(std::memory_order_relaxed);
    if (previousNominalRate_ <= 0.0 ||
        std::abs(previousNominalRate_ - nominalSampleRate) >= 0.5) {
        generation_.fetch_add(1, std::memory_order_acq_rel);
        previousStartIndex_ = 0;
        previousHostTime_ = 0;
        smoothedRate_ = nominalSampleRate;
        previousNominalRate_ = nominalSampleRate;
    } else if (previousHostTime_ != 0 && firstHostTime > previousHostTime_ &&
               start > previousStartIndex_) {
        const double measured = static_cast<double>(start - previousStartIndex_) *
            hostTicksPerSecond / static_cast<double>(firstHostTime - previousHostTime_);
        const double minimum = nominalSampleRate * 0.995;
        const double maximum = nominalSampleRate * 1.005;
        if (measured >= minimum && measured <= maximum) {
            smoothedRate_ += 0.02 * (measured - smoothedRate_);
        }
    }

    for (std::size_t frame = 0; frame < frames; ++frame) {
        const std::size_t slot = static_cast<std::size_t>((start + frame) & (capacity - 1u));
        left_[slot] = left[frame];
        right_[slot] = right[frame];
    }
    writeIndex_.store(start + frames, std::memory_order_release);

    const std::uint64_t odd = sequence_.fetch_add(1, std::memory_order_acq_rel) + 1u;
    anchorStartIndex_.store(start, std::memory_order_relaxed);
    anchorHostTime_.store(firstHostTime, std::memory_order_relaxed);
    anchorRateBits_.store(std::bit_cast<std::uint64_t>(smoothedRate_),
                          std::memory_order_relaxed);
    anchorGeneration_.store(generation_.load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
    sequence_.store(odd + 1u, std::memory_order_release);

    previousStartIndex_ = start;
    previousHostTime_ = firstHostTime;
}

bool StereoReferenceTimeline::loadAnchor(Anchor& anchor) const {
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        const std::uint64_t before = sequence_.load(std::memory_order_acquire);
        if ((before & 1u) != 0u || before == 0u) continue;
        anchor.startIndex = anchorStartIndex_.load(std::memory_order_relaxed);
        anchor.hostTime = anchorHostTime_.load(std::memory_order_relaxed);
        anchor.sampleRate = std::bit_cast<double>(
            anchorRateBits_.load(std::memory_order_relaxed));
        anchor.generation = anchorGeneration_.load(std::memory_order_relaxed);
        const std::uint64_t after = sequence_.load(std::memory_order_acquire);
        if (before == after && anchor.generation == generation_.load(std::memory_order_acquire)) {
            return anchor.hostTime != 0 && anchor.sampleRate > 0.0;
        }
    }
    return false;
}

double StereoReferenceTimeline::sourceSampleRate() const {
    Anchor anchor;
    return loadAnchor(anchor) ? anchor.sampleRate : 0.0;
}

const float* StereoReferenceTimeline::coefficientTable(double sourceRate) const {
    return coefficients_[nearestRateIndex(sourceRate)].data();
}

float StereoReferenceTimeline::interpolate(const std::vector<float>& ring,
                                           double position,
                                           const float* table) const {
    const auto integer = static_cast<std::int64_t>(std::floor(position));
    const double fraction = position - std::floor(position);
    const std::size_t phase = std::min<std::size_t>(
        phases - 1u, static_cast<std::size_t>(fraction * phases));
    const float* coefficients = table + phase * taps;
    const std::int64_t first = integer - static_cast<std::int64_t>(taps / 2 - 1);
    float result = 0.0f;
    for (std::size_t tap = 0; tap < taps; ++tap) {
        const std::uint64_t index = static_cast<std::uint64_t>(
            first + static_cast<std::int64_t>(tap));
        result += ring[static_cast<std::size_t>(index & (capacity - 1u))] * coefficients[tap];
    }
    return result;
}

bool StereoReferenceTimeline::render(std::uint64_t firstHostTime,
                                     double hostTicksPerSecond,
                                     float* left, float* right,
                                     std::size_t frames) {
    if (!left || !right || frames == 0 || firstHostTime == 0 || hostTicksPerSecond <= 0.0) {
        return false;
    }
    Anchor anchor;
    if (!loadAnchor(anchor)) {
        std::fill_n(left, frames, 0.0f);
        std::fill_n(right, frames, 0.0f);
        underruns_.fetch_add(frames, std::memory_order_relaxed);
        return false;
    }

    const double hostDelta = firstHostTime >= anchor.hostTime
        ? static_cast<double>(firstHostTime - anchor.hostTime)
        : -static_cast<double>(anchor.hostTime - firstHostTime);
    double position = static_cast<double>(anchor.startIndex) +
        hostDelta * anchor.sampleRate / hostTicksPerSecond;
    const double step = anchor.sampleRate / 48000.0;
    const std::uint64_t written = writeIndex_.load(std::memory_order_acquire);
    const double firstNeeded = position - static_cast<double>(taps / 2);
    const double lastNeeded = position + step * static_cast<double>(frames - 1u) +
                              static_cast<double>(taps / 2 + 1u);
    const double oldest = written > capacity ? static_cast<double>(written - capacity) : 0.0;
    if (firstNeeded < oldest || lastNeeded >= static_cast<double>(written)) {
        std::fill_n(left, frames, 0.0f);
        std::fill_n(right, frames, 0.0f);
        underruns_.fetch_add(frames, std::memory_order_relaxed);
        return false;
    }

    const float* table = coefficientTable(anchor.sampleRate);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        left[frame] = interpolate(left_, position, table);
        right[frame] = interpolate(right_, position, table);
        position += step;
    }
    return true;
}

EchoCanceller::EchoCanceller()
    : fftSetup_(vDSP_create_fftsetup(9, kFFTRadix2)),
      fftInput_(fftSize, 0.0f), workReal_(fftSize, 0.0f),
      workImaginary_(fftSize, 0.0f), inverseReal_(fftSize, 0.0f),
      inverseImaginary_(fftSize, 0.0f), inverseTime_(fftSize, 0.0f),
      echo_(blockSize, 0.0f), shadowEcho_(blockSize, 0.0f),
      nonlinearEcho_(blockSize, 0.0f), linearError_(blockSize, 0.0f),
      shadowError_(blockSize, 0.0f), nonlinearCandidateError_(blockSize, 0.0f),
      error_(blockSize, 0.0f), previousError_(blockSize, 0.0f),
      suppressedTime_(fftSize, 0.0f),
      linearSpectrumReal_(fftSize, 0.0f), linearSpectrumImaginary_(fftSize, 0.0f),
      shadowSpectrumReal_(fftSize, 0.0f), shadowSpectrumImaginary_(fftSize, 0.0f),
      nonlinearSpectrumReal_(fftSize, 0.0f), nonlinearSpectrumImaginary_(fftSize, 0.0f),
      errorSpectrumReal_(fftSize, 0.0f), errorSpectrumImaginary_(fftSize, 0.0f),
      microphoneSpectrumReal_(fftSize, 0.0f), microphoneSpectrumImaginary_(fftSize, 0.0f),
      suppressionSpectrumReal_(fftSize, 0.0f), suppressionSpectrumImaginary_(fftSize, 0.0f),
      covarianceMid_(spectrumBins, 1e-3f), covarianceSide_(spectrumBins, 1e-3f),
      covarianceCrossReal_(spectrumBins, 0.0f),
      covarianceCrossImaginary_(spectrumBins, 0.0f),
      historyMidPower_(partitions * spectrumBins, 0.0f),
      historySidePower_(partitions * spectrumBins, 0.0f),
      historyCrossReal_(partitions * spectrumBins, 0.0f),
      historyCrossImaginary_(partitions * spectrumBins, 0.0f),
      referenceMidPowerSum_(spectrumBins, 0.0f),
      referenceSidePowerSum_(spectrumBins, 0.0f),
      referenceCrossRealSum_(spectrumBins, 0.0f),
      referenceCrossImaginarySum_(spectrumBins, 0.0f),
      inverseCovarianceMid_(spectrumBins, 0.0f),
      inverseCovarianceSide_(spectrumBins, 0.0f),
      inverseCovarianceCrossReal_(spectrumBins, 0.0f),
      inverseCovarianceCrossImaginary_(spectrumBins, 0.0f),
      microphonePower_(spectrumBins, 1e-6f), echoPower_(spectrumBins, 1e-6f),
      errorPower_(spectrumBins, 1e-6f),
      microphoneEchoCrossReal_(spectrumBins, 0.0f),
      microphoneEchoCrossImaginary_(spectrumBins, 0.0f),
      nearEndProbability_(spectrumBins, 0.0f), residualGain_(spectrumBins, 1.0f),
      targetGain_(spectrumBins, 1.0f), frequencySmoothedGain_(spectrumBins, 1.0f),
      lateEchoPower_(spectrumBins, 0.0f) {
    for (std::size_t channel = 0; channel < 2; ++channel) {
        referenceReal_[channel].assign(partitions * fftSize, 0.0f);
        referenceImaginary_[channel].assign(partitions * fftSize, 0.0f);
        filterReal_[channel].assign(partitions * fftSize, 0.0f);
        filterImaginary_[channel].assign(partitions * fftSize, 0.0f);
        shadowFilterReal_[channel].assign(partitions * fftSize, 0.0f);
        shadowFilterImaginary_[channel].assign(partitions * fftSize, 0.0f);
        previousReference_[channel].assign(blockSize, 0.0f);
        referenceBlock_[channel].assign(blockSize, 0.0f);
        partitionEnergy_[channel].assign(partitions, 0.0f);
        partitionPeak_[channel].assign(partitions, 0u);
    }
    for (std::size_t stream = 0; stream < 4; ++stream) {
        nonlinearReferenceReal_[stream].assign(nonlinearPartitions * fftSize, 0.0f);
        nonlinearReferenceImaginary_[stream].assign(nonlinearPartitions * fftSize, 0.0f);
        nonlinearFilterReal_[stream].assign(nonlinearPartitions * fftSize, 0.0f);
        nonlinearFilterImaginary_[stream].assign(nonlinearPartitions * fftSize, 0.0f);
        previousNonlinearReference_[stream].assign(blockSize, 0.0f);
        nonlinearReferenceBlock_[stream].assign(blockSize, 0.0f);
        nonlinearPower_[stream].assign(fftSize, 1e-3f);
    }
}

EchoCanceller::~EchoCanceller() {
    if (fftSetup_) vDSP_destroy_fftsetup(fftSetup_);
}

void EchoCanceller::reset() {
    profile_ = requestedProfile_.load(std::memory_order_acquire);
    for (std::size_t channel = 0; channel < 2; ++channel) {
        std::fill(referenceReal_[channel].begin(), referenceReal_[channel].end(), 0.0f);
        std::fill(referenceImaginary_[channel].begin(), referenceImaginary_[channel].end(), 0.0f);
        std::fill(filterReal_[channel].begin(), filterReal_[channel].end(), 0.0f);
        std::fill(filterImaginary_[channel].begin(), filterImaginary_[channel].end(), 0.0f);
        std::fill(shadowFilterReal_[channel].begin(), shadowFilterReal_[channel].end(), 0.0f);
        std::fill(shadowFilterImaginary_[channel].begin(), shadowFilterImaginary_[channel].end(), 0.0f);
        std::fill(previousReference_[channel].begin(), previousReference_[channel].end(), 0.0f);
        std::fill(referenceBlock_[channel].begin(), referenceBlock_[channel].end(), 0.0f);
        std::fill(partitionEnergy_[channel].begin(), partitionEnergy_[channel].end(), 0.0f);
        std::fill(partitionPeak_[channel].begin(), partitionPeak_[channel].end(), 0u);
    }
    for (std::size_t stream = 0; stream < 4; ++stream) {
        std::fill(nonlinearReferenceReal_[stream].begin(), nonlinearReferenceReal_[stream].end(), 0.0f);
        std::fill(nonlinearReferenceImaginary_[stream].begin(), nonlinearReferenceImaginary_[stream].end(), 0.0f);
        std::fill(nonlinearFilterReal_[stream].begin(), nonlinearFilterReal_[stream].end(), 0.0f);
        std::fill(nonlinearFilterImaginary_[stream].begin(), nonlinearFilterImaginary_[stream].end(), 0.0f);
        std::fill(previousNonlinearReference_[stream].begin(), previousNonlinearReference_[stream].end(), 0.0f);
        std::fill(nonlinearReferenceBlock_[stream].begin(), nonlinearReferenceBlock_[stream].end(), 0.0f);
        std::fill(nonlinearPower_[stream].begin(), nonlinearPower_[stream].end(), 1e-3f);
    }
    std::fill(covarianceMid_.begin(), covarianceMid_.end(), 1e-3f);
    std::fill(covarianceSide_.begin(), covarianceSide_.end(), 1e-3f);
    std::fill(covarianceCrossReal_.begin(), covarianceCrossReal_.end(), 0.0f);
    std::fill(covarianceCrossImaginary_.begin(), covarianceCrossImaginary_.end(), 0.0f);
    std::fill(historyMidPower_.begin(), historyMidPower_.end(), 0.0f);
    std::fill(historySidePower_.begin(), historySidePower_.end(), 0.0f);
    std::fill(historyCrossReal_.begin(), historyCrossReal_.end(), 0.0f);
    std::fill(historyCrossImaginary_.begin(), historyCrossImaginary_.end(), 0.0f);
    std::fill(referenceMidPowerSum_.begin(), referenceMidPowerSum_.end(), 0.0f);
    std::fill(referenceSidePowerSum_.begin(), referenceSidePowerSum_.end(), 0.0f);
    std::fill(referenceCrossRealSum_.begin(), referenceCrossRealSum_.end(), 0.0f);
    std::fill(referenceCrossImaginarySum_.begin(), referenceCrossImaginarySum_.end(), 0.0f);
    std::fill(microphonePower_.begin(), microphonePower_.end(), 1e-6f);
    std::fill(echoPower_.begin(), echoPower_.end(), 1e-6f);
    std::fill(errorPower_.begin(), errorPower_.end(), 1e-6f);
    std::fill(microphoneEchoCrossReal_.begin(), microphoneEchoCrossReal_.end(), 0.0f);
    std::fill(microphoneEchoCrossImaginary_.begin(), microphoneEchoCrossImaginary_.end(), 0.0f);
    std::fill(nearEndProbability_.begin(), nearEndProbability_.end(), 0.0f);
    std::fill(residualGain_.begin(), residualGain_.end(), 1.0f);
    std::fill(targetGain_.begin(), targetGain_.end(), 1.0f);
    std::fill(frequencySmoothedGain_.begin(), frequencySmoothedGain_.end(), 1.0f);
    std::fill(lateEchoPower_.begin(), lateEchoPower_.end(), 0.0f);
    std::fill(previousError_.begin(), previousError_.end(), 0.0f);
    nonlinearInputPower_ = {1e-4f, 1e-4f};
    historyPosition_ = 0;
    nonlinearHistoryPosition_ = 0;
    constraintPosition_ = 0;
    shadowConstraintPosition_ = 0;
    nonlinearConstraintPosition_ = 0;
    processedBlocks_ = 0;
    doubleTalkCandidateBlocks_ = 0;
    doubleTalkHangover_ = 0;
    shadowBetterEvaluations_ = 0;
    pathDropBlocks_ = 0;
    trackingBlocks_ = 0;
    nonlinearBetterBlocks_ = 0;
    nonlinearWorseBlocks_ = 0;
    farEndHangoverBlocks_ = 0;
    nonlinearAccepted_ = false;
    promotionSecondBlock_ = false;
    shadowTrackingActive_ = false;
    referenceDiscontinuity_ = false;
    missingReferenceFrames_ = 0;
    lastNearEndShare_ = 0.0f;
    bestFarEndReductionDB_ = 0.0f;
    activeMix_ = 0.0f;
    active_.store(0, std::memory_order_relaxed);
    doubleTalk_.store(0, std::memory_order_relaxed);
    nonlinearActive_.store(0, std::memory_order_relaxed);
    inputClipping_.store(0, std::memory_order_relaxed);
    convergenceState_.store(static_cast<std::uint32_t>(EchoConvergenceState::learning),
                            std::memory_order_relaxed);
    reductionDB_.store(0.0f, std::memory_order_relaxed);
    linearReductionDB_.store(0.0f, std::memory_order_relaxed);
    residualSuppressionDB_.store(0.0f, std::memory_order_relaxed);
    estimatedDelayMs_.store(0.0f, std::memory_order_relaxed);
    pathChangeCount_.store(0, std::memory_order_relaxed);
}

void EchoCanceller::forward(const float* time, float* real, float* imaginary) {
    std::memcpy(real, time, fftSize * sizeof(float));
    std::fill_n(imaginary, fftSize, 0.0f);
    DSPSplitComplex split{real, imaginary};
    vDSP_fft_zip(fftSetup_, &split, 1, 9, FFT_FORWARD);
}

void EchoCanceller::inverse(const float* real, const float* imaginary, float* time) {
    std::memcpy(inverseReal_.data(), real, fftSize * sizeof(float));
    std::memcpy(inverseImaginary_.data(), imaginary, fftSize * sizeof(float));
    DSPSplitComplex split{inverseReal_.data(), inverseImaginary_.data()};
    vDSP_fft_zip(fftSetup_, &split, 1, 9, FFT_INVERSE);
    const float scale = 1.0f / static_cast<float>(fftSize);
    vDSP_vsmul(inverseReal_.data(), 1, &scale, time, 1, fftSize);
}

float EchoCanceller::minimumGain(bool doubleTalk) const {
    if (doubleTalk) {
        switch (profile_) {
            case EchoProfile::adaptive: return 0.7079458f;
            case EchoProfile::balanced: return 0.7079458f;
            case EchoProfile::strong: return 0.5011872f;
            case EchoProfile::quality:
            default: return 1.0f;
        }
    }
    switch (profile_) {
        case EchoProfile::adaptive: return 0.0316228f;
        case EchoProfile::balanced: return 0.0630957f;
        case EchoProfile::strong: return 0.0158489f;
        case EchoProfile::quality:
        default: return 0.2511886f;
    }
}

void EchoCanceller::synthesizeLinearEcho(const StereoBank& filterReal,
                                         const StereoBank& filterImaginary,
                                         float* outputReal, float* outputImaginary) {
    std::fill_n(outputReal, fftSize, 0.0f);
    std::fill_n(outputImaginary, fftSize, 0.0f);
    DSPSplitComplex product{workReal_.data(), workImaginary_.data()};
    for (std::size_t channel = 0; channel < 2; ++channel) {
        for (std::size_t partition = 0; partition < partitions; ++partition) {
            const std::size_t history =
                (historyPosition_ + partitions - partition) % partitions;
            const std::size_t xOffset = spectrumOffset(history);
            const std::size_t hOffset = spectrumOffset(partition);
            DSPSplitComplex reference{
                referenceReal_[channel].data() + xOffset,
                referenceImaginary_[channel].data() + xOffset};
            DSPSplitComplex filter{
                const_cast<float*>(filterReal[channel].data() + hOffset),
                const_cast<float*>(filterImaginary[channel].data() + hOffset)};
            vDSP_zvmul(&filter, 1, &reference, 1, &product, 1, fftSize, 1);
            vDSP_vadd(workReal_.data(), 1, outputReal, 1,
                      outputReal, 1, fftSize);
            vDSP_vadd(workImaginary_.data(), 1, outputImaginary, 1,
                      outputImaginary, 1, fftSize);
        }
    }
}

void EchoCanceller::updateReferenceStatistics() {
    // Every filter partition is updated from the same error spectrum.  The
    // normalization must therefore contain the summed power of the complete
    // 256 ms reference history.  Dividing this value by the active partition
    // count makes the effective NLMS step grow by as much as 48x and causes a
    // dynamic full-scale music stream to repeatedly diverge and reset.
    const std::size_t spectrum = spectrumOffset(historyPosition_);
    const std::size_t statistics = historyPosition_ * spectrumBins;
    for (std::size_t bin = 0; bin < spectrumBins; ++bin) {
        const float mr = referenceReal_[0][spectrum + bin];
        const float mi = referenceImaginary_[0][spectrum + bin];
        const float sr = referenceReal_[1][spectrum + bin];
        const float si = referenceImaginary_[1][spectrum + bin];
        const float newMidPower = mr * mr + mi * mi;
        const float newSidePower = sr * sr + si * si;
        const float newCrossReal = mr * sr + mi * si;
        const float newCrossImaginary = mi * sr - mr * si;
        const std::size_t slot = statistics + bin;
        referenceMidPowerSum_[bin] += newMidPower - historyMidPower_[slot];
        referenceSidePowerSum_[bin] += newSidePower - historySidePower_[slot];
        referenceCrossRealSum_[bin] += newCrossReal - historyCrossReal_[slot];
        referenceCrossImaginarySum_[bin] +=
            newCrossImaginary - historyCrossImaginary_[slot];
        historyMidPower_[slot] = newMidPower;
        historySidePower_[slot] = newSidePower;
        historyCrossReal_[slot] = newCrossReal;
        historyCrossImaginary_[slot] = newCrossImaginary;
        const float midPower = std::max(0.0f, referenceMidPowerSum_[bin]);
        const float sidePower = std::max(0.0f, referenceSidePowerSum_[bin]);
        const float crossReal = referenceCrossRealSum_[bin];
        const float crossImaginary = referenceCrossImaginarySum_[bin];
        covarianceMid_[bin] = 0.90f * covarianceMid_[bin] + 0.10f * midPower;
        covarianceSide_[bin] = 0.90f * covarianceSide_[bin] + 0.10f * sidePower;
        covarianceCrossReal_[bin] =
            0.90f * covarianceCrossReal_[bin] + 0.10f * crossReal;
        covarianceCrossImaginary_[bin] =
            0.90f * covarianceCrossImaginary_[bin] + 0.10f * crossImaginary;

        float a = covarianceMid_[bin];
        float b = covarianceSide_[bin];
        float cr = covarianceCrossReal_[bin];
        float ci = covarianceCrossImaginary_[bin];
        const float maximumCrossSquared = 0.98f * a * b;
        const float crossSquared = cr * cr + ci * ci;
        if (crossSquared > maximumCrossSquared && crossSquared > 0.0f) {
            const float scale = std::sqrt(maximumCrossSquared / crossSquared);
            cr *= scale;
            ci *= scale;
        }
        const float regularization = 1.0f + 0.001f * (a + b);
        const float ar = a + regularization;
        const float br = b + regularization;
        const float determinant = std::max(ar * br - cr * cr - ci * ci,
                                           0.05f * ar * br);
        const float normalizedCorrelation = crossSquared /
            std::max(a * b, 1e-12f);
        if (b < std::max(1e-3f, a * 0.001f)) {
            inverseCovarianceMid_[bin] = 1.0f / ar;
            inverseCovarianceSide_[bin] = 0.0f;
            inverseCovarianceCrossReal_[bin] = 0.0f;
            inverseCovarianceCrossImaginary_[bin] = 0.0f;
        } else if (normalizedCorrelation > 0.80f) {
            // A nearly rank-one stereo bin has no unique L/R solution.  Mid
            // alone can represent that bin's combined acoustic path.  Freeze
            // Side instead of allowing two non-unique filters to drift in
            // opposite directions.
            inverseCovarianceMid_[bin] = 1.0f / ar;
            inverseCovarianceSide_[bin] = 0.0f;
            inverseCovarianceCrossReal_[bin] = 0.0f;
            inverseCovarianceCrossImaginary_[bin] = 0.0f;
        } else {
            inverseCovarianceMid_[bin] = br / determinant;
            inverseCovarianceSide_[bin] = ar / determinant;
            inverseCovarianceCrossReal_[bin] = -cr / determinant;
            inverseCovarianceCrossImaginary_[bin] = -ci / determinant;
        }
    }
}

void EchoCanceller::adaptLinearFilter(StereoBank& filterReal,
                                      StereoBank& filterImaginary,
                                      const float* errorReal,
                                      const float* errorImaginary,
                                      float step) {
    if (step <= 0.0f) return;
    for (std::size_t partition = 0; partition < partitions; ++partition) {
        const std::size_t history =
            (historyPosition_ + partitions - partition) % partitions;
        const std::size_t xOffset = spectrumOffset(history);
        const std::size_t hOffset = spectrumOffset(partition);
        for (std::size_t bin = 0; bin < spectrumBins; ++bin) {
            const std::size_t uniqueBin = bin;
            const float mr = referenceReal_[0][xOffset + bin];
            const float mi = referenceImaginary_[0][xOffset + bin];
            const float sr = referenceReal_[1][xOffset + bin];
            const float si = referenceImaginary_[1][xOffset + bin];
            const float er = errorReal[bin];
            const float ei = errorImaginary[bin];
            const float qmReal = mr * er + mi * ei;
            const float qmImaginary = mr * ei - mi * er;
            const float qsReal = sr * er + si * ei;
            const float qsImaginary = sr * ei - si * er;
            const float cm = inverseCovarianceMid_[uniqueBin];
            const float cs = inverseCovarianceSide_[uniqueBin];
            const float cr = inverseCovarianceCrossReal_[uniqueBin];
            const float ci = inverseCovarianceCrossImaginary_[uniqueBin];
            const float localPower = mr * mr + mi * mi + sr * sr + si * si;
            const float smoothedPower = covarianceMid_[uniqueBin] +
                covarianceSide_[uniqueBin];
            const float transientScale = std::min(
                1.0f, (smoothedPower + 1.0f) / (localPower + 1.0f));
            const float gmReal = transientScale *
                (cm * qmReal + cr * qsReal - ci * qsImaginary);
            const float gmImaginary = transientScale *
                (cm * qmImaginary + cr * qsImaginary + ci * qsReal);
            const float gsReal = transientScale *
                (cr * qmReal + ci * qmImaginary + cs * qsReal);
            const float gsImaginary = transientScale *
                (cr * qmImaginary - ci * qmReal + cs * qsImaginary);
            filterReal[0][hOffset + bin] += step * gmReal;
            filterImaginary[0][hOffset + bin] += step * gmImaginary;
            filterReal[1][hOffset + bin] += step * gsReal;
            filterImaginary[1][hOffset + bin] += step * gsImaginary;
            if (bin > 0u && bin < fftSize / 2u) {
                const std::size_t mirror = fftSize - bin;
                filterReal[0][hOffset + mirror] += step * gmReal;
                filterImaginary[0][hOffset + mirror] -= step * gmImaginary;
                filterReal[1][hOffset + mirror] += step * gsReal;
                filterImaginary[1][hOffset + mirror] -= step * gsImaginary;
            }
        }
    }
}

void EchoCanceller::constrainLinearPartition(StereoBank& filterReal,
                                             StereoBank& filterImaginary,
                                             std::size_t partition,
                                             bool updateMetrics) {
    for (std::size_t channel = 0; channel < 2; ++channel) {
        const std::size_t offset = spectrumOffset(partition);
        inverse(filterReal[channel].data() + offset,
                filterImaginary[channel].data() + offset,
                inverseTime_.data());
        std::fill(inverseTime_.begin() + blockSize, inverseTime_.end(), 0.0f);
        if (updateMetrics) {
            float energy = 0.0f;
            std::size_t peak = 0;
            float peakValue = 0.0f;
            for (std::size_t index = 0; index < blockSize; ++index) {
                const float value = inverseTime_[index];
                energy += value * value;
                if (std::abs(value) > peakValue) {
                    peakValue = std::abs(value);
                    peak = index;
                }
            }
            partitionEnergy_[channel][partition] = energy;
            partitionPeak_[channel][partition] = peak;
        }
        forward(inverseTime_.data(), filterReal[channel].data() + offset,
                filterImaginary[channel].data() + offset);
    }
    if (updateMetrics) updateDelayEstimate(partition);
}

bool EchoCanceller::nonlinearStreamEnabled(std::size_t stream) const {
    if (profile_ == EchoProfile::quality) return false;
    if (profile_ == EchoProfile::adaptive) return true;
    return profile_ == EchoProfile::strong || stream >= 2u;
}

void EchoCanceller::synthesizeNonlinearEcho(float* outputReal, float* outputImaginary) {
    std::fill_n(outputReal, fftSize, 0.0f);
    std::fill_n(outputImaginary, fftSize, 0.0f);
    DSPSplitComplex product{workReal_.data(), workImaginary_.data()};
    for (std::size_t stream = 0; stream < 4; ++stream) {
        if (!nonlinearStreamEnabled(stream)) continue;
        for (std::size_t partition = 0; partition < nonlinearPartitions; ++partition) {
            const std::size_t history =
                (nonlinearHistoryPosition_ + nonlinearPartitions - partition) %
                nonlinearPartitions;
            const std::size_t xOffset = history * fftSize;
            const std::size_t hOffset = partition * fftSize;
            DSPSplitComplex reference{
                nonlinearReferenceReal_[stream].data() + xOffset,
                nonlinearReferenceImaginary_[stream].data() + xOffset};
            DSPSplitComplex filter{
                nonlinearFilterReal_[stream].data() + hOffset,
                nonlinearFilterImaginary_[stream].data() + hOffset};
            vDSP_zvmul(&filter, 1, &reference, 1, &product, 1, fftSize, 1);
            vDSP_vadd(workReal_.data(), 1, outputReal, 1,
                      outputReal, 1, fftSize);
            vDSP_vadd(workImaginary_.data(), 1, outputImaginary, 1,
                      outputImaginary, 1, fftSize);
        }
    }
}

void EchoCanceller::adaptNonlinearFilter(const float* errorReal,
                                         const float* errorImaginary,
                                         float step) {
    if (step <= 0.0f) return;
    for (std::size_t stream = 0; stream < 4; ++stream) {
        if (!nonlinearStreamEnabled(stream)) continue;
        for (std::size_t partition = 0; partition < nonlinearPartitions; ++partition) {
            const std::size_t history =
                (nonlinearHistoryPosition_ + nonlinearPartitions - partition) %
                nonlinearPartitions;
            const std::size_t xOffset = history * fftSize;
            const std::size_t hOffset = partition * fftSize;
            for (std::size_t bin = 0; bin < spectrumBins; ++bin) {
                const float xr = nonlinearReferenceReal_[stream][xOffset + bin];
                const float xi = nonlinearReferenceImaginary_[stream][xOffset + bin];
                const float er = errorReal[bin];
                const float ei = errorImaginary[bin];
                const float scale = step / (nonlinearPower_[stream][bin] + 1.0f);
                const float deltaReal = scale * (xr * er + xi * ei);
                const float deltaImaginary = scale * (xr * ei - xi * er);
                nonlinearFilterReal_[stream][hOffset + bin] += deltaReal;
                nonlinearFilterImaginary_[stream][hOffset + bin] += deltaImaginary;
                if (bin > 0u && bin < fftSize / 2u) {
                    const std::size_t mirror = fftSize - bin;
                    nonlinearFilterReal_[stream][hOffset + mirror] += deltaReal;
                    nonlinearFilterImaginary_[stream][hOffset + mirror] -= deltaImaginary;
                }
            }
        }
    }
}

void EchoCanceller::constrainNonlinearPartition(std::size_t partition) {
    for (std::size_t stream = 0; stream < 4; ++stream) {
        if (!nonlinearStreamEnabled(stream)) continue;
        const std::size_t offset = partition * fftSize;
        inverse(nonlinearFilterReal_[stream].data() + offset,
                nonlinearFilterImaginary_[stream].data() + offset,
                inverseTime_.data());
        std::fill(inverseTime_.begin() + blockSize, inverseTime_.end(), 0.0f);
        forward(inverseTime_.data(), nonlinearFilterReal_[stream].data() + offset,
                nonlinearFilterImaginary_[stream].data() + offset);
    }
}

void EchoCanceller::updateDelayEstimate(std::size_t partition) {
    (void)partition;
    float bestEnergy = 0.0f;
    std::size_t bestPartition = 0;
    std::size_t bestPeak = 0;
    for (std::size_t index = 0; index < partitions; ++index) {
        const float energy = partitionEnergy_[0][index] + partitionEnergy_[1][index];
        if (energy > bestEnergy) {
            bestEnergy = energy;
            bestPartition = index;
            bestPeak = partitionEnergy_[0][index] >= partitionEnergy_[1][index]
                ? partitionPeak_[0][index] : partitionPeak_[1][index];
        }
    }
    const double milliseconds = 1000.0 *
        static_cast<double>(bestPartition * blockSize + bestPeak) / sampleRate;
    estimatedDelayMs_.store(static_cast<float>(milliseconds), std::memory_order_relaxed);
}

void EchoCanceller::updateSpectralState(const float* microphone, const float* error,
                                        float microphoneEnergy, float echoEnergy,
                                        float errorEnergy, bool converged) {
    float nearEndWeight = 0.0f;
    float analyzedWeight = 0.0f;
    const bool steadySpectralModel = converged && processedBlocks_ >= 750u &&
        linearReductionDB_.load(std::memory_order_relaxed) >= 8.0f &&
        trackingBlocks_ == 0u;
    const bool analyzeSpectrum = !steadySpectralModel ||
        processedBlocks_ % 4u == 0u;
    if (analyzeSpectrum) {
        std::fill(fftInput_.begin(), fftInput_.begin() + blockSize, 0.0f);
        std::memcpy(fftInput_.data() + blockSize, error, blockSize * sizeof(float));
        forward(fftInput_.data(), errorSpectrumReal_.data(),
                errorSpectrumImaginary_.data());
        std::fill(fftInput_.begin(), fftInput_.begin() + blockSize, 0.0f);
        std::memcpy(fftInput_.data() + blockSize, microphone, blockSize * sizeof(float));
        forward(fftInput_.data(), microphoneSpectrumReal_.data(),
                microphoneSpectrumImaginary_.data());
        for (std::size_t bin = 0; bin < spectrumBins; ++bin) {
            const float yr = microphoneSpectrumReal_[bin];
            const float yi = microphoneSpectrumImaginary_[bin];
            const float er = errorSpectrumReal_[bin];
            const float ei = errorSpectrumImaginary_[bin];
            const float sr = yr - er;
            const float si = yi - ei;
            const float y2 = yr * yr + yi * yi;
            const float e2 = er * er + ei * ei;
            const float s2 = sr * sr + si * si;
            const float crossReal = yr * sr + yi * si;
            const float crossImaginary = yi * sr - yr * si;
            microphonePower_[bin] = 0.72f * microphonePower_[bin] + 0.28f * y2;
            errorPower_[bin] = 0.72f * errorPower_[bin] + 0.28f * e2;
            echoPower_[bin] = 0.72f * echoPower_[bin] + 0.28f * s2;
            microphoneEchoCrossReal_[bin] =
                0.72f * microphoneEchoCrossReal_[bin] + 0.28f * crossReal;
            microphoneEchoCrossImaginary_[bin] =
                0.72f * microphoneEchoCrossImaginary_[bin] + 0.28f * crossImaginary;
            lateEchoPower_[bin] = std::max(0.88f * lateEchoPower_[bin], 0.08f * s2);

            const float cross2 =
                microphoneEchoCrossReal_[bin] * microphoneEchoCrossReal_[bin] +
                microphoneEchoCrossImaginary_[bin] * microphoneEchoCrossImaginary_[bin];
            const float coherence = cross2 /
                std::max(microphonePower_[bin] * echoPower_[bin], 1e-12f);
            const bool nearEnd = coherence < 0.35f &&
                microphonePower_[bin] > echoPower_[bin] * 1.25f &&
                errorPower_[bin] > echoPower_[bin] * 0.20f;
            nearEndProbability_[bin] = 0.67f * nearEndProbability_[bin] +
                0.33f * (nearEnd ? 1.0f : 0.0f);
            if (bin >= 2u && bin <= 86u) {
                const float weight = microphonePower_[bin];
                analyzedWeight += weight;
                nearEndWeight += weight * nearEndProbability_[bin];
            }
        }
        lastNearEndShare_ = nearEndWeight / std::max(analyzedWeight, 1e-12f);
    }

    const float modelReductionDB = 10.0f * std::log10(
        (microphoneEnergy + 1e-20f) / (errorEnergy + 1e-20f));
    const float nearEndShare = lastNearEndShare_;
    const bool modelIsUseful = echoEnergy > microphoneEnergy * 0.02f;
    // A coherent render transient may briefly raise the residual, but it does
    // not create sustained energy in bins that are incoherent with the echo
    // estimate.  Use that spectral distinction as the primary gate so a
    // strong near-end onset is protected before it can contaminate the filter.
    const bool learnedLongEnough = processedBlocks_ >= 375u;
    const bool nearEndDominates = microphoneEnergy > echoEnergy * 1.20f &&
        errorEnergy > echoEnergy * 0.15f;
    const float establishedLinearReduction =
        linearReductionDB_.load(std::memory_order_relaxed);
    const bool reliableModel = establishedLinearReduction >= 8.0f &&
        bestFarEndReductionDB_ >= 8.0f;
    const bool modelDropped = modelReductionDB < bestFarEndReductionDB_ - 3.0f;
    const bool spectralNearEnd = nearEndShare > 0.10f;
    const bool detectedDoubleTalkCandidate = learnedLongEnough && reliableModel &&
        modelIsUseful && modelDropped &&
        (spectralNearEnd || (nearEndDominates && nearEndShare > 0.04f));
    if (detectedDoubleTalkCandidate) {
        doubleTalkCandidateBlocks_ =
            std::min<std::size_t>(2u, doubleTalkCandidateBlocks_ + 1u);
    } else {
        doubleTalkCandidateBlocks_ = 0u;
    }
    if (doubleTalkCandidateBlocks_ >= 2u) {
        doubleTalkHangover_ = 48u;
    } else if (doubleTalkHangover_ > 0u) {
        --doubleTalkHangover_;
    }
    const bool doubleTalk = doubleTalkHangover_ > 0u;
    doubleTalk_.store(doubleTalk ? 1u : 0u, std::memory_order_relaxed);

    if (!doubleTalk && echoEnergy > 1e-10f) {
        bestFarEndReductionDB_ = std::max(bestFarEndReductionDB_ * 0.9998f,
                                          modelReductionDB);
        const float current = std::clamp(modelReductionDB, 0.0f, 80.0f);
        linearReductionDB_.store(establishedLinearReduction * 0.90f + current * 0.10f,
                                 std::memory_order_relaxed);
    }

    if (converged && !doubleTalk && !detectedDoubleTalkCandidate && modelIsUseful &&
        establishedLinearReduction >= 8.0f &&
        modelReductionDB < establishedLinearReduction - 8.0f) {
        ++pathDropBlocks_;
    } else if (pathDropBlocks_ > 0u) {
        --pathDropBlocks_;
    }
    if (pathDropBlocks_ >= 4u && trackingBlocks_ == 0u) {
        trackingBlocks_ = 375u;
        pathDropBlocks_ = 0u;
        shadowBetterEvaluations_ = 0u;
        pathChangeCount_.fetch_add(1u, std::memory_order_relaxed);
    }
    if (trackingBlocks_ > 0u) --trackingBlocks_;
    const EchoConvergenceState state = trackingBlocks_ > 0u
        ? EchoConvergenceState::tracking
        : (converged ? EchoConvergenceState::converged
                     : EchoConvergenceState::learning);
    convergenceState_.store(static_cast<std::uint32_t>(state),
                            std::memory_order_relaxed);
}

void EchoCanceller::applyResidualSuppression(const float* microphone,
                                             const float* error, float* output,
                                             bool farActive, bool doubleTalk,
                                             float microphoneEnergy,
                                             float errorEnergy) {
    std::memcpy(fftInput_.data(), previousError_.data(), blockSize * sizeof(float));
    std::memcpy(fftInput_.data() + blockSize, error, blockSize * sizeof(float));
    forward(fftInput_.data(), suppressionSpectrumReal_.data(),
            suppressionSpectrumImaginary_.data());

    const bool clipping = inputClipping_.load(std::memory_order_relaxed) != 0u;
    for (std::size_t bin = 0; bin < spectrumBins; ++bin) {
        float voiceProbability = nearEndProbability_[bin];
        if (bin > 0u) {
            voiceProbability = std::max(voiceProbability,
                                        nearEndProbability_[bin - 1u]);
        }
        if (bin + 1u < spectrumBins) {
            voiceProbability = std::max(voiceProbability,
                                        nearEndProbability_[bin + 1u]);
        }
        if (profile_ == EchoProfile::adaptive && bin > 1u) {
            voiceProbability = std::max(voiceProbability,
                                        nearEndProbability_[bin - 2u]);
        }
        if (profile_ == EchoProfile::adaptive && bin + 2u < spectrumBins) {
            voiceProbability = std::max(voiceProbability,
                                        nearEndProbability_[bin + 2u]);
        }
        const float normalFloor = minimumGain(false);
        const float protectedVoiceFloor = 1.0f;
        const float voiceThreshold = profile_ == EchoProfile::adaptive ? 0.05f : 0.08f;
        const float voiceProtection = doubleTalk && voiceProbability >= voiceThreshold
            ? 1.0f : 0.0f;
        // Double-talk protection is spectral, not global.  A near-end voice
        // bin gets the profile's voice floor while render-only bins retain the
        // full residual-echo suppression.  The old global floor was why music
        // remained audible and masked the talker in balanced/strong modes.
        const float floor = normalFloor +
            voiceProtection * (protectedVoiceFloor - normalFloor);
        const float profileStrength = profile_ == EchoProfile::quality ? 8.0f : 2048.0f;
        const float residualEcho =
            profileStrength * (echoPower_[bin] + lateEchoPower_[bin]);
        const float ratio = std::sqrt(errorPower_[bin] /
            std::max(errorPower_[bin] + residualEcho, 1e-12f));
        targetGain_[bin] = farActive && !clipping &&
                !(doubleTalk && profile_ == EchoProfile::quality)
            ? std::clamp(std::max(floor, ratio), floor, 1.0f)
            : 1.0f;
    }
    for (std::size_t bin = 0; bin < spectrumBins; ++bin) {
        const float previous = targetGain_[bin == 0u ? 0u : bin - 1u];
        const float next = targetGain_[bin + 1u < spectrumBins ? bin + 1u : bin];
        frequencySmoothedGain_[bin] = doubleTalk ? targetGain_[bin] :
            (0.25f * previous + 0.50f * targetGain_[bin] + 0.25f * next);
        const float smoothing = frequencySmoothedGain_[bin] < residualGain_[bin]
            ? 0.35f : (doubleTalk ? 0.65f : 0.04f);
        residualGain_[bin] += smoothing *
            (frequencySmoothedGain_[bin] - residualGain_[bin]);
        suppressionSpectrumReal_[bin] *= residualGain_[bin];
        suppressionSpectrumImaginary_[bin] *= residualGain_[bin];
        if (bin > 0u && bin < fftSize / 2u) {
            const std::size_t mirror = fftSize - bin;
            suppressionSpectrumReal_[mirror] *= residualGain_[bin];
            suppressionSpectrumImaginary_[mirror] *= residualGain_[bin];
        }
    }
    inverse(suppressionSpectrumReal_.data(), suppressionSpectrumImaginary_.data(),
            suppressedTime_.data());
    std::memcpy(previousError_.data(), error, blockSize * sizeof(float));

    constexpr float mixStep = 1.0f / 480.0f;
    float processedEnergy = 0.0f;
    for (std::size_t index = 0; index < blockSize; ++index) {
        activeMix_ = farActive ? std::min(1.0f, activeMix_ + mixStep)
                               : std::max(0.0f, activeMix_ - mixStep);
        const float processed = suppressedTime_[blockSize + index];
        output[index] = microphone[index] + activeMix_ * (processed - microphone[index]);
        processedEnergy += processed * processed;
    }
    processedEnergy /= static_cast<float>(blockSize);

    if (farActive && !doubleTalk && microphoneEnergy > 1e-12f) {
        const float residualDB = std::clamp(10.0f * std::log10(
            (errorEnergy + 1e-12f) / (processedEnergy + 1e-12f)), 0.0f, 80.0f);
        const float totalDB = std::clamp(10.0f * std::log10(
            (microphoneEnergy + 1e-12f) / (processedEnergy + 1e-12f)), 0.0f, 80.0f);
        residualSuppressionDB_.store(
            residualSuppressionDB_.load(std::memory_order_relaxed) * 0.90f +
                residualDB * 0.10f,
            std::memory_order_relaxed);
        reductionDB_.store(reductionDB_.load(std::memory_order_relaxed) * 0.90f +
                               totalDB * 0.10f,
                           std::memory_order_relaxed);
    }
}

void EchoCanceller::processBlock(const float* microphone, const float* referenceLeft,
                                 const float* referenceRight, float* output) {
    const EchoProfile requested = requestedProfile_.load(std::memory_order_acquire);
    if (requested != profile_) {
        profile_ = requested;
        if (profile_ == EchoProfile::quality) {
            nonlinearAccepted_ = false;
            nonlinearBetterBlocks_ = 0u;
            nonlinearWorseBlocks_ = 0u;
        }
    }

    constexpr float inverseSquareRootTwo = 0.7071067811865475f;
    float farEnergy = 0.0f;
    float microphoneEnergy = 0.0f;
    float microphonePeak = 0.0f;
    std::array<float, 2> blockInputPower{};
    for (std::size_t index = 0; index < blockSize; ++index) {
        const float left = referenceLeft[index];
        const float right = referenceRight[index];
        referenceBlock_[0][index] = (left + right) * inverseSquareRootTwo;
        referenceBlock_[1][index] = (left - right) * inverseSquareRootTwo;
        farEnergy += 0.5f * (left * left + right * right);
        blockInputPower[0] += left * left;
        blockInputPower[1] += right * right;
        microphoneEnergy += microphone[index] * microphone[index];
        microphonePeak = std::max(microphonePeak, std::abs(microphone[index]));
    }
    farEnergy /= static_cast<float>(blockSize);
    microphoneEnergy /= static_cast<float>(blockSize);
    referenceLevelDBFS_.store(std::clamp(10.0f * std::log10(farEnergy + 1e-12f),
                                         -120.0f, 6.0f),
                              std::memory_order_relaxed);
    microphoneLevelDBFS_.store(std::clamp(10.0f * std::log10(
                                           microphoneEnergy + 1e-12f),
                                           -120.0f, 6.0f),
                               std::memory_order_relaxed);
    const bool currentFarActive = farEnergy > 1e-9f;
    if (currentFarActive) {
        // A room keeps returning the render signal after the current output
        // block becomes quiet. Keep cancellation alive for the complete
        // modeled 256 ms path so music gaps do not expose echo tails.
        farEndHangoverBlocks_ = partitions;
    } else if (farEndHangoverBlocks_ > 0u) {
        --farEndHangoverBlocks_;
    }
    const bool farActive = currentFarActive || farEndHangoverBlocks_ > 0u;
    const bool inputClipping = microphonePeak >= 0.9440609f;
    inputClipping_.store(inputClipping ? 1u : 0u, std::memory_order_relaxed);
    active_.store(farActive ? 1u : 0u, std::memory_order_relaxed);

    for (std::size_t channel = 0; channel < 2; ++channel) {
        std::memcpy(fftInput_.data(), previousReference_[channel].data(),
                    blockSize * sizeof(float));
        std::memcpy(fftInput_.data() + blockSize, referenceBlock_[channel].data(),
                    blockSize * sizeof(float));
        const std::size_t offset = spectrumOffset(historyPosition_);
        forward(fftInput_.data(), referenceReal_[channel].data() + offset,
                referenceImaginary_[channel].data() + offset);
        std::memcpy(previousReference_[channel].data(), referenceBlock_[channel].data(),
                    blockSize * sizeof(float));
    }
    const bool stableLinearModel = processedBlocks_ >= 750u &&
        linearReductionDB_.load(std::memory_order_relaxed) >= 8.0f &&
        trackingBlocks_ == 0u;
    const bool fullRateAdaptation = !stableLinearModel ||
        processedBlocks_ % 4u == 0u;
    updateReferenceStatistics();

    if (profile_ != EchoProfile::quality) {
        const float* physicalReference[2] = {referenceLeft, referenceRight};
        for (std::size_t channel = 0; channel < 2; ++channel) {
            const float instantaneousPower = blockInputPower[channel] /
                static_cast<float>(blockSize);
            nonlinearInputPower_[channel] = 0.98f * nonlinearInputPower_[channel] +
                0.02f * instantaneousPower;
            const float variance = std::max(nonlinearInputPower_[channel], 1e-6f);
            const float quadraticScale = 1.0f / std::sqrt(2.0f * variance);
            const float cubicScale = 1.0f / (std::sqrt(6.0f) * variance);
            for (std::size_t index = 0; index < blockSize; ++index) {
                const float sample = physicalReference[channel][index];
                nonlinearReferenceBlock_[channel][index] = std::clamp(
                    (sample * sample - variance) * quadraticScale, -2.0f, 2.0f);
                nonlinearReferenceBlock_[channel + 2u][index] = std::clamp(
                    (sample * sample * sample - 3.0f * variance * sample) * cubicScale,
                    -2.0f, 2.0f);
            }
        }
        for (std::size_t stream = 0; stream < 4; ++stream) {
            if (!nonlinearStreamEnabled(stream)) continue;
            std::memcpy(fftInput_.data(), previousNonlinearReference_[stream].data(),
                        blockSize * sizeof(float));
            std::memcpy(fftInput_.data() + blockSize,
                        nonlinearReferenceBlock_[stream].data(),
                        blockSize * sizeof(float));
            const std::size_t offset = nonlinearHistoryPosition_ * fftSize;
            forward(fftInput_.data(), nonlinearReferenceReal_[stream].data() + offset,
                    nonlinearReferenceImaginary_[stream].data() + offset);
            std::memcpy(previousNonlinearReference_[stream].data(),
                        nonlinearReferenceBlock_[stream].data(),
                        blockSize * sizeof(float));
            for (std::size_t bin = 0; bin < fftSize; ++bin) {
                const float xr = nonlinearReferenceReal_[stream][offset + bin];
                const float xi = nonlinearReferenceImaginary_[stream][offset + bin];
                nonlinearPower_[stream][bin] = 0.90f * nonlinearPower_[stream][bin] +
                    0.10f * (xr * xr + xi * xi);
            }
        }
    }

    synthesizeLinearEcho(filterReal_, filterImaginary_,
                         linearSpectrumReal_.data(), linearSpectrumImaginary_.data());
    inverse(linearSpectrumReal_.data(), linearSpectrumImaginary_.data(),
            inverseTime_.data());
    float linearErrorEnergy = 0.0f;
    for (std::size_t index = 0; index < blockSize; ++index) {
        echo_[index] = inverseTime_[blockSize + index];
        linearError_[index] = microphone[index] - echo_[index];
        linearErrorEnergy += linearError_[index] * linearError_[index];
    }
    linearErrorEnergy /= static_cast<float>(blockSize);

    // The shadow normally sleeps.  Once a possible path change wakes it, run
    // it each block so it can distinguish a new speaker path (reference-
    // correlated and learnable) from a near-end talker (not learnable).
    const bool evaluateShadow = shadowTrackingActive_ || promotionSecondBlock_;
    const bool scoreShadow = processedBlocks_ % 4u == 0u || promotionSecondBlock_;
    float shadowErrorEnergy = linearErrorEnergy;
    float shadowEchoEnergy = 0.0f;
    if (evaluateShadow) {
        synthesizeLinearEcho(shadowFilterReal_, shadowFilterImaginary_,
                             shadowSpectrumReal_.data(), shadowSpectrumImaginary_.data());
        inverse(shadowSpectrumReal_.data(), shadowSpectrumImaginary_.data(),
                inverseTime_.data());
        shadowErrorEnergy = 0.0f;
        for (std::size_t index = 0; index < blockSize; ++index) {
            shadowEcho_[index] = inverseTime_[blockSize + index];
            shadowError_[index] = microphone[index] - shadowEcho_[index];
            shadowErrorEnergy += shadowError_[index] * shadowError_[index];
            shadowEchoEnergy += shadowEcho_[index] * shadowEcho_[index];
        }
        shadowErrorEnergy /= static_cast<float>(blockSize);
        shadowEchoEnergy /= static_cast<float>(blockSize);
    }

    const bool converged = processedBlocks_ >= 375u &&
        bestFarEndReductionDB_ >= 6.0f;
    const bool previousDoubleTalk = doubleTalk_.load(std::memory_order_relaxed) != 0u;
    bool promoteShadow = false;
    if (evaluateShadow && scoreShadow && !promotionSecondBlock_ && converged && farActive &&
        !previousDoubleTalk && !inputClipping) {
        if (shadowErrorEnergy < linearErrorEnergy * 0.5011872f) {
            ++shadowBetterEvaluations_;
        } else {
            shadowBetterEvaluations_ = 0u;
        }
        promoteShadow = shadowBetterEvaluations_ >= 12u;
    }

    if (promotionSecondBlock_ && evaluateShadow) {
        for (std::size_t index = 0; index < blockSize; ++index) {
            const float mix = 0.5f + 0.5f * static_cast<float>(index + 1u) /
                static_cast<float>(blockSize);
            linearError_[index] = shadowError_[index] * (1.0f - mix) +
                linearError_[index] * mix;
        }
        promotionSecondBlock_ = false;
    } else if (promoteShadow) {
        for (std::size_t index = 0; index < blockSize; ++index) {
            const float mix = 0.5f * static_cast<float>(index + 1u) /
                static_cast<float>(blockSize);
            linearError_[index] = linearError_[index] * (1.0f - mix) +
                shadowError_[index] * mix;
        }
    }

    if (profile_ != EchoProfile::quality) {
        synthesizeNonlinearEcho(nonlinearSpectrumReal_.data(),
                                nonlinearSpectrumImaginary_.data());
        inverse(nonlinearSpectrumReal_.data(), nonlinearSpectrumImaginary_.data(),
                inverseTime_.data());
        float candidateEnergy = 0.0f;
        float baseEnergy = 0.0f;
        for (std::size_t index = 0; index < blockSize; ++index) {
            nonlinearEcho_[index] = inverseTime_[blockSize + index];
            nonlinearCandidateError_[index] = linearError_[index] - nonlinearEcho_[index];
            candidateEnergy += nonlinearCandidateError_[index] *
                nonlinearCandidateError_[index];
            baseEnergy += linearError_[index] * linearError_[index];
        }
        if (converged && farActive && !previousDoubleTalk && !inputClipping) {
            // A 0.5% win accepted ordinary stereo correlation as a nonlinear
            // loudspeaker path.  Require a sustained material improvement so
            // the auxiliary branch cannot over-fit music and later subtract
            // an unrelated near-end voice.
            if (candidateEnergy < baseEnergy * 0.97f) {
                ++nonlinearBetterBlocks_;
                nonlinearWorseBlocks_ = 0u;
            } else if (candidateEnergy > baseEnergy * 1.02f) {
                ++nonlinearWorseBlocks_;
                nonlinearBetterBlocks_ = 0u;
            }
            if (nonlinearBetterBlocks_ >= 24u) nonlinearAccepted_ = true;
            if (nonlinearWorseBlocks_ >= 8u) nonlinearAccepted_ = false;
        }
    } else {
        nonlinearAccepted_ = false;
    }

    float errorEnergy = 0.0f;
    float echoEnergy = 0.0f;
    for (std::size_t index = 0; index < blockSize; ++index) {
        error_[index] = nonlinearAccepted_
            ? nonlinearCandidateError_[index] : linearError_[index];
        const float modeledEcho = microphone[index] - error_[index];
        errorEnergy += error_[index] * error_[index];
        echoEnergy += modeledEcho * modeledEcho;
    }
    errorEnergy /= static_cast<float>(blockSize);
    echoEnergy /= static_cast<float>(blockSize);
    nonlinearActive_.store(nonlinearAccepted_ ? 1u : 0u, std::memory_order_relaxed);

    const bool shadowExplainsPath = evaluateShadow &&
        shadowErrorEnergy < errorEnergy * 0.80f &&
        shadowEchoEnergy > microphoneEnergy * 0.02f;
    updateSpectralState(microphone,
                        shadowExplainsPath ? shadowError_.data() : error_.data(),
                        microphoneEnergy,
                        shadowExplainsPath ? shadowEchoEnergy : echoEnergy,
                        shadowExplainsPath ? shadowErrorEnergy : errorEnergy,
                        converged);
    const bool doubleTalk = doubleTalk_.load(std::memory_order_relaxed) != 0u;

    if (trackingBlocks_ > 0u && !shadowTrackingActive_ && !promotionSecondBlock_) {
        for (std::size_t channel = 0; channel < 2; ++channel) {
            std::copy(filterReal_[channel].begin(), filterReal_[channel].end(),
                      shadowFilterReal_[channel].begin());
            std::copy(filterImaginary_[channel].begin(), filterImaginary_[channel].end(),
                      shadowFilterImaginary_[channel].begin());
        }
        shadowTrackingActive_ = true;
        shadowBetterEvaluations_ = 0u;
    } else if (trackingBlocks_ == 0u && !promotionSecondBlock_) {
        shadowTrackingActive_ = false;
    }

    if (farActive && fullRateAdaptation) {
        // Keep only a conservative foreground step during double-talk.  A
        // full-rate update erodes the talker, while a complete freeze cannot
        // follow a moved loudspeaker.  The separately validated shadow path
        // remains responsible for fast recovery.
        const bool adaptiveProfile = profile_ == EchoProfile::adaptive;
        const float mainStep = inputClipping || shadowExplainsPath ? 0.0f :
            (adaptiveProfile ? (doubleTalk ? 0.005f : 0.08f)
                             : (doubleTalk ? 0.020f : 0.30f));
        // The shadow filter never reaches the output while double-talk is
        // active, so it may keep a guarded path-tracking step.  This lets a
        // real speaker movement recover without teaching the foreground
        // filter the near-end voice.
        const float shadowStep = inputClipping || !evaluateShadow ? 0.0f :
            (adaptiveProfile
                ? (doubleTalk ? 0.025f : (trackingBlocks_ > 0u ? 0.30f : 0.18f))
                : (doubleTalk ? 0.10f : (trackingBlocks_ > 0u ? 0.60f : 0.40f)));
        adaptLinearFilter(filterReal_, filterImaginary_, errorSpectrumReal_.data(),
                          errorSpectrumImaginary_.data(), mainStep);
        adaptLinearFilter(shadowFilterReal_, shadowFilterImaginary_,
                          errorSpectrumReal_.data(), errorSpectrumImaginary_.data(),
                          shadowStep);
        if (profile_ != EchoProfile::quality) {
            const float nonlinearStep = inputClipping || doubleTalk ? 0.0f :
                (profile_ == EchoProfile::adaptive ? 0.006f :
                 (profile_ == EchoProfile::strong ? 0.008f : 0.010f));
            std::fill(fftInput_.begin(), fftInput_.begin() + blockSize, 0.0f);
            std::memcpy(fftInput_.data() + blockSize,
                        nonlinearCandidateError_.data(), blockSize * sizeof(float));
            forward(fftInput_.data(), nonlinearSpectrumReal_.data(),
                    nonlinearSpectrumImaginary_.data());
            adaptNonlinearFilter(nonlinearSpectrumReal_.data(),
                                 nonlinearSpectrumImaginary_.data(), nonlinearStep);
        }
    }

    if (fullRateAdaptation) {
        constrainLinearPartition(filterReal_, filterImaginary_, constraintPosition_, true);
        constraintPosition_ = (constraintPosition_ + 1u) % partitions;
    }
    if (evaluateShadow) {
        constrainLinearPartition(shadowFilterReal_, shadowFilterImaginary_,
                                 shadowConstraintPosition_, false);
        shadowConstraintPosition_ = (shadowConstraintPosition_ + 1u) % partitions;
    }
    const std::size_t nonlinearConstraintInterval = stableLinearModel ? 8u : 4u;
    if (profile_ != EchoProfile::quality &&
        processedBlocks_ % nonlinearConstraintInterval == 0u) {
        constrainNonlinearPartition(nonlinearConstraintPosition_);
        nonlinearConstraintPosition_ =
            (nonlinearConstraintPosition_ + 1u) % nonlinearPartitions;
    }

    const bool finiteModel = std::isfinite(linearErrorEnergy) &&
        std::isfinite(errorEnergy) && std::isfinite(echoEnergy);
    const bool boundedModel = errorEnergy <= microphoneEnergy * 4.0f + 1e-8f &&
        echoEnergy <= microphoneEnergy * 9.0f + 1e-8f;
    // A nearby talker can dominate the block energy even when the learned
    // echo estimate remains correct. Requiring a 5% whole-block improvement
    // in that case repeatedly exposed raw speaker audio between syllables.
    // Keep the established path during detected double-talk; spectral voice
    // protection below still preserves the independent nearby source.
    const bool usefulModel = bestFarEndReductionDB_ >= 3.0f &&
        (linearErrorEnergy < microphoneEnergy * 0.95f || (converged && doubleTalk));
    if (finiteModel && boundedModel && usefulModel) {
        applyResidualSuppression(microphone, error_.data(), output, farActive,
                                 doubleTalk, microphoneEnergy, errorEnergy);
    } else if (finiteModel && boundedModel && converged && !inputClipping) {
        linearOnlyBlocks_.fetch_add(1u, std::memory_order_relaxed);
        // A quiet echo can improve the whole microphone block by less than
        // 5% when nearby speech dominates. Retain the bounded learned linear
        // subtraction, but do not apply a suppressor without voice confidence.
        // Chance correlation with nearby speech can make even a correct
        // echo estimate raise one short block's energy. Limit the subtraction
        // continuously to a 10% energy increase instead of switching to raw.
        float estimateEnergy = 0.0f, cross = 0.0f;
        for (std::size_t i = 0; i < blockSize; ++i) {
            const float estimate = microphone[i] - linearError_[i];
            estimateEnergy += estimate * estimate;
            cross += microphone[i] * estimate;
        }
        estimateEnergy /= blockSize;
        cross /= blockSize;
        const float allowedIncrease = microphoneEnergy * 0.10f;
        const float subtraction = std::clamp((cross + std::sqrt(
            cross * cross + allowedIncrease * estimateEnergy)) /
            std::max(estimateEnergy, 1e-20f), 0.0f, 1.0f);
        float outputEnergy = 0.0f;
        for (std::size_t i = 0; i < blockSize; ++i) {
            output[i] = microphone[i] + subtraction * (linearError_[i] - microphone[i]);
            outputEnergy += output[i] * output[i];
        }
        outputEnergy /= blockSize;
        std::memcpy(previousError_.data(), output, blockSize * sizeof(float));
        std::fill(residualGain_.begin(), residualGain_.end(), 1.0f);
        activeMix_ = 1.0f;
        reductionDB_.store(std::max(0.0f, 10.0f * std::log10(
            (microphoneEnergy + 1e-20f) / (outputEnergy + 1e-20f))), std::memory_order_relaxed);
        residualSuppressionDB_.store(0.0f, std::memory_order_relaxed);
    } else {
        modelBypassBlocks_.fetch_add(1u, std::memory_order_relaxed);
        // Until the room model has demonstrated a real improvement, exposing
        // its residual would attenuate the talker or amplify an unstable
        // estimate.  Continue learning, but keep the public microphone exactly
        // equal to M2 input 1.
        std::memcpy(output, microphone, blockSize * sizeof(float));
        activeMix_ = 0.0f;
        reductionDB_.store(0.0f, std::memory_order_relaxed);
        residualSuppressionDB_.store(0.0f, std::memory_order_relaxed);
    }

    if (promoteShadow) {
        filterReal_.swap(shadowFilterReal_);
        filterImaginary_.swap(shadowFilterImaginary_);
        std::fill(partitionEnergy_[0].begin(), partitionEnergy_[0].end(), 0.0f);
        std::fill(partitionEnergy_[1].begin(), partitionEnergy_[1].end(), 0.0f);
        shadowBetterEvaluations_ = 0u;
        if (trackingBlocks_ == 0u) {
            pathChangeCount_.fetch_add(1u, std::memory_order_relaxed);
        }
        trackingBlocks_ = std::max<std::size_t>(trackingBlocks_, 375u);
        promotionSecondBlock_ = true;
    }

    historyPosition_ = (historyPosition_ + 1u) % partitions;
    nonlinearHistoryPosition_ =
        (nonlinearHistoryPosition_ + 1u) % nonlinearPartitions;
    ++processedBlocks_;
}

void EchoCanceller::process(const float* microphone, const float* referenceLeft,
                            const float* referenceRight, float* output,
                            std::size_t frameCount, bool referenceAvailable) {
    if (!microphone || !output || frameCount == 0u) return;
    if (!valid() || !referenceLeft || !referenceRight || !referenceAvailable) {
        if (output != microphone) std::memcpy(output, microphone, frameCount * sizeof(float));
        active_.store(0, std::memory_order_relaxed);
        doubleTalk_.store(0, std::memory_order_relaxed);
        nonlinearActive_.store(0, std::memory_order_relaxed);
        inputClipping_.store(0, std::memory_order_relaxed);
        activeMix_ = 0.0f;
        constexpr std::size_t resetThresholdFrames = sampleRate / 10u;
        missingReferenceFrames_ = std::min<std::size_t>(
            resetThresholdFrames, missingReferenceFrames_ + frameCount);
        referenceDiscontinuity_ = missingReferenceFrames_ >= resetThresholdFrames;
        return;
    }
    if (referenceDiscontinuity_) {
        reset();
    }
    missingReferenceFrames_ = 0;
    const std::size_t processed = frameCount - frameCount % blockSize;
    for (std::size_t offset = 0; offset < processed; offset += blockSize) {
        processBlock(microphone + offset, referenceLeft + offset,
                     referenceRight + offset, output + offset);
        float microphonePeak = 0.0f;
        float outputPeak = 0.0f;
        bool finite = true;
        for (std::size_t index = 0; index < blockSize; ++index) {
            microphonePeak = std::max(microphonePeak,
                                      std::abs(microphone[offset + index]));
            const float sample = output[offset + index];
            finite = finite && std::isfinite(sample);
            outputPeak = std::max(outputPeak, std::abs(sample));
        }
        const float safePeak = std::max(2.0f, microphonePeak * 4.0f);
        if (!finite || outputPeak > safePeak) {
            std::memcpy(output + offset, microphone + offset,
                        blockSize * sizeof(float));
            stabilityResetCount_.fetch_add(1u, std::memory_order_relaxed);
            reset();
        }
    }
    if (processed < frameCount) {
        std::memcpy(output + processed, microphone + processed,
                    (frameCount - processed) * sizeof(float));
    }
}

EchoMetrics EchoCanceller::metrics(std::uint64_t referenceUnderruns) const {
    EchoMetrics result;
    result.active = active_.load(std::memory_order_relaxed) != 0u;
    result.doubleTalk = doubleTalk_.load(std::memory_order_relaxed) != 0u;
    result.nonlinearActive = nonlinearActive_.load(std::memory_order_relaxed) != 0u;
    result.inputClipping = inputClipping_.load(std::memory_order_relaxed) != 0u;
    result.reductionDB = reductionDB_.load(std::memory_order_relaxed);
    result.linearReductionDB = linearReductionDB_.load(std::memory_order_relaxed);
    result.residualSuppressionDB =
        residualSuppressionDB_.load(std::memory_order_relaxed);
    result.estimatedDelayMs = estimatedDelayMs_.load(std::memory_order_relaxed);
    result.referenceLevelDBFS = referenceLevelDBFS_.load(std::memory_order_relaxed);
    result.microphoneLevelDBFS = microphoneLevelDBFS_.load(std::memory_order_relaxed);
    result.referenceUnderruns = referenceUnderruns;
    result.pathChangeCount = pathChangeCount_.load(std::memory_order_relaxed);
    result.stabilityResetCount = stabilityResetCount_.load(std::memory_order_relaxed);
    result.modelBypassBlocks = modelBypassBlocks_.load(std::memory_order_relaxed);
    result.linearOnlyBlocks = linearOnlyBlocks_.load(std::memory_order_relaxed);
    result.convergence = static_cast<EchoConvergenceState>(
        convergenceState_.load(std::memory_order_relaxed));
    return result;
}

}  // namespace macsound
