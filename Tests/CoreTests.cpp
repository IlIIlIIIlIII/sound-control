#include "AEC.hpp"
#include "DSP.hpp"
#include "DisplayTools.hpp"
#include "MicShared.h"
#include "REWParser.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace macsound;

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

const char* leftText = R"(Notes:left
Configurable_PEQ
Number Enabled Control Type Frequency(Hz) Gain(dB) Q Bandwidth(Hz)
1 True Auto PK 66.00 -12.00 7.40 8.92
2 True Auto PK 140.0 -12.00 3.21 43.61
3 True Auto None
)";

const char* rightText = R"(Notes:R
Configurable_PEQ
Number Enabled Control Type Frequency(Hz) Gain(dB) Q Bandwidth(Hz)
1 True Auto PK 65.00 -8.40 8.00 8.12
2 True Auto PK 142.0 -12.00 3.36 42.26
)";

void parserTests() {
    const auto left = parseREWConfigurablePEQ(leftText, Channel::left);
    expect(static_cast<bool>(left), left.error);
    expect(left.filters.size() == 2, "left filter count");
    expect(left.filters[0] == PEQFilter{66.0, -12.0, 7.4}, "left first filter");

    const auto right = parseREWConfigurablePEQ(rightText, Channel::right);
    expect(static_cast<bool>(right), right.error);
    expect(right.filters.size() == 2, "right filter count");
    expect(right.filters[0] == PEQFilter{65.0, -8.4, 8.0}, "right first filter");

    const auto mismatch = parseREWConfigurablePEQ(leftText, Channel::right);
    expect(!mismatch, "channel mismatch must fail");

    const auto unsupported = parseREWConfigurablePEQ(
        "Notes:left\nConfigurable_PEQ\n1 True Auto LS 100 2 0.7\n", Channel::left);
    expect(!unsupported, "unsupported enabled type must fail");

    const auto malformed = parseREWConfigurablePEQ(
        "Notes:left\nConfigurable_PEQ\n1 True Auto PK nope 2 0.7\n", Channel::left);
    expect(!malformed, "malformed number must fail");
}

void importTests() {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("macsound-tests-" + std::to_string(getpid()));
    std::filesystem::create_directories(directory);
    const auto source = directory / "source.txt";
    const auto destination = directory / "left.txt";
    {
        std::ofstream file(source);
        file << leftText;
    }
    const auto imported = importREWConfigurablePEQFile(source, destination, Channel::left);
    expect(static_cast<bool>(imported), imported.error);
    expect(imported.filters.size() == 2, "valid import band count");
    {
        std::ifstream file(destination);
        std::ostringstream contents;
        contents << file.rdbuf();
        expect(contents.str() == leftText, "valid import preserves source bytes");
    }

    {
        std::ofstream file(source);
        file << "Configurable_PEQ\n1 True Auto PK bad -2 1\n";
    }
    const auto rejected = importREWConfigurablePEQFile(source, destination, Channel::left);
    expect(!rejected, "invalid replacement is rejected");
    {
        std::ifstream file(destination);
        std::ostringstream contents;
        contents << file.rdbuf();
        expect(contents.str() == leftText, "invalid replacement preserves last valid file");
    }
    std::filesystem::remove_all(directory);
}

void responseTests() {
    const std::vector<double> rates{44100.0, 48000.0, 96000.0, 192000.0};
    for (const auto rate : rates) {
        const PEQFilter filter{1000.0, 6.0, 2.0};
        expect(std::abs(responseDB({filter}, 1000.0, rate) - 6.0) < 1e-9,
               "center gain at " + std::to_string(rate));
    }

    const auto left = parseREWConfigurablePEQ(leftText, Channel::left);
    const auto right = parseREWConfigurablePEQ(rightText, Channel::right);
    expect(std::abs(automaticPreampDB(left.filters, right.filters, 48000.0)) < 1e-4,
           "cut-only filters need no headroom reduction");
    expect(automaticPreampDB({{1000.0, 6.0, 1.0}}, {{1000.0, 3.0, 1.0}}, 48000.0) < -5.99,
           "positive filters receive automatic headroom");
}

void processorTests() {
    std::string error;
    StereoDSP dsp;
    const std::vector<PEQFilter> flat{{1000.0, 0.0, 1.0}};
    expect(dsp.configure(flat, flat, 48000.0, error), error);

    std::vector<double> flatData{0.2, -0.3, 0.7, 0.1, -0.2, 0.9};
    const auto original = flatData;
    dsp.processInterleaved(flatData.data(), flatData.size() / 2);
    expect(std::memcmp(flatData.data(), original.data(), flatData.size() * sizeof(double)) == 0,
           "0 dB filter remains bit exact");

    StereoDSP split;
    expect(split.configure({{1000.0, -12.0, 2.0}}, flat, 48000.0, error), error);
    std::vector<double> stereo(2048 * 2, 0.0);
    for (std::size_t i = 0; i < 2048; ++i) {
        stereo[i * 2] = std::sin(2.0 * 3.141592653589793 * 1000.0 * i / 48000.0);
        stereo[i * 2 + 1] = static_cast<double>(i % 17) / 17.0;
    }
    std::vector<double> rightBefore(2048);
    for (std::size_t i = 0; i < 2048; ++i) rightBefore[i] = stereo[i * 2 + 1];
    split.processInterleaved(stereo.data(), 2048);
    bool rightExact = true;
    for (std::size_t i = 0; i < 2048; ++i) rightExact &= stereo[i * 2 + 1] == rightBefore[i];
    expect(rightExact, "left filter must not alter right channel");
}

