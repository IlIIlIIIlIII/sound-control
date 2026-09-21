#pragma once

#include "SignalOps.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace soundcontrol {

enum class EchoProfile {
    adaptive,
    // Retained for deterministic regression fixtures. The app migrates every
    // historical profile name to adaptive and exposes only that mode.
    quality,
    balanced,
    strong,
};

enum class EchoConvergenceState {
    learning,
    converged,
    tracking,
};

const char* echoProfileName(EchoProfile profile);
EchoProfile echoProfileFromName(const char* name);
const char* echoConvergenceName(EchoConvergenceState state);

struct EchoMetrics {
    bool active = false;
    bool doubleTalk = false;
    bool nonlinearActive = false;
    bool inputClipping = false;
    double reductionDB = 0.0;
    double linearReductionDB = 0.0;
    double residualSuppressionDB = 0.0;
    double estimatedDelayMs = 0.0;
    double referenceLevelDBFS = -120.0;
    double microphoneLevelDBFS = -120.0;
    std::uint64_t referenceUnderruns = 0;
    std::uint64_t pathChangeCount = 0;
    std::uint64_t stabilityResetCount = 0;
    std::uint64_t modelBypassBlocks = 0;
    std::uint64_t linearOnlyBlocks = 0;
    EchoConvergenceState convergence = EchoConvergenceState::learning;
};

// One writer (the selected output IOProc) and one reader (the M2 input IOProc).
// Samples stay at the output device's rate; the reader renders them on the M2
// 48 kHz host-time grid, so the audible signal is never resampled.
class StereoReferenceTimeline {
public:
    // 131072 source frames still covers more than 680 ms at 192 kHz. The
    // canceller owns the separate 256 ms echo history, so retaining several
    // seconds here only increases the resident set without improving AEC.
    static constexpr std::size_t capacity = 1u << 17;
    static constexpr std::size_t taps = 32;
    static constexpr std::size_t phases = 1024;

    StereoReferenceTimeline();
    StereoReferenceTimeline(const StereoReferenceTimeline&) = delete;
    StereoReferenceTimeline& operator=(const StereoReferenceTimeline&) = delete;

    void reset();
    void push(const float* left, const float* right, std::size_t frames,
              double nominalSampleRate, std::uint64_t firstHostTime,
              double hostTicksPerSecond);
    bool render(std::uint64_t firstHostTime, double hostTicksPerSecond,
                float* left, float* right, std::size_t frames);

    std::uint64_t underruns() const {
        return underruns_.load(std::memory_order_relaxed);
    }
    std::uint64_t generation() const {
        return generation_.load(std::memory_order_acquire);
    }
    double sourceSampleRate() const;

private:
    struct Anchor {
        std::uint64_t startIndex = 0;
        std::uint64_t hostTime = 0;
        double sampleRate = 0.0;
        std::uint64_t generation = 0;
    };

    bool loadAnchor(Anchor& anchor) const;
    const float* coefficientTable(double sourceRate) const;
    float interpolate(const std::vector<float>& ring, double position,
                      const float* table) const;

    std::vector<float> left_;
    std::vector<float> right_;
    std::array<std::vector<float>, 4> coefficients_;
    std::atomic<std::uint64_t> writeIndex_{0};
    std::atomic<std::uint64_t> sequence_{0};
    std::atomic<std::uint64_t> anchorStartIndex_{0};
    std::atomic<std::uint64_t> anchorHostTime_{0};
    std::atomic<std::uint64_t> anchorRateBits_{0};
    std::atomic<std::uint64_t> generation_{1};
    std::atomic<std::uint64_t> anchorGeneration_{0};
    std::atomic<std::uint64_t> underruns_{0};

    std::uint64_t previousStartIndex_ = 0;
    std::uint64_t previousHostTime_ = 0;
    double smoothedRate_ = 0.0;
    double previousNominalRate_ = 0.0;
};

class EchoCanceller {
public:
    static constexpr std::size_t sampleRate = 48000;
    static constexpr std::size_t blockSize = 256;
    static constexpr std::size_t fftSize = 512;
    static constexpr std::size_t spectrumBins = fftSize / 2 + 1;
    static constexpr std::size_t partitions = 48;
    static constexpr std::size_t nonlinearPartitions = 12;

