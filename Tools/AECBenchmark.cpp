#include "AEC.hpp"

#include <chrono>
#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <vector>

int main() {
    constexpr std::size_t block = 512;
    constexpr double seconds = 30.0;
    constexpr std::size_t blocks = static_cast<std::size_t>(48000.0 * seconds) / block;
    constexpr std::size_t frames = blocks * block;
    std::vector<float> left(frames);
    std::vector<float> right(frames);
    std::vector<float> microphone(frames);
    std::vector<float> output(block);
    std::uint32_t random = 0x73b2a91du;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const double time = static_cast<double>(frame) / 48000.0;
        random = random * 1664525u + 1013904223u;
        const float noise = static_cast<float>((random >> 8) *
            (1.0 / 16777216.0) - 0.5);
        const float rhythm = std::sin(2.0 * 3.141592653589793 * 2.1 * time) > 0.55
            ? 1.0f : 0.35f;
        const float center = static_cast<float>(
            0.065 * std::sin(2.0 * 3.141592653589793 * 110.0 * time) +
            0.035 * std::sin(2.0 * 3.141592653589793 * 440.0 * time) +
            0.018 * rhythm * noise);
        left[frame] = center + static_cast<float>(
            0.025 * std::sin(2.0 * 3.141592653589793 * 733.0 * time));
        right[frame] = center + static_cast<float>(
            0.023 * std::sin(2.0 * 3.141592653589793 * 977.0 * time));
        if (frame >= 720u) microphone[frame] += 0.30f * left[frame - 720u];
        if (frame >= 1337u) microphone[frame] += 0.24f * right[frame - 1337u];
        if (frame >= 5200u) microphone[frame] += 0.08f * left[frame - 5200u];
    }

    struct ProfileRun {
        soundcontrol::EchoProfile profile;
        std::string_view name;
        double cpuBudget;
    };
    constexpr std::array<ProfileRun, 1> profiles{{
        {soundcontrol::EchoProfile::adaptive, "automatic", 1.50},
    }};

    std::cout << std::fixed << std::setprecision(3);
    for (const ProfileRun& run : profiles) {
        soundcontrol::EchoCanceller canceller;
        canceller.setProfile(run.profile);
        if (!canceller.valid()) return 1;
        std::vector<double> callbackMicroseconds(blocks, 0.0);
        double elapsed = 0.0;
        for (std::size_t iteration = 0; iteration < blocks; ++iteration) {
            const std::size_t offset = iteration * block;
            const auto started = std::chrono::steady_clock::now();
            canceller.process(microphone.data() + offset, left.data() + offset,
                              right.data() + offset, output.data(), block, true);
            const double callbackElapsed = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - started).count();
            elapsed += callbackElapsed;
            callbackMicroseconds[iteration] = callbackElapsed * 1.0e6;
            if (!std::all_of(output.begin(), output.end(),
                             [](float sample) { return std::isfinite(sample); })) {
                std::cerr << run.name << ": non-finite output at block "
                          << iteration << " ("
                          << static_cast<double>(iteration * block) / 48000.0
                          << " s), linear=" << canceller.metrics(0).linearReductionDB
                          << ", total=" << canceller.metrics(0).reductionDB
                          << ", state="
                          << soundcontrol::echoConvergenceName(canceller.metrics(0).convergence)
                          << "\n";
                return 2;
            }
        }
        std::sort(callbackMicroseconds.begin(), callbackMicroseconds.end());
        const std::size_t p99Index = static_cast<std::size_t>(
            0.99 * static_cast<double>(callbackMicroseconds.size() - 1u));
        const double cpu = elapsed / seconds * 100.0;
        const soundcontrol::EchoMetrics metrics = canceller.metrics(0);
        std::cout << run.name << ": cpu=" << cpu << "% (target "
                  << run.cpuBudget << "%), p99=" << callbackMicroseconds[p99Index]
                  << " us, linear=" << metrics.linearReductionDB
                  << " dB, total=" << metrics.reductionDB
                  << " dB, nonlinear=" << (metrics.nonlinearActive ? "on" : "off")
                  << "\n";
    }
    return 0;
}