void micRingTests() {
    auto shared = std::make_unique<MSMicSharedMemory>();
    std::memset(shared.get(), 0, sizeof(*shared));
    shared->magic = MS_MIC_SHARED_MAGIC;
    shared->version = MS_MIC_SHARED_VERSION;
    shared->capacity = MS_MIC_RING_CAPACITY;
    MSMicSetEngineOnline(shared.get(), true);

    MSMicPublishTimestamp(shared.get(), 1234.5, 987654321u);
    Float64 sampleTime = 0;
    uint64_t hostTime = 0;
    expect(MSMicSourceSequence(shared.get()) != 0u &&
               (MSMicSourceSequence(shared.get()) & 1u) == 0u,
           "mic source heartbeat publishes a stable sequence");
    expect(MSMicLoadTimestamp(shared.get(), &sampleTime, &hostTime) &&
               sampleTime == 1234.5 && hostTime == 987654321u,
           "mic source heartbeat preserves its timestamp");
    Float64 zeroSampleTime = 0.0;
    uint64_t zeroHostTime = 0u;
    expect(MSMicCalculateZeroTimestamp(1234.5, 987654321u, 512.0, 10.0,
                                       &zeroSampleTime, &zeroHostTime) &&
               zeroSampleTime == 1024.0 && zeroHostTime == 987652216u,
           "virtual mic clock anchors to the physical M2 source timestamp");
    expect(!MSMicCalculateZeroTimestamp(-1.0, 987654321u, 512.0, 10.0,
                                        &zeroSampleTime, &zeroHostTime),
           "virtual mic clock rejects an invalid source timestamp");

    const Float32 first[] = {0.125f, -0.25f, 0.5f, -1.0f};
    expect(MSMicWrite(shared.get(), first, 4) == 4, "mic ring writes all frames");
    Float32 output[6] = {};
    bool primed = false;
    expect(MSMicRead(shared.get(), output, 4, 0, &primed) == 4,
           "mic ring reads all frames");
    expect(std::memcmp(first, output, sizeof(first)) == 0,
           "mic ring preserves mono Float32 samples exactly");

    std::fill(std::begin(output), std::end(output), 1.0f);
    expect(MSMicRead(shared.get(), output, 6, 0, &primed) == 0,
           "empty mic ring reports underrun");
    expect(std::all_of(std::begin(output), std::end(output),
                       [](Float32 value) { return value == 0.0f; }),
           "mic underrun is zero-filled");

    shared->writeIndex = MS_MIC_RING_CAPACITY - 2;
    shared->readIndex = MS_MIC_RING_CAPACITY - 2;
    primed = false;
    expect(MSMicWrite(shared.get(), first, 4) == 4, "mic ring wraps on write");
    std::fill(std::begin(output), std::end(output), 0.0f);
    expect(MSMicRead(shared.get(), output, 4, 0, &primed) == 4,
           "mic ring wraps on read");
    expect(std::memcmp(first, output, sizeof(first)) == 0,
           "wrapped mic ring preserves ordering");

    MSMicResetReader(shared.get());
    primed = false;
    std::vector<Float32> prebuffer(MS_MIC_PREBUFFER_FRAMES, 0.75f);
    expect(MSMicWrite(shared.get(), prebuffer.data(), prebuffer.size() - 1) ==
               prebuffer.size() - 1,
           "mic prebuffer accepts initial frames");
    output[0] = 1.0f;
    expect(MSMicRead(shared.get(), output, 1, MS_MIC_PREBUFFER_FRAMES, &primed) == 0 &&
               output[0] == 0.0f && !primed,
           "mic stays silent until the jitter prebuffer is ready");
    expect(MSMicWrite(shared.get(), prebuffer.data(), 1) == 1,
           "mic prebuffer reaches threshold");
    expect(MSMicRead(shared.get(), output, 1, MS_MIC_PREBUFFER_FRAMES, &primed) == 1 &&
               output[0] == 0.75f && primed,
           "mic starts without losing the first buffered sample");

    MSMicResetReader(shared.get());
    primed = true;
    std::vector<Float32> excessive(MS_MIC_MAX_BUFFERED_FRAMES + 512u);
    for (std::size_t index = 0; index < excessive.size(); ++index) {
        excessive[index] = static_cast<Float32>(index);
    }
    expect(MSMicWrite(shared.get(), excessive.data(), excessive.size()) == excessive.size(),
           "mic ring accepts a temporary producer burst");
    output[0] = 0.0f;
    expect(MSMicRead(shared.get(), output, 1, MS_MIC_PREBUFFER_FRAMES, &primed) == 1 &&
               output[0] == static_cast<Float32>(excessive.size() - MS_MIC_PREBUFFER_FRAMES),
           "mic reader drops stale audio and retains only the low-latency prebuffer");
    expect(MSMicOverruns(shared.get()) == excessive.size() - MS_MIC_PREBUFFER_FRAMES,
           "mic latency guard accounts for discarded stale frames");

    MSMicSetEngineOnline(shared.get(), false);
    expect(!MSMicEngineOnline(shared.get()), "mic engine offline state is visible to driver");
    output[0] = 1.0f;
    expect(MSMicRead(shared.get(), output, 1, 0, &primed) == 0 && output[0] == 0.0f,
           "offline mic source always returns silence");
}

std::array<std::byte, 128> targetEDID() {
    std::array<std::byte, 128> bytes{};
    const std::uint8_t header[] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    for (std::size_t index = 0; index < sizeof(header); ++index) {
        bytes[index] = std::byte{header[index]};
    }
    bytes[8] = std::byte{0x0d};
    bytes[9] = std::byte{0xf3};
    bytes[10] = std::byte{0x50};
    bytes[11] = std::byte{0x31};
    bytes[12] = std::byte{0x01};
    bytes[126] = std::byte{0x00};
    unsigned sum = 0;
    for (std::size_t index = 0; index < 127; ++index) {
        sum += std::to_integer<std::uint8_t>(bytes[index]);
    }
    bytes[127] = std::byte{static_cast<std::uint8_t>((256 - (sum & 0xffu)) & 0xffu)};
    return bytes;
}

void displayCoreTests() {
    auto edid = targetEDID();
    std::string error;
    expect(mactools::display::validateEDID(edid, error), error);
    expect(mactools::display::isTarget32RTX950EDID(edid),
           "32RTX950 EDID identity is recognized");

    auto corrupt = edid;
    corrupt[20] = std::byte{0x01};
    expect(!mactools::display::validateEDID(corrupt, error),
           "bad EDID checksum is rejected");
    auto wrongExtensionCount = edid;
    wrongExtensionCount[126] = std::byte{0x01};
    expect(!mactools::display::validateEDID(wrongExtensionCount, error),
           "EDID extension length mismatch is rejected");

    using mactools::display::ReinitializeDisplay;
    const auto order = mactools::display::makeReinitializeOrder({
        {50, 0x1234, 0x5678, "unrelated"},
        {5, mactools::display::k32RTX950Vendor,
         mactools::display::k32RTX950Model,
         mactools::display::kHDMI32RTX950UUID},
        {3, mactools::display::kMO32U24Vendor,
         mactools::display::kMO32U24Model, "mo-b"},
        {2, mactools::display::k32RTX950Vendor,
         mactools::display::k32RTX950Model,
         mactools::display::kDock32RTX950UUID},
        {4, mactools::display::kMO32U24Vendor,
         mactools::display::kMO32U24Model, "mo-a"},
    });
    expect(order == std::vector<std::uint32_t>({4, 3, 2, 5, 50}),
           "display reinitialization prioritizes MO pair, dock 32RTX950, then HDMI 32RTX950");

    using mactools::display::DisplayGeometry;
    std::vector<DisplayGeometry> displays{
        {1, 0x0610, 1, 1, 0, 0, 1728, 1117, 0, true, true, false},
        {3, mactools::display::kMO32U24Vendor, mactools::display::kMO32U24Model,
         mactools::display::kMO32U24Serial, 0, -1440, 2560, 1440, 180,
         false, true, false},
        {4, mactools::display::kMO32U24Vendor, mactools::display::kMO32U24Model,
         mactools::display::kMO32U24Serial, 0, 0, 2560, 1440, 0,
         false, true, false},
    };
    const auto swap = mactools::display::makeMO32U24SwapPlan(displays, error);
    expect(swap.has_value(), error);
    expect(swap && swap->first.id == 3 && swap->first.x == 0 &&
               swap->first.y == 0 && swap->first.rotation == 0 &&
               swap->second.id == 4 && swap->second.x == 0 &&
               swap->second.y == -1440 && swap->second.rotation == 180,
           "MO32U24 position and rotation roles are swapped together");

    displays[2].online = false;
    expect(!mactools::display::makeMO32U24SwapPlan(displays, error),
           "swap refuses a missing MO32U24");
    displays[2].online = true;
    displays[2].mirrored = true;
    expect(!mactools::display::makeMO32U24SwapPlan(displays, error),
           "swap refuses a mirrored MO32U24");
    displays[2].mirrored = false;
    displays[2].width = 1920;
    expect(!mactools::display::makeMO32U24SwapPlan(displays, error),
           "swap refuses different logical sizes");
    displays[2].width = 2560;
    displays[2].rotation = 45;
    expect(!mactools::display::makeMO32U24SwapPlan(displays, error),
           "swap refuses an unsupported rotation");
}

double measuredGainDB(StereoDSP& dsp, double rate, double frequency, bool measureLeft) {
    const double intervalSeconds = std::max(0.5, 20.0 / frequency);
    const std::size_t intervalFrames =
        static_cast<std::size_t>(std::ceil(intervalSeconds * rate));
    const std::size_t frames = intervalFrames * 2;
    std::vector<double> leftSamples(frames, 0.0);
    std::vector<double> rightSamples(frames, 0.0);
    auto& input = measureLeft ? leftSamples : rightSamples;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        input[frame] = 0.25 * std::sin(2.0 * 3.141592653589793 * frequency * frame / rate);
    }

    double inputSquareSum = 0.0;
    for (std::size_t frame = intervalFrames; frame < frames; ++frame) {
        inputSquareSum += input[frame] * input[frame];
    }
    dsp.reset();
    dsp.processPlanar(leftSamples.data(), rightSamples.data(), frames);
    const auto& output = measureLeft ? leftSamples : rightSamples;
    double outputSquareSum = 0.0;
    for (std::size_t frame = intervalFrames; frame < frames; ++frame) {
        outputSquareSum += output[frame] * output[frame];
    }
    return 10.0 * std::log10(outputSquareSum / inputSquareSum);
}