    EchoCanceller();
    ~EchoCanceller();
    EchoCanceller(const EchoCanceller&) = delete;
    EchoCanceller& operator=(const EchoCanceller&) = delete;

    bool valid() const { return fftSetup_ != nullptr; }
    void reset();
    void setProfile(EchoProfile profile) {
        requestedProfile_.store(profile, std::memory_order_release);
    }
    EchoProfile profile() const {
        return requestedProfile_.load(std::memory_order_acquire);
    }

    // frameCount must be a multiple of blockSize. Other tail samples pass through
    // bit-exactly; the current M2 callback is 512 frames.
    void process(const float* microphone, const float* referenceLeft,
                 const float* referenceRight, float* output,
                 std::size_t frameCount, bool referenceAvailable);
    EchoMetrics metrics(std::uint64_t referenceUnderruns) const;

private:
    using StereoBank = std::array<std::vector<float>, 2>;
    using NonlinearBank = std::array<std::vector<float>, 4>;

    void forward(const float* time, float* real, float* imaginary);
    void inverse(const float* real, const float* imaginary, float* time);
    void processBlock(const float* microphone, const float* referenceLeft,
                      const float* referenceRight, float* output);
    void synthesizeLinearEcho(const StereoBank& filterReal,
                              const StereoBank& filterImaginary,
                              float* outputReal, float* outputImaginary);
    void adaptLinearFilter(StereoBank& filterReal, StereoBank& filterImaginary,
                           const float* errorReal, const float* errorImaginary,
                           float step);
    void constrainLinearPartition(StereoBank& filterReal,
                                  StereoBank& filterImaginary,
                                  std::size_t partition, bool updateMetrics);
    void synthesizeNonlinearEcho(float* outputReal, float* outputImaginary);
    void adaptNonlinearFilter(const float* errorReal, const float* errorImaginary,
                              float step);
    void constrainNonlinearPartition(std::size_t partition);
    void updateReferenceStatistics();
    void updateSpectralState(const float* microphone, const float* error,
                             float microphoneEnergy, float echoEnergy,
                             float errorEnergy, bool converged);
    void applyResidualSuppression(const float* microphone, const float* error,
                                  float* output, bool farActive, bool doubleTalk,
                                  float microphoneEnergy, float errorEnergy);
    bool nonlinearStreamEnabled(std::size_t stream) const;
    void updateDelayEstimate(std::size_t partition);
    float minimumGain(bool doubleTalk) const;

    signal::FFTSetup fftSetup_ = nullptr;
    EchoProfile profile_ = EchoProfile::adaptive;
    std::size_t weakModelBlocks_ = 0;
    std::atomic<EchoProfile> requestedProfile_{EchoProfile::adaptive};

    StereoBank referenceReal_;
    StereoBank referenceImaginary_;
    StereoBank filterReal_;
    StereoBank filterImaginary_;
    StereoBank shadowFilterReal_;
    StereoBank shadowFilterImaginary_;
    StereoBank previousReference_;
    StereoBank referenceBlock_;
    StereoBank partitionEnergy_;
    std::array<std::vector<std::size_t>, 2> partitionPeak_;

    NonlinearBank nonlinearReferenceReal_;
    NonlinearBank nonlinearReferenceImaginary_;
    NonlinearBank nonlinearFilterReal_;
    NonlinearBank nonlinearFilterImaginary_;
    NonlinearBank previousNonlinearReference_;
    NonlinearBank nonlinearReferenceBlock_;
    NonlinearBank nonlinearPower_;

    std::vector<float> fftInput_;
    std::vector<float> workReal_;
    std::vector<float> workImaginary_;
    std::vector<float> inverseReal_;
    std::vector<float> inverseImaginary_;
    std::vector<float> inverseTime_;
    std::vector<float> echo_;
    std::vector<float> shadowEcho_;
    std::vector<float> nonlinearEcho_;
    std::vector<float> linearError_;
    std::vector<float> shadowError_;
    std::vector<float> nonlinearCandidateError_;
    std::vector<float> error_;
    std::vector<float> previousError_;
    std::vector<float> suppressedTime_;

