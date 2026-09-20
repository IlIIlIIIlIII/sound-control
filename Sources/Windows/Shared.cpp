#include "Shared.hpp"
#include <shlobj.h>
#include <cmath>

namespace macsound::win {
std::filesystem::path dataDirectory() {
#ifdef MACTOOLS_TESTING
    wchar_t root[32768]{};
    GetTempPathW(32768,root);
    return std::filesystem::path(root)/(L"MacTools-contract-"+std::to_wstring(GetCurrentProcessId()));
#endif
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &path))) return {};
    const auto result = std::filesystem::path(path) / L"MacTools";
    CoTaskMemFree(path); return result;
}
SharedFile::~SharedFile() {
    if (data_) UnmapViewOfFile(data_);
    if (mapping_) CloseHandle(mapping_);
    if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
}
bool SharedFile::open(bool create) {
    if (data_) return readWord(&data_->magic) == sharedMagic;
    if (mapping_) { CloseHandle(mapping_); mapping_ = nullptr; }
    if (file_ != INVALID_HANDLE_VALUE) { CloseHandle(file_); file_ = INVALID_HANDLE_VALUE; }
    const auto directory = dataDirectory();
    if (directory.empty()) return false;
    std::error_code error;
    if (create) std::filesystem::create_directories(directory, error);
    if (error) return false;
    file_ = CreateFileW((directory / L"state-v1.bin").c_str(), GENERIC_READ|GENERIC_WRITE,
                        FILE_SHARE_READ|FILE_SHARE_WRITE, nullptr,
                        create ? OPEN_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file_, &size)) return false;
    if (size.QuadPart != sizeof(SharedData)) {
        if (!create || size.QuadPart != 0) return false;
        size.QuadPart = sizeof(SharedData);
        if (!SetFilePointerEx(file_, size, nullptr, FILE_BEGIN) || !SetEndOfFile(file_)) return false;
    }
    mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READWRITE, 0, sizeof(SharedData), nullptr);
    if (!mapping_) return false;
    data_ = static_cast<SharedData*>(MapViewOfFile(mapping_,FILE_MAP_ALL_ACCESS,0,0,sizeof(SharedData)));
    if (!data_) return false;
    if (create && readWord(&data_->magic) == 0) {
        Configuration config;
        if (!write(config)) return false;
        InterlockedExchange(&data_->magic, sharedMagic);
    }
    if (readWord(&data_->magic) != sharedMagic) {
        UnmapViewOfFile(data_); data_=nullptr;
        return false;
    }
    return true;
}
bool SharedFile::read(Configuration& out, LONG& revision) const {
    if (!data_) return false;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        const LONG before = readWord(&data_->sequence);
        if (!before || (before & 1)) continue;
        std::array<LONG,sizeof(Configuration)/sizeof(LONG)> snapshot{};
        for (std::size_t i = 0; i < snapshot.size(); ++i) snapshot[i] = readWord(&data_->words[i]);
        if (before == readWord(&data_->sequence)) {
            std::memcpy(&out, snapshot.data(), sizeof(out));
            if (out.version != 1 || out.count[0] > 32 || out.count[1] > 32) return false;
            revision = before; return true;
        }
    }
    return false;
}
bool SharedFile::write(const Configuration& config) {
    if (!data_) return false;
    LONG seq = readWord(&data_->sequence);
    if ((seq & 1) || InterlockedCompareExchange(&data_->sequence, seq + 1, seq) != seq) return false;
    std::array<LONG,sizeof(Configuration)/sizeof(LONG)> snapshot{};
    std::memcpy(snapshot.data(), &config, sizeof(config));
    for (std::size_t i = 0; i < snapshot.size(); ++i) InterlockedExchange(&data_->words[i], snapshot[i]);
    InterlockedExchange(&data_->sequence, seq + 2); return true;
}
bool prepareConfiguration(Configuration& cfg, const std::vector<PEQFilter>& left,
                          const std::vector<PEQFilter>& right, std::string& error) {
    if (left.empty() || right.empty() || left.size()>32 || right.size()>32) {
        error = "Both channels require 1 to 32 filters"; return false;
    }
    Configuration next = cfg;
    for (std::size_t r = 0; r < rates.size(); ++r) {
        StereoDSP validate;
        if (!validate.configure(left,right,rates[r],error)) return false;
        next.banks[r].gain = validate.linearPreamp();
        for (std::size_t i=0;i<left.size();++i) next.banks[r].coefficients[0][i]=peakingCoefficients(left[i],rates[r]);
        for (std::size_t i=0;i<right.size();++i) next.banks[r].coefficients[1][i]=peakingCoefficients(right[i],rates[r]);
    }
    next.count = {static_cast<DWORD>(left.size()),static_cast<DWORD>(right.size())};
    cfg = next; return true;
}
bool RealtimeEQ::configure(const Configuration& cfg, unsigned rate) {
    const auto it = std::find(rates.begin(),rates.end(),rate);
    if (it == rates.end() || cfg.count[0]>32 || cfg.count[1]>32) return false;
    const auto& bank = cfg.banks[it-rates.begin()];
    if (!std::isfinite(bank.gain) || bank.gain<0 || bank.gain>1) return false;
    for (unsigned ch=0;ch<2;++ch) for (unsigned s=0;s<cfg.count[ch];++s) {
        const auto& c=bank.coefficients[ch][s];
        if (!std::isfinite(c.b0)||!std::isfinite(c.b1)||!std::isfinite(c.b2)||
            !std::isfinite(c.a1)||!std::isfinite(c.a2)||std::abs(c.a2)>=1 ||
            1+c.a1+c.a2<=0 || 1-c.a1+c.a2<=0) return false;
    }
    bank_=bank;count_=cfg.count;reset();return true;
}
void RealtimeEQ::reset() { state_ = {}; }
void RealtimeEQ::process(const float* input,float* output,std::size_t frames,bool silent,bool enabled) {
    for(std::size_t i=0;i<frames;++i) for(unsigned ch=0;ch<2;++ch) {
        double x=silent?0:input[i*2+ch];
        if (!std::isfinite(x)) x=0;
        if(enabled) {
            for(unsigned s=0;s<count_[ch];++s) {
                const auto& c=bank_.coefficients[ch][s];auto& z=state_[ch][s];
                const double y=c.b0*x+z[0];z[0]=c.b1*x-c.a1*y+z[1];z[1]=c.b2*x-c.a2*y;x=y;
            }
            x*=bank_.gain;
        }
        output[i*2+ch]=static_cast<float>(x);
    }
}
}
