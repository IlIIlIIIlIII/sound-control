#pragma once
#include <windows.h>
#include "DSP.hpp"
#include <array>
#include <cstring>
#include <filesystem>
#include <type_traits>

namespace personaltools::win {
inline constexpr GUID eqClass{0x4538bfc1,0xcced,0x4c3d,{0xa9,0x81,0x89,0x5a,0x73,0x71,0x12,0x01}};
inline constexpr GUID aecClass{0x4538bfc1,0xcced,0x4c3d,{0xa9,0x81,0x89,0x5a,0x73,0x71,0x12,0x02}};
inline constexpr wchar_t eqClassText[] = L"{4538BFC1-CCED-4C3D-A981-895A73711201}";
inline constexpr wchar_t aecClassText[] = L"{4538BFC1-CCED-4C3D-A981-895A73711202}";
inline constexpr std::array<unsigned, 6> rates{44100,48000,88200,96000,176400,192000};
struct EqBank {
    std::array<std::array<BiquadCoefficients,32>,2> coefficients{};
    double gain = 1;
};
struct Configuration {
    DWORD version = 1, eqEnabled = 0, aecEnabled = 0;
    std::array<DWORD,2> count{};
    std::array<EqBank,6> banks{};
    wchar_t renderId[512]{}, captureId[512]{};
};
static_assert(std::is_trivially_copyable_v<Configuration>);
static_assert(sizeof(Configuration) % sizeof(LONG) == 0);
struct alignas(64) Meter {
    volatile LONG64 callbacks = 0, lastQpc = 0, frames = 0;
    volatile LONG rate = 0, enabled = 0, reference = 0, clipping = 0;
    volatile LONG reductionDB100 = 0, error = 0;
    volatile LONG64 missingFrames = 0, dropped = 0, maxCallbackTicks = 0;
    // Use the existing alignment padding; preserve the v1 shared-file ABI.
    volatile LONG64 diagnosticInstance = 0, microphoneTime = 0, referenceTime = 0;
    volatile LONG inputFrames = 0, inputSignature = 0, linearReductionDB100 = 0;
    volatile LONG neuralState = 0;
    volatile LONG64 neuralBlocks = 0;
};
static_assert(sizeof(Meter) == 128);
struct SharedData {
    volatile LONG magic = 0;
    volatile LONG sequence = 0;
    volatile LONG words[sizeof(Configuration)/sizeof(LONG)]{};
    Meter render{}, capture{};
};
inline constexpr LONG sharedMagic = 0x4d535701;
inline LONG readWord(const volatile LONG* p) { return InterlockedCompareExchange(const_cast<volatile LONG*>(p),0,0); }
inline LONG64 readWide(const volatile LONG64* p) { return InterlockedCompareExchange64(const_cast<volatile LONG64*>(p),0,0); }
std::filesystem::path dataDirectory();
class SharedFile {
public:
    ~SharedFile();
    bool open(bool create = false);
    bool read(Configuration& out, LONG& revision) const;
    bool write(const Configuration& config, LONG expectedRevision = 0);
    SharedData* data() const { return data_; }
private:
    HANDLE file_ = INVALID_HANDLE_VALUE, mapping_ = nullptr;
    SharedData* data_ = nullptr;
};
bool prepareConfiguration(Configuration& config, const std::vector<PEQFilter>& left,
                          const std::vector<PEQFilter>& right, std::string& error);
class RealtimeEQ {
public:
    bool configure(const Configuration& config, unsigned rate);
    void process(const float* input, float* output, std::size_t frames, bool silent, bool enabled);
    void reset();
private:
    EqBank bank_{};
    std::array<DWORD,2> count_{};
    std::array<std::array<std::array<double,2>,32>,2> state_{};
};
}