    std::vector<float> linearSpectrumReal_;
    std::vector<float> linearSpectrumImaginary_;
    std::vector<float> shadowSpectrumReal_;
    std::vector<float> shadowSpectrumImaginary_;
    std::vector<float> nonlinearSpectrumReal_;
    std::vector<float> nonlinearSpectrumImaginary_;
    std::vector<float> errorSpectrumReal_;
    std::vector<float> errorSpectrumImaginary_;
    std::vector<float> microphoneSpectrumReal_;
    std::vector<float> microphoneSpectrumImaginary_;
    std::vector<float> suppressionSpectrumReal_;
    std::vector<float> suppressionSpectrumImaginary_;

    std::vector<float> covarianceMid_;
    std::vector<float> covarianceSide_;
    std::vector<float> covarianceCrossReal_;
    std::vector<float> covarianceCrossImaginary_;
    std::vector<float> historyMidPower_;
    std::vector<float> historySidePower_;
    std::vector<float> historyCrossReal_;
    std::vector<float> historyCrossImaginary_;
    std::vector<float> referenceMidPowerSum_;
    std::vector<float> referenceSidePowerSum_;
    std::vector<float> referenceCrossRealSum_;
    std::vector<float> referenceCrossImaginarySum_;
    std::vector<float> inverseCovarianceMid_;
    std::vector<float> inverseCovarianceSide_;
    std::vector<float> inverseCovarianceCrossReal_;
    std::vector<float> inverseCovarianceCrossImaginary_;
    std::vector<float> microphonePower_;
    std::vector<float> echoPower_;
    std::vector<float> errorPower_;
    std::vector<float> microphoneEchoCrossReal_;
    std::vector<float> microphoneEchoCrossImaginary_;
    std::vector<float> nearEndProbability_;
    std::vector<float> residualGain_;
    std::vector<float> targetGain_;
    std::vector<float> frequencySmoothedGain_;
    std::vector<float> lateEchoPower_;
    std::size_t historyPosition_ = 0;
    std::size_t nonlinearHistoryPosition_ = 0;
    std::size_t constraintPosition_ = 0;
    std::size_t shadowConstraintPosition_ = 0;
    std::size_t nonlinearConstraintPosition_ = 0;
    std::uint64_t processedBlocks_ = 0;
    std::size_t doubleTalkCandidateBlocks_ = 0;
    std::size_t doubleTalkHangover_ = 0;
    std::size_t shadowBetterEvaluations_ = 0;
    std::size_t pathDropBlocks_ = 0;
    std::size_t trackingBlocks_ = 0;
    std::size_t nonlinearBetterBlocks_ = 0;
    std::size_t nonlinearWorseBlocks_ = 0;
    std::size_t farEndHangoverBlocks_ = 0;
    bool nonlinearAccepted_ = false;
    bool promotionSecondBlock_ = false;
    bool shadowTrackingActive_ = false;
    bool referenceDiscontinuity_ = false;
    std::size_t missingReferenceFrames_ = 0;
    std::array<float, 2> nonlinearInputPower_{1e-4f, 1e-4f};
    float lastNearEndShare_ = 0.0f;
    float bestFarEndReductionDB_ = 0.0f;
    float activeMix_ = 0.0f;
    std::atomic<std::uint32_t> active_{0};
    std::atomic<std::uint32_t> doubleTalk_{0};
    std::atomic<std::uint32_t> nonlinearActive_{0};
    std::atomic<std::uint32_t> inputClipping_{0};
    std::atomic<std::uint32_t> convergenceState_{
        static_cast<std::uint32_t>(EchoConvergenceState::learning)};
    std::atomic<float> reductionDB_{0.0f};
    std::atomic<float> linearReductionDB_{0.0f};
    std::atomic<float> residualSuppressionDB_{0.0f};
    std::atomic<float> estimatedDelayMs_{0.0f};
    std::atomic<float> referenceLevelDBFS_{-120.0f};
    std::atomic<float> microphoneLevelDBFS_{-120.0f};
    std::atomic<std::uint64_t> pathChangeCount_{0};
    std::atomic<std::uint64_t> stabilityResetCount_{0};
    std::atomic<std::uint64_t> modelBypassBlocks_{0};
    std::atomic<std::uint64_t> linearOnlyBlocks_{0};
};

}  // namespace soundcontrol
