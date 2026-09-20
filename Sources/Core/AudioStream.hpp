#pragma once
#include "AEC.hpp"
#include <array>
#include <atomic>

namespace macsound {
// One auxiliary-input producer, one microphone consumer. The consumer alone
// owns the timeline and canceller. Never reset the queue while either is active.
class EchoStream {
public:
    static constexpr std::size_t block = EchoCanceller::blockSize;
    static constexpr std::size_t latencyFrames = 1024;
    static constexpr double ticksPerSecond = 10000000.0; // normalized timestamps: 100 ns units
    EchoStream();
    bool valid() const { return canceller_.valid(); }
    void reset(); // only while both callbacks are stopped
    void reference(const float* interleaved, std::size_t frames, unsigned channels,
                   std::uint64_t timestamp, bool silent);
    void microphone(const float* interleaved, float* output, std::size_t frames,
                    unsigned inputChannels, unsigned outputChannels,
                    std::uint64_t timestamp, bool silent, bool enabled);
    EchoMetrics metrics() const { return canceller_.metrics(missing_.load()); }
    std::uint64_t dropped() const { return dropped_.load(); }
private:
    struct Packet {
        std::array<float, block> left{}, right{};
        std::uint64_t time = 0;
        std::size_t frames = 0;
    };
    struct MicBlock { std::array<float, block> samples{}; std::uint64_t time = 0; bool timed = false; };
    static constexpr std::size_t queueSize = 256;
    std::array<Packet, queueSize> queue_{};
    alignas(64) std::atomic<std::uint64_t> write_{0};
    alignas(64) std::atomic<std::uint64_t> read_{0};
    std::atomic<std::uint64_t> dropped_{0}, missing_{0};
    std::uint64_t seenDropped_ = 0, expectedReference_ = 0, expectedMic_ = 0;
    StereoReferenceTimeline timeline_;
    EchoCanceller canceller_;
    std::array<MicBlock, latencyFrames / block> pending_{};
    std::array<float, block> output_{}, left_{}, right_{};
    std::size_t position_ = 0, pendingWrite_ = 0, filled_ = 0;
    bool enabled_ = false;
    void drain();
    void resetMic();
};
}
