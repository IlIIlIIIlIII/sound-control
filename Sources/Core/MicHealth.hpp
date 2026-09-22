#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

namespace personaltools {

struct EchoDelayObservation {
    bool fresh = false;
    bool confident = false;
    double delayMs = 0;
    double correlation = 0;
    double validHistoryMs = 0;
    unsigned rejectedPoints = 0;
};

// The microphone callback is the sole producer. The control thread drains and
// correlates a bounded, decimated history. No audio is written to disk, and no
// allocation, lock, or correlation runs on the audio thread.
class EchoDelayMonitor {
public:
    static constexpr unsigned decimation = 24; // 48 kHz -> 2 kHz
    static constexpr unsigned capacity = 8192;
    static constexpr unsigned window = 2048;
    static constexpr unsigned maximumLag = 1600; // 800 ms, independent of AEC
    void push(const float* mic, const float* left, const float* right,
              unsigned frames, bool referenceAvailable);
    EchoDelayObservation analyze();
    // Call only while the producer is stopped.
    void reset();

private:
    struct Point { float mic, left, right; std::uint64_t sequence; bool continuous, valid; };
    std::array<Point, capacity> queue_{};
    std::atomic<std::uint64_t> write_{0}, read_{0};
    std::uint64_t sequence_ = 0, lastSequence_ = 0;
    unsigned phase_ = 0;
    std::array<float, 3> low_{}, dc_{}, sum_{};
    bool continuous_ = true, valid_ = true;
    std::array<Point, capacity> history_{};
    unsigned historySize_ = 0, historyWrite_ = 0, freshPoints_ = 0;
};

enum class MicHealthDecision { unavailable, healthy, warning, recover, limited };

class MicRecoveryPolicy {
public:
    MicHealthDecision observe(const EchoDelayObservation& observation, double now);
    void resetEvidence();
    // Preserve the rate limit when capture is rebuilt.
    void recoveryStarted(double now);
private:
    unsigned consecutive_ = 0;
    double lastDelay_ = 0;
    std::vector<double> attempts_;
};

// Hardware actions are injected so failure/timeout paths can be tested without
// changing a user's audio devices. tick() is driven by a control-thread timer.
class MicRateRecovery {
public:
    struct IO {
        std::function<void()> stop;
        std::function<bool(double)> setRate;
        std::function<double()> rate;
        std::function<bool()> start;
    };
    enum class State { idle, alternate, restoring, captureStarted, failed };
    void begin(IO io, double now, double originalRate = 48000,
               double alternateRate = 44100);
    State tick(double now);
    void cancel(); // Restore the original rate; never restart during shutdown.
    State state() const { return state_; }
    bool active() const { return state_ == State::alternate || state_ == State::restoring; }
    bool alternateSucceeded() const { return alternateSucceeded_; }
private:
    void restore(double now);
    IO io_;
    State state_ = State::idle;
    double original_ = 48000, alternate_ = 44100, deadline_ = 0;
    bool alternateSucceeded_ = false;
};

} // namespace personaltools