void measuredResponseTests() {
    const auto left = parseREWConfigurablePEQ(leftText, Channel::left);
    const auto right = parseREWConfigurablePEQ(rightText, Channel::right);
    const std::vector<double> rates{44100.0, 48000.0, 96000.0, 192000.0};
    for (const double rate : rates) {
        std::string error;
        StereoDSP dsp;
        expect(dsp.configure(left.filters, right.filters, rate, error), error);
        constexpr std::size_t points = 48;
        for (std::size_t point = 0; point < points; ++point) {
            const double t = static_cast<double>(point) / static_cast<double>(points - 1);
            const double frequency = 20.0 * std::pow(1000.0, t);
            const double measuredLeft = measuredGainDB(dsp, rate, frequency, true);
            const double expectedLeft =
                responseDB(left.filters, frequency, rate) + dsp.preampDB();
            expect(std::abs(measuredLeft - expectedLeft) < 0.05,
                   "left measured response at " + std::to_string(rate) + " Hz sample rate, " +
                       std::to_string(frequency) + " Hz");

            const double measuredRight = measuredGainDB(dsp, rate, frequency, false);
            const double expectedRight =
                responseDB(right.filters, frequency, rate) + dsp.preampDB();
            expect(std::abs(measuredRight - expectedRight) < 0.05,
                   "right measured response at " + std::to_string(rate) + " Hz sample rate, " +
                       std::to_string(frequency) + " Hz");
        }
    }
}

