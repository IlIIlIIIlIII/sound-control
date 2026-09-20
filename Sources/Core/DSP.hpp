#pragma once

#include "REWParser.hpp"

#include "SignalOps.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace soundcontrol {

struct BiquadCoefficients {
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
};

BiquadCoefficients peakingCoefficients(const PEQFilter& filter, double sampleRate);
double responseDB(const std::vector<PEQFilter>& filters, double frequencyHz, double sampleRate);
double automaticPreampDB(const std::vector<PEQFilter>& left,
                         const std::vector<PEQFilter>& right,
                         double sampleRate);

class StereoDSP {
public:
    StereoDSP() = default;
    ~StereoDSP();
    StereoDSP(const StereoDSP&) = delete;
    StereoDSP& operator=(const StereoDSP&) = delete;

    bool configure(const std::vector<PEQFilter>& left,
                   const std::vector<PEQFilter>& right,
                   double sampleRate,
                   std::string& error);
    void reset();
    void processPlanar(double* left, double* right, std::size_t frames);
    void processInterleaved(double* stereo, std::size_t frames);

    double preampDB() const { return preampDB_; }
    double linearPreamp() const { return linearPreamp_; }
    bool configured() const { return configured_; }

private:
    void destroy();

    signal::BiquadSetup leftSetup_ = nullptr;
    signal::BiquadSetup rightSetup_ = nullptr;
    std::vector<double> leftDelay_;
    std::vector<double> rightDelay_;
    double preampDB_ = 0.0;
    double linearPreamp_ = 1.0;
    bool configured_ = false;
};

}  // namespace soundcontrol
