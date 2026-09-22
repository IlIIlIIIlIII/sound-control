#include "DSP.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

int main() {
    constexpr double sampleRate = 192000.0;
    constexpr std::size_t blockFrames = 512;
    constexpr double audioSeconds = 30.0;
    const std::vector<personaltools::PEQFilter> left{
        {66.0, -12.0, 7.40}, {140.0, -12.0, 3.21}};
    const std::vector<personaltools::PEQFilter> right{
        {65.0, -8.4, 8.00}, {142.0, -12.0, 3.36}};

    personaltools::StereoDSP dsp;
    std::string error;
    if (!dsp.configure(left, right, sampleRate, error)) {
        std::cerr << error << '\n';
        return 1;
    }

    std::vector<double> leftBuffer(blockFrames);
    std::vector<double> rightBuffer(blockFrames);
    const std::size_t blocks =
        static_cast<std::size_t>(sampleRate * audioSeconds) / blockFrames;
    const auto started = std::chrono::steady_clock::now();
    std::size_t frameBase = 0;
    for (std::size_t block = 0; block < blocks; ++block) {
        for (std::size_t frame = 0; frame < blockFrames; ++frame) {
            leftBuffer[frame] = std::sin(2.0 * 3.141592653589793 * 997.0 *
                                         (frameBase + frame) / sampleRate);
            rightBuffer[frame] = std::sin(2.0 * 3.141592653589793 * 1009.0 *
                                          (frameBase + frame) / sampleRate);
        }
        dsp.processPlanar(leftBuffer.data(), rightBuffer.data(), blockFrames);
        frameBase += blockFrames;
    }
    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    std::cout << "Processed " << audioSeconds << " seconds at 192 kHz in "
              << elapsed << " seconds\n";
    std::cout << "Equivalent single-core DSP load: "
              << (elapsed / audioSeconds * 100.0) << "%\n";
    return 0;
}
