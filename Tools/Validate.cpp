#include "DSP.hpp"
#include "REWParser.hpp"

#include <cstdlib>
#include <iomanip>
#include <iostream>

int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        std::cerr << "Usage: MacToolsValidate L.txt R.txt [sample-rate]\n";
        return 2;
    }
    const double sampleRate = argc == 4 ? std::strtod(argv[3], nullptr) : 48000.0;
    const auto left = macsound::parseREWConfigurablePEQFile(argv[1], macsound::Channel::left);
    const auto right = macsound::parseREWConfigurablePEQFile(argv[2], macsound::Channel::right);
    if (!left) {
        std::cerr << "L: " << left.error << '\n';
        return 1;
    }
    if (!right) {
        std::cerr << "R: " << right.error << '\n';
        return 1;
    }

    macsound::StereoDSP dsp;
    std::string error;
    if (!dsp.configure(left.filters, right.filters, sampleRate, error)) {
        std::cerr << error << '\n';
        return 1;
    }

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "sampleRate=" << sampleRate << "\n";
    std::cout << "leftBands=" << left.filters.size() << "\n";
    for (const auto &filter : left.filters) {
        std::cout << "L PK " << filter.frequencyHz << "Hz "
                  << filter.gainDB << "dB Q" << filter.q << "\n";
    }
    std::cout << "rightBands=" << right.filters.size() << "\n";
    for (const auto &filter : right.filters) {
        std::cout << "R PK " << filter.frequencyHz << "Hz "
                  << filter.gainDB << "dB Q" << filter.q << "\n";
    }
    std::cout << "automaticPreampDB=" << dsp.preampDB() << "\n";
    return 0;
}
