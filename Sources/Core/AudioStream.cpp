#include "AudioStream.hpp"
#include <algorithm>
#include <cmath>

namespace personaltools {
namespace {
std::uint64_t offsetTime(std::uint64_t time, std::size_t frames) {
    return time + (frames * 10000000ull + 24000) / 48000;
}
bool discontinuous(std::uint64_t expected, std::uint64_t actual) {
    return expected && (actual > expected ? actual - expected : expected - actual) > 10000;
}
}
EchoStream::EchoStream() = default;
void EchoStream::resetMic() {
    position_ = pendingWrite_ = filled_ = 0;
    expectedMic_ = 0;
    output_.fill(0);
    canceller_.reset();
}
void EchoStream::reset() {
    write_.store(0); read_.store(0); dropped_.store(0); missing_.store(0);
    seenDropped_ = expectedReference_ = 0;
    timeline_.reset(); resetMic(); enabled_ = false;
}
void EchoStream::reference(const float* input, std::size_t frames, unsigned channels,
                           std::uint64_t time, bool silent) {
    if (!time || (channels != 1 && channels != 2) || (!silent && !input)) return;
    for (std::size_t offset = 0; offset < frames; offset += block) {
        const auto w = write_.load(std::memory_order_relaxed);
        if (w - read_.load(std::memory_order_acquire) >= queueSize) {
            dropped_.fetch_add(1, std::memory_order_relaxed); return;
        }
        auto& p = queue_[w % queueSize];
        p.frames = std::min(block, frames - offset); p.time = offsetTime(time, offset);
        for (std::size_t i = 0; i < p.frames; ++i) {
            p.left[i] = silent ? 0.f : input[(offset + i) * channels];
            p.right[i] = silent ? 0.f : input[(offset + i) * channels + channels - 1];
        }
        write_.store(w + 1, std::memory_order_release);
    }
}
void EchoStream::drain() {
    const auto dropped = dropped_.load(std::memory_order_relaxed);
    if (dropped != seenDropped_) {
        // Discard only packets already published; the producer may continue.
        read_.store(write_.load(std::memory_order_acquire), std::memory_order_release);
        timeline_.reset(); canceller_.reset(); expectedReference_ = 0;
        seenDropped_ = dropped;
    }
    const auto end = write_.load(std::memory_order_acquire);
    auto r = read_.load(std::memory_order_relaxed);
    for (; r < end; ++r) {
        const auto& p = queue_[r % queueSize];
        if (discontinuous(expectedReference_, p.time)) {
            timeline_.reset(); canceller_.reset();
        }
        timeline_.push(p.left.data(), p.right.data(), p.frames, 48000, p.time, ticksPerSecond);
        expectedReference_ = offsetTime(p.time, p.frames);
        read_.store(r + 1, std::memory_order_release);
    }
}
void EchoStream::microphone(const float* input, float* out, std::size_t frames,
                            unsigned inChannels, unsigned outChannels,
                            std::uint64_t time, bool silent, bool enabled) {
    if (!out || !inChannels || !outChannels || (!silent && !input)) return;
    drain();
    if (discontinuous(expectedMic_, time)) resetMic();
    expectedMic_ = time ? offsetTime(time, frames) : 0;
    if (enabled != enabled_) { canceller_.reset(); enabled_ = enabled; }
    for (std::size_t i = 0; i < frames; ++i) {
        auto& current = pending_[pendingWrite_];
        if (!position_) { current.time = offsetTime(time, i); current.timed = time != 0; }
        current.timed = current.timed && time != 0;
        const float sample = silent ? 0.f : input[i * inChannels];
        current.samples[position_] = std::isfinite(sample) ? sample : 0.f;
        const float value = output_[position_];
        for (unsigned c = 0; c < outChannels; ++c) out[i * outChannels + c] = value;
        if (++position_ == block) {
            position_ = 0;
            pendingWrite_ = (pendingWrite_ + 1) % pending_.size();
            if (filled_ < pending_.size()) ++filled_;
            if (filled_ == pending_.size()) {
                const auto& oldest = pending_[pendingWrite_];
                const bool available = enabled && oldest.timed && timeline_.render(
                    oldest.time, ticksPerSecond, left_.data(), right_.data(), block);
                if (enabled && !available) missing_.fetch_add(block, std::memory_order_relaxed);
                if (!enabled) output_ = oldest.samples;
                else canceller_.process(oldest.samples.data(), left_.data(), right_.data(),
                                        output_.data(), block, available);
            } else output_.fill(0);
        }
    }
}
}
