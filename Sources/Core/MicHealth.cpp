#include "MicHealth.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace macsound {

void EchoDelayMonitor::push(const float* mic, const float* left, const float* right,
                           unsigned frames, bool referenceAvailable) {
    if (!mic || !left || !right) return;
    for (unsigned i = 0; i < frames; ++i) {
        const float values[3] = {mic[i], left[i], right[i]};
        continuous_ = continuous_ && referenceAvailable;
        valid_ = valid_ && std::abs(mic[i]) < 0.9440609f;
        for (unsigned ch = 0; ch < 3; ++ch) {
            if (!std::isfinite(values[ch])) continuous_ = false;
            const float value = std::isfinite(values[ch]) ? values[ch] : 0;
            low_[ch] += 0.05f * (value - low_[ch]);
            dc_[ch] += 0.004f * (low_[ch] - dc_[ch]);
            sum_[ch] += low_[ch] - dc_[ch];
        }
        if (++phase_ != decimation) continue;
        const auto write = write_.load(std::memory_order_relaxed);
        const auto read = read_.load(std::memory_order_acquire);
        ++sequence_;
        if (write - read < capacity) {
            queue_[write % capacity] = {sum_[0] / decimation, sum_[1] / decimation,
                sum_[2] / decimation, sequence_, continuous_, valid_};
            write_.store(write + 1, std::memory_order_release);
        }
        sum_.fill(0);
        phase_ = 0;
        continuous_ = valid_ = true;
    }
}

void EchoDelayMonitor::reset() {
    write_.store(0); read_.store(0);
    sequence_ = lastSequence_ = 0;
    phase_ = historySize_ = historyWrite_ = freshPoints_ = 0;
    low_.fill(0); dc_.fill(0); sum_.fill(0); continuous_ = valid_ = true;
}

EchoDelayObservation EchoDelayMonitor::analyze() {
    EchoDelayObservation result;
    auto read = read_.load(std::memory_order_relaxed);
    const auto write = write_.load(std::memory_order_acquire);
    while (read != write) {
        const Point point = queue_[read++ % capacity];
        if (!point.continuous || (lastSequence_ && point.sequence != lastSequence_ + 1)) {
            historySize_ = freshPoints_ = 0;
        }
        if (!point.continuous || !point.valid) ++result.rejectedPoints;
        lastSequence_ = point.sequence;
        if (point.continuous) {
            history_[historyWrite_++ % capacity] = point;
            historySize_ = std::min(capacity, historySize_ + 1);
            ++freshPoints_;
        }
    }
    read_.store(read, std::memory_order_release);
    result.validHistoryMs = historySize_ * 0.5;
    if (freshPoints_ < window) return result;
    result.fresh = true;
    freshPoints_ = 0;
    if (historySize_ < window + maximumLag) return result;

    // Linearize only the needed history on the control thread.
    constexpr unsigned count = window + maximumLag;
    std::array<float, count> l{}, r{}, m{};
    std::array<bool, window> validMic{};
    for (unsigned i = 0; i < count; ++i) {
        const auto &p = history_[(historyWrite_ - count + i) % capacity];
        l[i] = p.left; r[i] = p.right; m[i] = p.mic;
        if (i >= maximumLag) validMic[i - maximumLag] = p.valid;
    }
    double micEnergy = 0, micSum = 0;
    unsigned usable = 0;
    for (unsigned i = maximumLag; i < count; ++i) {
        if (!validMic[i - maximumLag]) continue;
        micEnergy += m[i] * m[i]; micSum += m[i];
        ++usable;
    }
    // An isolated peak must not discard seconds of otherwise useful history.
    // Exclude overloaded microphone points from every lag's statistics, and
    // reject the entire observation if more than 5% of its points overloaded.
    if (usable < window * 0.95) return result;
    micEnergy -= micSum * micSum / usable;
    if (micEnergy / usable < 1e-9) return result;
    std::array<double, maximumLag + 1> scores{};
    unsigned bestLag = 0;
    for (unsigned lag = 0; lag <= maximumLag; ++lag) {
        for (const auto* ref : {l.data(), r.data()}) {
            double xy = 0, xx = 0, sum = 0;
            for (unsigned i = 0; i < window; ++i) {
                if (!validMic[i]) continue;
                const float x = ref[maximumLag - lag + i];
                xy += x * m[maximumLag + i]; xx += x * x; sum += x;
            }
            xx -= sum * sum / usable;
            xy -= sum * micSum / usable;
            if (xx / usable < 1e-8) continue;
            scores[lag] = std::max(scores[lag], std::abs(xy) /
                std::sqrt(std::max(xx * micEnergy, 1e-30)));
        }
        if (scores[lag] > scores[bestLag]) bestLag = lag;
    }
    double competitor = 0;
    for (unsigned lag = 0; lag <= maximumLag; ++lag) {
        if (std::abs(static_cast<int>(lag) - static_cast<int>(bestLag)) > 40)
            competitor = std::max(competitor, scores[lag]);
    }
    result.delayMs = bestLag * 0.5;
    result.correlation = scores[bestLag];
    // A pure tone has many equally plausible delays. Speech unrelated to the
    // render also must not trigger a hardware reset merely because AEC is weak.
    result.confident = bestLag < maximumLag - 4 && scores[bestLag] >= 0.30 &&
        scores[bestLag] - competitor >= 0.08;
    return result;
}

