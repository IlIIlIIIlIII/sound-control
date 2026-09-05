#include "MicHealth.hpp"
#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace macsound;
static int failures = 0;
static void expect(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

static EchoDelayObservation signalTest(double delayMs, int mode = 0) {
    auto monitor = std::make_unique<EchoDelayMonitor>();
    constexpr unsigned n = 48000 * 4, block = 512;
    std::vector<float> l(n), r(n), m(n);
    uint32_t random = 971;
    auto noise = [&] { random = random * 1664525u + 1013904223u;
        return ((random >> 8) / 8388608.f - 1) * 0.2f; };
    const unsigned lag = delayMs * 48;
    for (unsigned i = 0; i < n; ++i) {
        l[i] = mode == 2 ? 0.15f * std::sin(i * 2 * 3.141592653589793 * 220 / 48000) : noise();
        r[i] = mode == 2 ? l[i] : noise();
        m[i] = mode == 1 ? noise() : 0.01f * noise();
        if (i >= lag + 37 && mode != 1)
            m[i] += -0.6f * r[i - lag] - 0.2f * r[i - lag - 37];
        if (mode == 3) l[i] = r[i] = 0;
        if (mode == 4 && i % 5000 == 0) m[i] = 0.99f;
        if (mode == 5 && i % 240 < 48) m[i] = 0.99f;
    }
    EchoDelayObservation result;
    for (unsigned i = 0; i + block <= n; i += block) {
        monitor->push(m.data()+i, l.data()+i, r.data()+i, block, true);
        if ((i / block) % 100 == 99) {
            auto value = monitor->analyze();
            if (value.fresh) result = value;
        }
    }
    auto value = monitor->analyze();
    if (value.fresh) result = value;
    return result;
}

int main() {
    for (double delay : {22., 323., 700.}) {
        const auto value = signalTest(delay);
        std::cout << "delay=" << delay << " measured=" << value.delayMs
                  << " correlation=" << value.correlation << '\n';
        expect(value.confident && std::abs(value.delayMs-delay) <= 1.5,
               "detect an acoustic path independently of the 256 ms canceller");
    }
    expect(!signalTest(323, 1).confident, "unrelated speech must not trigger recovery");
    expect(!signalTest(323, 2).confident, "periodic ambiguous audio must not trigger recovery");
    expect(!signalTest(323, 3).confident, "silent render must not trigger recovery");
    const auto sparsePeaks = signalTest(323, 4);
    expect(sparsePeaks.confident && std::abs(sparsePeaks.delayMs - 323) <= 1.5,
           "isolated input peaks are excluded without starving delay detection");
    expect(!signalTest(323, 5).confident, "frequent overload cannot trigger a hardware reset");

    auto monitor = std::make_unique<EchoDelayMonitor>();
    float silence[512]{};
    for (int i=0;i<500;++i) monitor->push(silence,silence,silence,512,true);
    expect(!monitor->analyze().confident, "queue overflow and silence are not a delay measurement");
    expect(!monitor->analyze().fresh, "a stalled producer cannot reuse an old observation");
    monitor->reset();
    monitor->push(silence,silence,silence,512,false);
    expect(!monitor->analyze().confident, "missing reference breaks the measurement history");

    MicRecoveryPolicy policy;
    EchoDelayObservation late{true,true,323,0.7};
    expect(policy.observe(late,0)==MicHealthDecision::warning, "first late observation only warns");
    expect(policy.observe(late,2)==MicHealthDecision::warning, "second late observation only warns");
    expect(policy.observe(late,4)==MicHealthDecision::recover, "three late observations request recovery");
    policy.recoveryStarted(4);
    policy.observe(late,6); policy.observe(late,8);
    expect(policy.observe(late,10)==MicHealthDecision::limited, "cooldown prevents restart loops");
    expect(policy.observe(late,125)==MicHealthDecision::recover, "cooldown expires");
    policy.recoveryStarted(125);
    policy.observe(late,246); policy.observe(late,248);
    expect(policy.observe(late,250)==MicHealthDecision::recover, "third attempt is permitted");
    policy.recoveryStarted(250);
    policy.observe(late,372); policy.observe(late,374);
    expect(policy.observe(late,376)==MicHealthDecision::limited, "three attempts per fifteen minutes");
    expect(policy.observe({true,true,22,0.7},378)==MicHealthDecision::healthy, "normal delay clears warning evidence");
    expect(policy.observe(late,380)==MicHealthDecision::warning, "healthy observation resets consecutive count");
    expect(policy.observe({true,false,323,0.1},382)==MicHealthDecision::unavailable, "low confidence breaks bad evidence");
    expect(policy.observe(late,384)==MicHealthDecision::warning, "speech cannot bridge bad evidence");

    for (int failure = 0; failure < 5; ++failure) {
        MicRateRecovery recovery;
        double rate = 48000;
        std::vector<int> actions;
        bool started = false;
        recovery.begin({
            [&] { actions.push_back(0); },
            [&](double value) { actions.push_back(value);
                if (failure==1 && value==44100) return false;
                if (failure==2 && value==48000) return false;
                if (failure!=3) rate=value;
                return true; },
            [&] { return rate; },
            [&] { started=true; actions.push_back(1); return failure!=4; }
        },0);
        recovery.tick(.1); recovery.tick(1.2); recovery.tick(2.4);
        expect(actions.front()==0, "capture stops before changing the rate");
        expect(actions.size()>=3 && actions[1]==44100 && actions[2]==48000,
               "restore original rate even after alternate failure or timeout");
        if (failure==0) expect(recovery.state()==MicRateRecovery::State::captureStarted && started,
                              "restart capture only after readback confirms restored rate");
        else expect(recovery.state()==MicRateRecovery::State::failed,
                    "rate errors, timeouts and start failure must not report success");
        if (failure==2) expect(!started, "never start capture at the wrong rate");
    }
    MicRateRecovery cancelled;
    double rate=48000; bool started=false;
    cancelled.begin({[]{},[&](double v){rate=v;return true;},[&]{return rate;},[&]{started=true;return true;}},0);
    cancelled.cancel();
    expect(rate==48000 && !started && !cancelled.active(), "shutdown restores rate without restarting capture");
    return failures ? 1 : 0;
}