void aecTests() {
    constexpr std::size_t frames = 48000 * 8;
    std::vector<float> left(frames, 0.0f);
    std::vector<float> right(frames, 0.0f);
    std::uint32_t random = 0x12345678u;
    for (std::size_t index = 0; index < frames; ++index) {
        random = random * 1664525u + 1013904223u;
        const float noise = static_cast<float>((random >> 8) * (1.0 / 16777216.0) - 0.5);
        left[index] = 0.12f * std::sin(2.0 * 3.141592653589793 * 431.0 * index / 48000.0) +
                      0.04f * noise;
        right[index] = 0.10f * std::sin(2.0 * 3.141592653589793 * 877.0 * index / 48000.0) -
                       0.03f * noise;
    }
    std::vector<float> microphone(frames, 0.0f);
    for (std::size_t index = 0; index < frames; ++index) {
        if (index >= 720) microphone[index] += 0.32f * left[index - 720];
        if (index >= 1337) microphone[index] += 0.24f * right[index - 1337];
        if (index >= 3911) microphone[index] += 0.11f * left[index - 3911];
        if (index >= 8420) microphone[index] += 0.06f * right[index - 8420];
    }

    const auto measureProfile = [&](EchoProfile profile) {
        EchoCanceller canceller;
        expect(canceller.valid(), "AEC creates its vDSP FFT setup");
        canceller.setProfile(profile);
        std::vector<float> output(frames, 0.0f);
        for (std::size_t offset = 0; offset < frames; offset += EchoCanceller::blockSize) {
            canceller.process(microphone.data() + offset, left.data() + offset,
                              right.data() + offset, output.data() + offset,
                              EchoCanceller::blockSize, true);
        }
        double inputEnergy = 0.0;
        double outputEnergy = 0.0;
        for (std::size_t index = frames - 48000; index < frames; ++index) {
            inputEnergy += microphone[index] * microphone[index];
            outputEnergy += output[index] * output[index];
        }
        return 10.0 * std::log10(inputEnergy / std::max(outputEnergy, 1e-20));
    };
    const double adaptiveReduction = measureProfile(EchoProfile::adaptive);
    const double qualityReduction = measureProfile(EchoProfile::quality);
    const double balancedReduction = measureProfile(EchoProfile::balanced);
    const double strongReduction = measureProfile(EchoProfile::strong);
    expect(adaptiveReduction >= 30.0,
           "automatic AEC reaches at least 30 dB far-end suppression; got " +
               std::to_string(adaptiveReduction));
    expect(qualityReduction >= 15.0,
           "quality AEC reaches at least 15 dB far-end suppression; got " +
               std::to_string(qualityReduction));
    expect(balancedReduction >= 25.0,
           "balanced AEC reaches at least 25 dB far-end suppression; got " +
               std::to_string(balancedReduction));
    expect(strongReduction >= 35.0,
           "strong AEC reaches at least 35 dB far-end suppression; got " +
               std::to_string(strongReduction));

    // A quiet speaker path must be allowed to keep adapting. Without the
    // model-usefulness gate this case was mistaken for double-talk after the
    // initial convergence timer and the filter froze near zero.
    EchoCanceller quietPathCanceller;
    std::vector<float> quietMicrophone(frames, 0.0f);
    std::vector<float> quietOutput(frames, 0.0f);
    for (std::size_t index = 720; index < frames; ++index) {
        quietMicrophone[index] = 0.025f * left[index - 720];
    }
    for (std::size_t offset = 0; offset < frames; offset += EchoCanceller::blockSize) {
        quietPathCanceller.process(quietMicrophone.data() + offset, left.data() + offset,
                                   right.data() + offset, quietOutput.data() + offset,
                                   EchoCanceller::blockSize, true);
    }
    double quietInputEnergy = 0.0;
    double quietOutputEnergy = 0.0;
    for (std::size_t index = frames - 48000; index < frames; ++index) {
        quietInputEnergy += quietMicrophone[index] * quietMicrophone[index];
        quietOutputEnergy += quietOutput[index] * quietOutput[index];
    }
    const double quietReduction = 10.0 * std::log10(
        quietInputEnergy / std::max(quietOutputEnergy, 1e-20));
    expect(quietReduction >= 15.0,
           "quiet speaker path keeps adapting; got " + std::to_string(quietReduction));

    EchoCanceller doubleTalkCanceller;
    doubleTalkCanceller.setProfile(EchoProfile::quality);
    std::vector<float> trainedOutput(frames, 0.0f);
    constexpr std::size_t doubleTalkFrames = EchoCanceller::blockSize * 187u;
    const std::size_t tail = frames - doubleTalkFrames;
    for (std::size_t offset = 0; offset < tail;
         offset += EchoCanceller::blockSize) {
        doubleTalkCanceller.process(microphone.data() + offset, left.data() + offset,
                                    right.data() + offset, trainedOutput.data() + offset,
                                    EchoCanceller::blockSize, true);
    }
    const EchoMetrics trainedMetrics = doubleTalkCanceller.metrics(0);
    expect(trainedMetrics.linearReductionDB >= 6.0,
           "AEC linear model converges before double-talk; got " +
               std::to_string(trainedMetrics.linearReductionDB) +
               " state=" + echoConvergenceName(trainedMetrics.convergence));
    std::vector<float> doubleTalkMic(doubleTalkFrames);
    std::vector<float> doubleTalkOutput(doubleTalkFrames);
    std::vector<float> nearEnd(doubleTalkFrames);
    for (std::size_t frame = 0; frame < doubleTalkFrames; ++frame) {
        nearEnd[frame] = static_cast<float>(0.08 * std::sin(
            2.0 * 3.141592653589793 * 1231.0 * frame / 48000.0));
        doubleTalkMic[frame] = microphone[tail + frame] + nearEnd[frame];
    }
    for (std::size_t offset = 0; offset < doubleTalkFrames;
         offset += EchoCanceller::blockSize) {
        doubleTalkCanceller.process(doubleTalkMic.data() + offset, left.data() + tail + offset,
                                    right.data() + tail + offset,
                                    doubleTalkOutput.data() + offset,
                                    EchoCanceller::blockSize, true);
    }
    const EchoMetrics doubleTalkMetrics = doubleTalkCanceller.metrics(0);
    expect(doubleTalkMetrics.doubleTalk,
           "AEC detects an uncorrelated near-end talker after convergence; linear=" +
               std::to_string(doubleTalkMetrics.linearReductionDB) +
               " total=" + std::to_string(doubleTalkMetrics.reductionDB) +
               " state=" + echoConvergenceName(doubleTalkMetrics.convergence));
    expect(doubleTalkMetrics.pathChangeCount == 0u,
           "near-end speech is not misreported as a speaker-path change");
    double projection = 0.0;
    double nearEnergy = 0.0;
    for (std::size_t frame = 0; frame < doubleTalkFrames; ++frame) {
        projection += doubleTalkOutput[frame] * nearEnd[frame];
        nearEnergy += nearEnd[frame] * nearEnd[frame];
    }
    const double nearGainDB = 20.0 * std::log10(std::abs(projection / nearEnergy));
    expect(std::abs(nearGainDB) <= 1.0,
           "quality AEC preserves double-talk voice within 1 dB; got " +
               std::to_string(nearGainDB));

    const auto measureDoubleTalkVoice = [&](EchoProfile profile) {
        EchoCanceller profileCanceller;
        profileCanceller.setProfile(profile);
        std::vector<float> trainingScratch(EchoCanceller::blockSize, 0.0f);
        for (std::size_t offset = 0; offset < tail;
             offset += EchoCanceller::blockSize) {
            profileCanceller.process(microphone.data() + offset, left.data() + offset,
                                     right.data() + offset, trainingScratch.data(),
                                     EchoCanceller::blockSize, true);
        }
        std::vector<float> result(doubleTalkFrames, 0.0f);
        for (std::size_t offset = 0; offset < doubleTalkFrames;
             offset += EchoCanceller::blockSize) {
            profileCanceller.process(doubleTalkMic.data() + offset,
                                     left.data() + tail + offset,
                                     right.data() + tail + offset,
                                     result.data() + offset,
                                     EchoCanceller::blockSize, true);
        }
        expect(profileCanceller.metrics(0).doubleTalk,
               "profile AEC retains double-talk protection after convergence");
        double resultProjection = 0.0;
        for (std::size_t frame = 0; frame < doubleTalkFrames; ++frame) {
            resultProjection += result[frame] * nearEnd[frame];
        }
        return 20.0 * std::log10(std::abs(resultProjection / nearEnergy));
    };
    const double adaptiveNearGainDB = measureDoubleTalkVoice(EchoProfile::adaptive);
    const double balancedNearGainDB = measureDoubleTalkVoice(EchoProfile::balanced);
    const double strongNearGainDB = measureDoubleTalkVoice(EchoProfile::strong);
    expect(std::abs(adaptiveNearGainDB) <= 1.5,
           "automatic AEC preserves double-talk voice within 1.5 dB; got " +
               std::to_string(adaptiveNearGainDB));
    expect(std::abs(balancedNearGainDB) <= 2.0,
           "balanced AEC preserves double-talk voice within 2 dB; got " +
               std::to_string(balancedNearGainDB));
    expect(std::abs(strongNearGainDB) <= 3.0,
           "strong AEC preserves double-talk voice within 3 dB; got " +
               std::to_string(strongNearGainDB));

    EchoCanceller canceller;
    std::vector<float> bypass(1024, 0.0f);
    canceller.process(microphone.data(), nullptr, nullptr, bypass.data(), bypass.size(), false);
    expect(std::memcmp(microphone.data(), bypass.data(), bypass.size() * sizeof(float)) == 0,
           "AEC reference-unavailable bypass is bit exact");

    std::vector<float> silence(1024, 0.0f);
    canceller.process(microphone.data(), silence.data(), silence.data(), bypass.data(),
                      bypass.size(), true);
    expect(std::memcmp(microphone.data(), bypass.data(), bypass.size() * sizeof(float)) == 0,
           "AEC silent-reference bypass is bit exact");

    EchoCanceller transitionCanceller;
    std::vector<float> transitionOutput(1024, 0.0f);
    for (std::size_t offset = 0; offset < 48000; offset += EchoCanceller::blockSize) {
        transitionCanceller.process(microphone.data() + offset, left.data() + offset,
                                    right.data() + offset, transitionOutput.data(),
                                    EchoCanceller::blockSize, true);
    }
    const EchoMetrics beforeBriefGap = transitionCanceller.metrics(0);
    transitionCanceller.process(microphone.data() + 48000, silence.data(), silence.data(),
                                transitionOutput.data(), 512, false);
    transitionCanceller.process(microphone.data() + 48512, silence.data(), silence.data(),
                                transitionOutput.data() + 512, 512, false);
    expect(std::memcmp(microphone.data() + 48000, transitionOutput.data(),
                       1024 * sizeof(float)) == 0,
           "AEC transport failure bypasses immediately and bit exactly");
    std::array<float, EchoCanceller::blockSize> resumedOutput{};
    transitionCanceller.process(microphone.data() + 49024, left.data() + 49024,
                                right.data() + 49024, resumedOutput.data(),
                                EchoCanceller::blockSize, true);
    expect(std::all_of(resumedOutput.begin(), resumedOutput.end(),
                       [](float sample) { return std::isfinite(sample); }) &&
               transitionCanceller.metrics(0).linearReductionDB >=
                   beforeBriefGap.linearReductionDB * 0.75f &&
               transitionCanceller.metrics(0).pathChangeCount == 0u,
           "AEC preserves its room model across a brief reference outage");

    for (std::size_t missing = 0; missing < 10u; ++missing) {
        transitionCanceller.process(microphone.data() + 49280,
                                    silence.data(), silence.data(),
                                    transitionOutput.data(), 512, false);
    }
    transitionCanceller.process(microphone.data() + 54400,
                                left.data() + 54400,
                                right.data() + 54400,
                                resumedOutput.data(), EchoCanceller::blockSize, true);
    expect(transitionCanceller.metrics(0).linearReductionDB < 0.1,
           "AEC discards a stale room model after a long reference outage");

    // Music-like material stresses the rank-one stereo bins that occur when a
    // centered vocal, bass, or kick is present in both channels.  It also has
    // decorrelated high-frequency content and abrupt rhythmic envelopes.
    constexpr std::size_t musicFrames = 48000 * 8;
    std::vector<float> musicLeft(musicFrames, 0.0f);
    std::vector<float> musicRight(musicFrames, 0.0f);
    std::vector<float> musicMicrophone(musicFrames, 0.0f);
    random = 0x73b2a91du;
    for (std::size_t frame = 0; frame < musicFrames; ++frame) {
        const double time = static_cast<double>(frame) / 48000.0;
        random = random * 1664525u + 1013904223u;
        const float noise = static_cast<float>((random >> 8) *
            (1.0 / 16777216.0) - 0.5);
        const float beat = std::sin(2.0 * 3.141592653589793 * 2.25 * time) > 0.45
            ? 1.0f : 0.25f;
        const float center = static_cast<float>(
            0.075 * std::sin(2.0 * 3.141592653589793 * 110.0 * time) +
            0.030 * std::sin(2.0 * 3.141592653589793 * 330.0 * time) +
            0.020 * beat * noise);
        musicLeft[frame] = center + static_cast<float>(
            0.025 * std::sin(2.0 * 3.141592653589793 * 733.0 * time));
        musicRight[frame] = center + static_cast<float>(
            0.023 * std::sin(2.0 * 3.141592653589793 * 977.0 * time));
        if (frame >= 720u) musicMicrophone[frame] += 0.31f * musicLeft[frame - 720u];
        if (frame >= 1337u) musicMicrophone[frame] += 0.23f * musicRight[frame - 1337u];
        if (frame >= 5200u) musicMicrophone[frame] += 0.08f * musicLeft[frame - 5200u];
    }
    const auto measureMusic = [&](EchoProfile profile) {
        EchoCanceller musicCanceller;
        musicCanceller.setProfile(profile);
        std::vector<float> musicOutput(musicFrames, 0.0f);
        bool finite = true;
        for (std::size_t offset = 0; offset < musicFrames;
             offset += EchoCanceller::blockSize) {
            musicCanceller.process(musicMicrophone.data() + offset,
                                   musicLeft.data() + offset,
                                   musicRight.data() + offset,
                                   musicOutput.data() + offset,
                                   EchoCanceller::blockSize, true);
            for (std::size_t index = offset;
                 index < offset + EchoCanceller::blockSize; ++index) {
                finite = finite && std::isfinite(musicOutput[index]);
            }
        }
        expect(finite, "music AEC remains finite for correlated stereo");
        expect(!musicCanceller.metrics(0).doubleTalk,
               "far-end-only music is not mistaken for near-end speech");
        double input = 0.0;
        double result = 0.0;
        for (std::size_t index = musicFrames - 48000u; index < musicFrames; ++index) {
            input += musicMicrophone[index] * musicMicrophone[index];
            result += musicOutput[index] * musicOutput[index];
        }
        return 10.0 * std::log10(input / std::max(result, 1e-20));
    };
    const double adaptiveMusic = measureMusic(EchoProfile::adaptive);
    const double qualityMusic = measureMusic(EchoProfile::quality);
    const double balancedMusic = measureMusic(EchoProfile::balanced);
    const double strongMusic = measureMusic(EchoProfile::strong);
    expect(adaptiveMusic >= 32.0,
           "automatic AEC removes correlated music by at least 32 dB; got " +
               std::to_string(adaptiveMusic));
    expect(qualityMusic >= 18.0,
           "quality AEC removes correlated music by at least 18 dB; got " +
               std::to_string(qualityMusic));
    expect(balancedMusic >= 28.0,
           "balanced AEC removes correlated music by at least 28 dB; got " +
               std::to_string(balancedMusic));
    expect(strongMusic >= 38.0,
           "strong AEC removes correlated music by at least 38 dB; got " +
               std::to_string(strongMusic));

    // Real applications commonly render close to 0 dBFS.  Keep the adaptive
    // filters stable when the digital reference is much hotter than the
    // synthetic fixtures above and the room path still keeps the microphone
    // below clipping.
    std::vector<float> loudLeft(musicFrames, 0.0f);
    std::vector<float> loudRight(musicFrames, 0.0f);
    std::vector<float> loudMicrophone(musicFrames, 0.0f);
    for (std::size_t frame = 0; frame < musicFrames; ++frame) {
        loudLeft[frame] = std::clamp(musicLeft[frame] * 7.5f, -1.05f, 1.05f);
        loudRight[frame] = std::clamp(musicRight[frame] * 7.5f, -1.05f, 1.05f);
        if (frame >= 720u) loudMicrophone[frame] += 0.31f * loudLeft[frame - 720u];
        if (frame >= 1337u) loudMicrophone[frame] += 0.23f * loudRight[frame - 1337u];
        if (frame >= 5200u) loudMicrophone[frame] += 0.08f * loudLeft[frame - 5200u];
    }
    EchoCanceller loudCanceller;
    loudCanceller.setProfile(EchoProfile::adaptive);
    std::vector<float> loudOutput(musicFrames, 0.0f);
    float loudOutputPeak = 0.0f;
    for (std::size_t offset = 0; offset < musicFrames;
         offset += EchoCanceller::blockSize) {
        loudCanceller.process(loudMicrophone.data() + offset,
                              loudLeft.data() + offset,
                              loudRight.data() + offset,
                              loudOutput.data() + offset,
                              EchoCanceller::blockSize, true);
        for (std::size_t index = offset;
             index < offset + EchoCanceller::blockSize; ++index) {
            loudOutputPeak = std::max(loudOutputPeak, std::abs(loudOutput[index]));
        }
    }
    expect(std::isfinite(loudOutputPeak) && loudOutputPeak <= 1.0f,
           "AEC stays bounded with a near-full-scale render; peak=" +
               std::to_string(loudOutputPeak));

    // A reference that is temporarily unrelated to the microphone must not
    // make the adaptive bank walk away while it is still learning the room.
    std::vector<float> unrelatedMicrophone(musicFrames, 0.0f);
    random = 0x9a83d5e1u;
    for (std::size_t frame = 0; frame < musicFrames; ++frame) {
        random = random * 1664525u + 1013904223u;
        const float noise = static_cast<float>((random >> 8) *
            (1.0 / 16777216.0) - 0.5);
        unrelatedMicrophone[frame] = static_cast<float>(
            0.16 * std::sin(2.0 * 3.141592653589793 * 191.0 * frame / 48000.0) +
            0.05 * noise);
    }
    EchoCanceller unrelatedCanceller;
    unrelatedCanceller.setProfile(EchoProfile::adaptive);
    std::vector<float> unrelatedOutput(musicFrames, 0.0f);
    float unrelatedPeak = 0.0f;
    for (std::size_t offset = 0; offset < musicFrames;
         offset += EchoCanceller::blockSize) {
        unrelatedCanceller.process(unrelatedMicrophone.data() + offset,
                                   loudLeft.data() + offset,
                                   loudRight.data() + offset,
                                   unrelatedOutput.data() + offset,
                                   EchoCanceller::blockSize, true);
        for (std::size_t index = offset;
             index < offset + EchoCanceller::blockSize; ++index) {
            unrelatedPeak = std::max(unrelatedPeak,
                                     std::abs(unrelatedOutput[index]));
        }
    }
    expect(std::isfinite(unrelatedPeak) && unrelatedPeak <= 1.0f,
           "AEC stays bounded before an acoustic path is found; peak=" +
               std::to_string(unrelatedPeak));
    expect(std::memcmp(unrelatedMicrophone.data(), unrelatedOutput.data(),
                       unrelatedOutput.size() * sizeof(float)) == 0,
           "AEC keeps an unrelated near-end source bit exact before finding a room path");

    // Streaming music has large block-to-block spectral changes that the
    // stationary tone fixture does not exercise.  Alternate dense transients,
    // quiet passages, and full-scale sections while retaining a valid room
    // echo path.
    std::vector<float> dynamicLeft(musicFrames, 0.0f);
    std::vector<float> dynamicRight(musicFrames, 0.0f);
    std::vector<float> dynamicMicrophone(musicFrames, 0.0f);
    random = 0x66e40cc5u;
    float lowLeft = 0.0f;
    float lowRight = 0.0f;
    for (std::size_t frame = 0; frame < musicFrames; ++frame) {
        random = random * 1664525u + 1013904223u;
        const float whiteLeft = static_cast<float>((random >> 8) *
            (2.0 / 16777216.0) - 1.0);
        random = random * 1664525u + 1013904223u;
        const float whiteRight = static_cast<float>((random >> 8) *
            (2.0 / 16777216.0) - 1.0);
        lowLeft = 0.82f * lowLeft + 0.18f * whiteLeft;
        lowRight = 0.79f * lowRight + 0.21f * whiteRight;
        const std::size_t section = (frame / 4096u) % 6u;
        const float level = section == 0u ? 0.0f :
            (section == 1u ? 0.02f : (section == 2u ? 0.90f :
            (section == 3u ? 0.08f : (section == 4u ? 0.65f : 0.25f))));
        dynamicLeft[frame] = std::clamp(level *
            (0.72f * lowLeft + 0.28f * whiteLeft), -1.0f, 1.0f);
        dynamicRight[frame] = std::clamp(level *
            (0.70f * lowRight + 0.20f * whiteRight + 0.10f * lowLeft),
            -1.0f, 1.0f);
        if (frame >= 720u) {
            dynamicMicrophone[frame] += 0.35f * dynamicLeft[frame - 720u];
        }
        if (frame >= 1337u) {
            dynamicMicrophone[frame] += 0.28f * dynamicRight[frame - 1337u];
        }
        if (frame >= 8420u) {
            dynamicMicrophone[frame] += 0.08f * dynamicLeft[frame - 8420u];
        }
    }
    EchoCanceller dynamicCanceller;
    dynamicCanceller.setProfile(EchoProfile::adaptive);
    std::vector<float> dynamicOutput(musicFrames, 0.0f);
    float dynamicPeak = 0.0f;
    std::size_t dynamicBypassBlocks = 0u;
    std::size_t dynamicTrackingBlocks = 0u;
    for (std::size_t offset = 0; offset < musicFrames;
         offset += EchoCanceller::blockSize) {
        dynamicCanceller.process(dynamicMicrophone.data() + offset,
                                 dynamicLeft.data() + offset,
                                 dynamicRight.data() + offset,
                                 dynamicOutput.data() + offset,
                                 EchoCanceller::blockSize, true);
        if (std::memcmp(dynamicMicrophone.data() + offset,
                        dynamicOutput.data() + offset,
                        EchoCanceller::blockSize * sizeof(float)) == 0) {
            ++dynamicBypassBlocks;
        }
        if (dynamicCanceller.metrics(0u).convergence ==
            EchoConvergenceState::tracking) {
            ++dynamicTrackingBlocks;
        }
        for (std::size_t index = offset;
             index < offset + EchoCanceller::blockSize; ++index) {
            dynamicPeak = std::max(dynamicPeak, std::abs(dynamicOutput[index]));
        }
    }
    expect(std::isfinite(dynamicPeak) && dynamicPeak <= 1.0f,
           "AEC stays bounded across music transients; peak=" +
               std::to_string(dynamicPeak));
    double dynamicInputEnergy = 0.0;
    double dynamicOutputEnergy = 0.0;
    for (std::size_t frame = musicFrames - 48000u; frame < musicFrames; ++frame) {
        dynamicInputEnergy += dynamicMicrophone[frame] * dynamicMicrophone[frame];
        dynamicOutputEnergy += dynamicOutput[frame] * dynamicOutput[frame];
    }
    const double dynamicReductionDB = 10.0 * std::log10(
        dynamicInputEnergy / std::max(dynamicOutputEnergy, 1e-20));
    const auto dynamicMetrics = dynamicCanceller.metrics(0u);
    expect(dynamicReductionDB >= 24.0,
           "automatic AEC remains converged across full-scale music dynamics; got " +
               std::to_string(dynamicReductionDB) + " dB (linear " +
               std::to_string(dynamicMetrics.linearReductionDB) +
               " dB, residual " +
               std::to_string(dynamicMetrics.residualSuppressionDB) +
               " dB, state " + echoConvergenceName(dynamicMetrics.convergence) +
               ", bypass blocks " + std::to_string(dynamicBypassBlocks) +
               ", tracking blocks " + std::to_string(dynamicTrackingBlocks) +
               ")");

    // Speech-like double-talk is broadband and syllabic rather than a single
    // stationary tone.  Verify that each profile keeps the near-end component
    // while continuing to reject render-correlated music in the other bins.
    constexpr std::size_t speechStart = 48000u * 5u;
    std::vector<float> nearSpeech(musicFrames, 0.0f);
    random = 0x1b873593u;
    for (std::size_t frame = speechStart; frame < musicFrames; ++frame) {
        const double time = static_cast<double>(frame - speechStart) / 48000.0;
        random = random * 1664525u + 1013904223u;
        const float breath = static_cast<float>((random >> 8) *
            (1.0 / 16777216.0) - 0.5);
        const float syllable = static_cast<float>(0.20 + 0.80 *
            std::max(0.0, std::sin(2.0 * 3.141592653589793 * 3.7 * time)));
        nearSpeech[frame] = syllable * static_cast<float>(
            0.032 * std::sin(2.0 * 3.141592653589793 * 173.0 * time) +
            0.018 * std::sin(2.0 * 3.141592653589793 * 346.0 * time) +
            0.011 * std::sin(2.0 * 3.141592653589793 * 692.0 * time) +
            0.007 * std::sin(2.0 * 3.141592653589793 * 1384.0 * time) +
            0.004 * breath);
    }
    const auto measureSpeechDoubleTalk = [&](EchoProfile profile,
                                              double voiceLimitDB,
                                              double echoTargetDB) {
        EchoCanceller speechCanceller;
        speechCanceller.setProfile(profile);
        std::vector<float> combined(musicFrames, 0.0f);
        std::vector<float> result(musicFrames, 0.0f);
        for (std::size_t frame = 0; frame < musicFrames; ++frame) {
            combined[frame] = musicMicrophone[frame] + nearSpeech[frame];
        }
        for (std::size_t offset = 0; offset < musicFrames;
             offset += EchoCanceller::blockSize) {
            speechCanceller.process(combined.data() + offset,
                                    musicLeft.data() + offset,
                                    musicRight.data() + offset,
                                    result.data() + offset,
                                    EchoCanceller::blockSize, true);
        }
        const EchoMetrics metrics = speechCanceller.metrics(0);
        expect(metrics.doubleTalk,
               "speech-like near end triggers double-talk protection");

        // Solve a two-source least-squares projection.  This separates voice
        // preservation from remaining far-end echo even when the two finite
        // test sequences have a small non-zero cross-correlation.
        double nn = 0.0;
        double ff = 0.0;
        double nf = 0.0;
        double yn = 0.0;
        double yf = 0.0;
        for (std::size_t frame = musicFrames - 48000u; frame < musicFrames; ++frame) {
            const double near = nearSpeech[frame];
            const double far = musicMicrophone[frame];
            const double value = result[frame];
            nn += near * near;
            ff += far * far;
            nf += near * far;
            yn += value * near;
            yf += value * far;
        }
        const double determinant = std::max(nn * ff - nf * nf, 1e-20);
        const double nearGain = (yn * ff - yf * nf) / determinant;
        const double farGain = (yf * nn - yn * nf) / determinant;
        const double voiceGainDB = 20.0 * std::log10(std::max(std::abs(nearGain), 1e-12));
        const double farReductionDB = -20.0 * std::log10(
            std::max(std::abs(farGain), 1e-12));
        expect(voiceGainDB >= -voiceLimitDB && voiceGainDB <= 0.5,
               std::string(echoProfileName(profile)) +
                   " speech-like near end stays within the profile voice limit; got " +
                   std::to_string(voiceGainDB));
        expect(farReductionDB >= echoTargetDB,
               std::string(echoProfileName(profile)) +
                   " double-talk retains music rejection; got " +
                   std::to_string(farReductionDB) + " linear=" +
                   std::to_string(metrics.linearReductionDB));
    };
    measureSpeechDoubleTalk(EchoProfile::adaptive, 1.5, 14.5);
    measureSpeechDoubleTalk(EchoProfile::quality, 1.0, 12.0);
    measureSpeechDoubleTalk(EchoProfile::balanced, 2.0, 14.5);
    measureSpeechDoubleTalk(EchoProfile::strong, 3.0, 14.5);

    // A quiet passage follows a learned loud passage while nearby speech
    // stays audible. Whole-block reduction is no longer a reliable measure
    // of whether the much smaller echo estimate should remain in use.
    {
        EchoCanceller quietCanceller;
        std::vector<float> left(musicFrames), right(musicFrames), echo(musicFrames),
            mic(musicFrames), output(musicFrames);
        for (std::size_t i = 0; i < musicFrames; ++i) {
            const float gain = i >= speechStart ? 0.03f : 1.0f;
            left[i] = musicLeft[i] * gain;
            right[i] = musicRight[i] * gain;
            if (i >= 720) echo[i] += 0.31f * left[i - 720];
            if (i >= 1337) echo[i] += 0.23f * right[i - 1337];
            if (i >= 5200) echo[i] += 0.08f * left[i - 5200];
            mic[i] = echo[i] + nearSpeech[i];
        }
        unsigned bypass = 0;
        unsigned linearOnly = 0;
        bool boundedLinearOutput = true;
        for (std::size_t i = 0; i < musicFrames; i += EchoCanceller::blockSize) {
            const auto before = quietCanceller.metrics(0).linearOnlyBlocks;
            quietCanceller.process(mic.data()+i, left.data()+i, right.data()+i,
                                   output.data()+i, EchoCanceller::blockSize, true);
            if (quietCanceller.metrics(0).linearOnlyBlocks > before) {
                ++linearOnly;
                double inputEnergy = 0, outputEnergy = 0;
                for (std::size_t j=i; j<i+EchoCanceller::blockSize; ++j) {
                    inputEnergy += mic[j]*mic[j];
                    outputEnergy += output[j]*output[j];
                }
                boundedLinearOutput = boundedLinearOutput && std::isfinite(outputEnergy) &&
                    outputEnergy <= inputEnergy * 1.1001 + 1e-12;
            }
            if (i > speechStart + 24000 &&
                std::memcmp(mic.data()+i, output.data()+i, EchoCanceller::blockSize*sizeof(float)) == 0)
                ++bypass;
        }
        double nn=0, ff=0, nf=0, yn=0, yf=0;
        for (std::size_t i=speechStart+24000; i<musicFrames; ++i) {
            const double n=nearSpeech[i], f=echo[i], y=output[i];
            nn+=n*n; ff+=f*f; nf+=n*f; yn+=y*n; yf+=y*f;
        }
        const double det=std::max(nn*ff-nf*nf,1e-20);
        const double voice=20*std::log10(std::max(std::abs((yn*ff-yf*nf)/det),1e-12));
        const double rejection=-20*std::log10(std::max(std::abs((yf*nn-yn*nf)/det),1e-12));
        std::cout << "Quiet passage: bypass=" << bypass << ", voice=" << voice
                  << " dB, echo reduction=" << rejection << " dB\n";
        expect(bypass == 0, "a quiet learned passage must not repeatedly expose raw microphone blocks");
        expect(linearOnly > 0 && boundedLinearOutput,
               "quiet linear subtraction limits each block to a ten percent energy increase");
        expect(voice >= -1.5, "quiet-passage cancellation preserves nearby speech");
        expect(rejection >= 8, "quiet-passage cancellation retains the learned echo path");
    }

    // A separate nearby source can abruptly dominate a learned speaker path.
    // Measure short windows as well as the average: a raw-mic bypass lasting
    // only a few blocks is audible even when the whole utterance scores well.
    EchoCanceller burstCanceller;
    std::vector<float> burstNear(musicFrames), burstMic(musicFrames), burstOut(musicFrames);
    for (std::size_t i = 0; i < musicFrames; ++i) {
        const bool loud = ((i / 4096u) % 3u) != 0;
        burstNear[i] = nearSpeech[i] * (loud ? 7.0f : 1.0f);
        burstMic[i] = musicMicrophone[i] + burstNear[i];
    }
    for (std::size_t i = 0; i < musicFrames; i += EchoCanceller::blockSize)
        burstCanceller.process(burstMic.data()+i, musicLeft.data()+i, musicRight.data()+i,
                               burstOut.data()+i, EchoCanceller::blockSize, true);
    double worstBurstEcho = 100, worstBurstVoice = 100;
    for (std::size_t start = speechStart + 4800; start + 2400 <= musicFrames; start += 2400) {
        double nn=0, ff=0, nf=0, yn=0, yf=0;
        for (std::size_t i=start; i<start+2400; ++i) {
            const double n=burstNear[i], f=musicMicrophone[i], y=burstOut[i];
            nn+=n*n; ff+=f*f; nf+=n*f; yn+=y*n; yf+=y*f;
        }
        const double det=std::max(nn*ff-nf*nf,1e-20);
        const double voice=(yn*ff-yf*nf)/det, echo=(yf*nn-yn*nf)/det;
        worstBurstVoice=std::min(worstBurstVoice,20*std::log10(std::max(std::abs(voice),1e-12)));
        worstBurstEcho=std::min(worstBurstEcho,-20*std::log10(std::max(std::abs(echo),1e-12)));
    }
    std::cout << "50 ms near-source burst windows: minimum voice gain=" << worstBurstVoice
              << " dB, minimum echo reduction=" << worstBurstEcho << " dB\n";
    expect(worstBurstVoice >= -1.5, "nearby source bursts preserve voice in 50 ms windows; got " + std::to_string(worstBurstVoice));
    expect(worstBurstEcho >= 8.0, "nearby source bursts do not expose raw speaker echo in 50 ms windows; got " + std::to_string(worstBurstEcho));

    // Loudspeaker distortion is modeled as a short generalized-Hammerstein
    // path.  Use the same normalized polynomial basis as the runtime, but a
    // separate acoustic delay, so the nonlinear branch must earn activation.
    double leftVariance = 0.0;
    double rightVariance = 0.0;
    for (std::size_t index = 0; index < musicFrames; ++index) {
        leftVariance += musicLeft[index] * musicLeft[index];
        rightVariance += musicRight[index] * musicRight[index];
    }
    leftVariance /= static_cast<double>(musicFrames);
    rightVariance /= static_cast<double>(musicFrames);
    std::vector<float> nonlinearMicrophone(musicFrames, 0.0f);
    for (std::size_t frame = 0; frame < musicFrames; ++frame) {
        if (frame >= 720u) nonlinearMicrophone[frame] += 0.28f * musicLeft[frame - 720u];
        if (frame >= 1337u) nonlinearMicrophone[frame] += 0.22f * musicRight[frame - 1337u];
        if (frame >= 900u) {
            const float sample = musicLeft[frame - 900u];
            nonlinearMicrophone[frame] += static_cast<float>(0.055 *
                (sample * sample - leftVariance) / std::sqrt(2.0 * leftVariance));
        }
        if (frame >= 1180u) {
            const float sample = musicRight[frame - 1180u];
            nonlinearMicrophone[frame] += static_cast<float>(0.050 *
                (sample * sample * sample - 3.0 * rightVariance * sample) /
                (std::sqrt(6.0) * rightVariance));
        }
    }
    EchoCanceller nonlinearCanceller;
    nonlinearCanceller.setProfile(EchoProfile::adaptive);
    std::vector<float> nonlinearOutput(musicFrames, 0.0f);
    for (std::size_t offset = 0; offset < musicFrames;
         offset += EchoCanceller::blockSize) {
        nonlinearCanceller.process(nonlinearMicrophone.data() + offset,
                                   musicLeft.data() + offset,
                                   musicRight.data() + offset,
                                   nonlinearOutput.data() + offset,
                                   EchoCanceller::blockSize, true);
    }
    double nonlinearInputEnergy = 0.0;
    double nonlinearOutputEnergy = 0.0;
    for (std::size_t index = musicFrames - 48000u; index < musicFrames; ++index) {
        nonlinearInputEnergy += nonlinearMicrophone[index] * nonlinearMicrophone[index];
        nonlinearOutputEnergy += nonlinearOutput[index] * nonlinearOutput[index];
    }
    const double nonlinearReduction = 10.0 * std::log10(
        nonlinearInputEnergy / std::max(nonlinearOutputEnergy, 1e-20));
    expect(nonlinearReduction >= 28.0,
           "automatic AEC removes high-volume nonlinear music by at least 28 dB; got " +
               std::to_string(nonlinearReduction));
    expect(nonlinearCanceller.metrics(0).nonlinearActive,
           "automatic AEC accepts the nonlinear path only after it improves the estimate");

    // Change both acoustic delays and gains after the steady-state cadence has
    // engaged.  The main filter must flag the path loss, wake the faster shadow
    // filter, and recover within the remaining two seconds.
    constexpr std::size_t pathChangeFrame = 48000u * 6u;
    std::vector<float> changedPathMicrophone(musicFrames, 0.0f);
    for (std::size_t frame = 0; frame < musicFrames; ++frame) {
        if (frame < pathChangeFrame) {
            if (frame >= 720u) {
                changedPathMicrophone[frame] += 0.31f * musicLeft[frame - 720u];
            }
            if (frame >= 1337u) {
                changedPathMicrophone[frame] += 0.23f * musicRight[frame - 1337u];
            }
        } else {
            if (frame >= 2680u) {
                changedPathMicrophone[frame] += 0.24f * musicLeft[frame - 2680u];
            }
            if (frame >= 4210u) {
                changedPathMicrophone[frame] += 0.34f * musicRight[frame - 4210u];
            }
        }
    }
    EchoCanceller pathCanceller;
    std::vector<float> changedPathOutput(musicFrames, 0.0f);
    for (std::size_t offset = 0; offset < musicFrames;
         offset += EchoCanceller::blockSize) {
        pathCanceller.process(changedPathMicrophone.data() + offset,
                              musicLeft.data() + offset,
                              musicRight.data() + offset,
                              changedPathOutput.data() + offset,
                              EchoCanceller::blockSize, true);
    }
    double changedPathInputEnergy = 0.0;
    double changedPathOutputEnergy = 0.0;
    for (std::size_t index = musicFrames - 24000u; index < musicFrames; ++index) {
        changedPathInputEnergy += changedPathMicrophone[index] * changedPathMicrophone[index];
        changedPathOutputEnergy += changedPathOutput[index] * changedPathOutput[index];
    }
    const double changedPathReduction = 10.0 * std::log10(
        changedPathInputEnergy / std::max(changedPathOutputEnergy, 1e-20));
    const EchoMetrics changedPathMetrics = pathCanceller.metrics(0);
    expect(changedPathMetrics.pathChangeCount >= 1u,
           "AEC detects an abrupt speaker-path change; state=" +
               std::string(echoConvergenceName(changedPathMetrics.convergence)) +
               " linear=" + std::to_string(changedPathMetrics.linearReductionDB));
    expect(changedPathReduction >= 15.0,
           "AEC recovers from a speaker-path change within two seconds; got " +
               std::to_string(changedPathReduction));

    // Historical profile names migrate to the one automatic public mode.
    expect(echoProfileFromName("quality") == EchoProfile::adaptive &&
               echoProfileFromName("balanced") == EchoProfile::adaptive &&
               echoProfileFromName("strong") == EchoProfile::adaptive,
           "legacy AEC profiles migrate to automatic mode");
    EchoCanceller switchingCanceller;
    std::array<float, EchoCanceller::blockSize> switchingOutput{};
    for (std::size_t offset = 0; offset < 48000u * 3u;
         offset += EchoCanceller::blockSize) {
        switchingCanceller.process(musicMicrophone.data() + offset,
                                   musicLeft.data() + offset,
                                   musicRight.data() + offset,
                                   switchingOutput.data(),
                                   EchoCanceller::blockSize, true);
    }
    switchingCanceller.setProfile(EchoProfile::adaptive);
    switchingCanceller.process(musicMicrophone.data() + 48000u * 3u,
                               musicLeft.data() + 48000u * 3u,
                               musicRight.data() + 48000u * 3u,
                               switchingOutput.data(),
                               EchoCanceller::blockSize, true);
    expect(switchingCanceller.profile() == EchoProfile::adaptive,
           "AEC remains in automatic mode without rebuilding the route");
    expect(std::all_of(switchingOutput.begin(), switchingOutput.end(),
                       [](float sample) { return std::isfinite(sample); }),
           "AEC profile switch has a finite transition block");

    std::array<float, EchoCanceller::blockSize> clippedMicrophone{};
    clippedMicrophone.fill(0.96f);
    switchingCanceller.process(clippedMicrophone.data(),
                               musicLeft.data() + 48000u * 3u,
                               musicRight.data() + 48000u * 3u,
                               switchingOutput.data(),
                               EchoCanceller::blockSize, true);
    expect(switchingCanceller.metrics(0).inputClipping,
           "AEC reports M2 input clipping and freezes adaptation");
    expect(std::all_of(switchingOutput.begin(), switchingOutput.end(),
                       [](float sample) { return std::isfinite(sample); }),
           "AEC clipping protection keeps output finite");
}