void MicRecoveryPolicy::resetEvidence() { consecutive_ = 0; lastDelay_ = 0; }

void MicRecoveryPolicy::recoveryStarted(double now) {
    attempts_.push_back(now);
    resetEvidence();
}

MicHealthDecision MicRecoveryPolicy::observe(const EchoDelayObservation& value, double now) {
    if (!value.fresh) { resetEvidence(); return MicHealthDecision::unavailable; }
    if (!value.confident) { resetEvidence(); return MicHealthDecision::unavailable; }
    if (value.delayMs < 256) {
        resetEvidence();
        return value.delayMs >= 200 ? MicHealthDecision::warning : MicHealthDecision::healthy;
    }
    if (consecutive_ && std::abs(value.delayMs - lastDelay_) > 30) consecutive_ = 0;
    lastDelay_ = value.delayMs;
    ++consecutive_;
    if (consecutive_ < 3) return MicHealthDecision::warning;
    attempts_.erase(std::remove_if(attempts_.begin(), attempts_.end(),
        [now](double t) { return now - t >= 900; }), attempts_.end());
    if ((!attempts_.empty() && now - attempts_.back() < 120) || attempts_.size() >= 3)
        return MicHealthDecision::limited;
    return MicHealthDecision::recover;
}

void MicRateRecovery::begin(IO io, double now, double originalRate, double alternateRate) {
    if (active()) return;
    io_ = std::move(io); original_ = originalRate; alternate_ = alternateRate;
    alternateSucceeded_ = false;
    io_.stop();
    state_ = State::alternate;
    deadline_ = now + 1.0;
    if (!io_.setRate(alternate_)) restore(now);
}

void MicRateRecovery::restore(double now) {
    state_ = State::restoring;
    deadline_ = now + 1.0;
    // Always try restoring even if changing to the alternate rate failed.
    (void)io_.setRate(original_);
}

MicRateRecovery::State MicRateRecovery::tick(double now) {
    if (!active()) return state_;
    const double rate = io_.rate();
    if (state_ == State::alternate) {
        if (std::isfinite(rate) && std::abs(rate - alternate_) < 0.5) {
            alternateSucceeded_ = true;
            restore(now);
        } else if (now >= deadline_) restore(now);
    } else if (std::isfinite(rate) && std::abs(rate - original_) < 0.5) {
        const bool started = io_.start();
        state_ = started && alternateSucceeded_ ? State::captureStarted : State::failed;
    } else if (now >= deadline_) {
        (void)io_.setRate(original_);
        state_ = State::failed;
    }
    if (!active()) io_ = {};
    return state_;
}

void MicRateRecovery::cancel() {
    if (active()) (void)io_.setRate(original_);
    state_ = State::idle;
    io_ = {};
}

} // namespace macsound