void referenceTimelineTests() {
    constexpr double hostFrequency = 1000000000.0;
    constexpr std::uint64_t hostOrigin = 1000000000u;
    const std::array<double, 4> rates{44100.0, 48000.0, 96000.0, 192000.0};
    for (double nominalRate : rates) {
        for (double ppm : {-200.0, 200.0}) {
            StereoReferenceTimeline timeline;
            const double actualRate = nominalRate * (1.0 + ppm * 1e-6);
            constexpr std::size_t block = 512;
            const std::size_t total = static_cast<std::size_t>(actualRate * 2.0);
            std::vector<float> left(block);
            std::vector<float> right(block);
            for (std::size_t offset = 0; offset + block <= total; offset += block) {
                for (std::size_t frame = 0; frame < block; ++frame) {
                    const double time = static_cast<double>(offset + frame) / actualRate;
                    left[frame] = static_cast<float>(0.4 * std::sin(
                        2.0 * 3.141592653589793 * 997.0 * time));
                    right[frame] = static_cast<float>(0.3 * std::cos(
                        2.0 * 3.141592653589793 * 601.0 * time));
                }
                const std::uint64_t hostTime = hostOrigin + static_cast<std::uint64_t>(
                    std::llround(static_cast<double>(offset) * hostFrequency / actualRate));
                timeline.push(left.data(), right.data(), block, nominalRate,
                              hostTime, hostFrequency);
            }

            constexpr std::size_t renderedFrames = 1024;
            std::vector<float> renderedLeft(renderedFrames);
            std::vector<float> renderedRight(renderedFrames);
            constexpr double renderTime = 1.5;
            const std::uint64_t renderHost = hostOrigin + static_cast<std::uint64_t>(
                std::llround(renderTime * hostFrequency));
            expect(timeline.render(renderHost, hostFrequency, renderedLeft.data(),
                                   renderedRight.data(), renderedFrames),
                   "reference timeline renders " + std::to_string(nominalRate) +
                       " Hz at " + std::to_string(ppm) + " ppm");
            double error = 0.0;
            double signal = 0.0;
            for (std::size_t frame = 0; frame < renderedFrames; ++frame) {
                const double time = renderTime + static_cast<double>(frame) / 48000.0;
                const double expectedLeft = 0.4 * std::sin(
                    2.0 * 3.141592653589793 * 997.0 * time);
                const double expectedRight = 0.3 * std::cos(
                    2.0 * 3.141592653589793 * 601.0 * time);
                error += std::pow(renderedLeft[frame] - expectedLeft, 2.0) +
                         std::pow(renderedRight[frame] - expectedRight, 2.0);
                signal += expectedLeft * expectedLeft + expectedRight * expectedRight;
            }
            const double snr = 10.0 * std::log10(signal / std::max(error, 1e-20));
            expect(snr >= 35.0, "reference ASRC remains accurate; SNR " +
                                    std::to_string(snr) + " dB at " +
                                    std::to_string(nominalRate) + " Hz");
        }
    }
}

}  // namespace

int main() {
    parserTests();
    importTests();
    responseTests();
    processorTests();
    micRingTests();
    displayCoreTests();
    measuredResponseTests();
    aecTests();
    referenceTimelineTests();
    if (failures == 0) {
        std::cout << "All MacTools core tests passed\n";
        return 0;
    }
    std::cerr << failures << " test(s) failed\n";
    return 1;
}
