#include "Shared.hpp"
#include "LoopbackReference.hpp"
#include "REWParser.hpp"
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

// UI-only C ABI. The audio callback never loads the CLR or calls this library.
using namespace soundcontrol;
using namespace soundcontrol::win;
using Microsoft::WRL::ComPtr;
namespace {
struct Device { std::wstring id, name; };
std::wstring wide(const std::string& s) {
    const int n=MultiByteToWideChar(CP_UTF8,0,s.data(),static_cast<int>(s.size()),nullptr,0);
    std::wstring r(n,L' '); MultiByteToWideChar(CP_UTF8,0,s.data(),static_cast<int>(s.size()),r.data(),n); return r;
}
std::wstring quote(const std::wstring& s) {
    std::wostringstream out; out<<L'"';
    for(const auto c:s) {
        if(c==L'"'||c==L'\\') out<<L'\\'<<c;
        else if(c<32) out<<L"\\u"<<std::hex<<std::setw(4)<<std::setfill(L'0')<<static_cast<unsigned>(c);
        else out<<c;
    }
    out<<L'"'; return out.str();
}
std::vector<Device> devices(EDataFlow flow) {
    struct Apartment {
        HRESULT result=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        ~Apartment(){if(SUCCEEDED(result))CoUninitialize();}
    } apartment;
    ComPtr<IMMDeviceEnumerator> e;
    if(FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&e))))
        throw std::runtime_error("Audio device enumeration failed");
    ComPtr<IMMDeviceCollection> collection;
    if(FAILED(e->EnumAudioEndpoints(flow,DEVICE_STATE_ACTIVE,&collection))) throw std::runtime_error("Audio device enumeration failed");
    UINT count=0; collection->GetCount(&count); std::vector<Device> result;
    for(UINT i=0;i<count;++i) {
        ComPtr<IMMDevice> d; ComPtr<IPropertyStore> p; LPWSTR id=nullptr;
        if(FAILED(collection->Item(i,&d))||FAILED(d->GetId(&id))) continue;
        if(SUCCEEDED(d->OpenPropertyStore(STGM_READ,&p))) {
            PROPVARIANT v; PropVariantInit(&v);
            if(SUCCEEDED(p->GetValue(PKEY_Device_FriendlyName,&v))&&v.vt==VT_LPWSTR) result.push_back({id,v.pwszVal});
            PropVariantClear(&v);
        }
        CoTaskMemFree(id);
    }
    return result;
}
bool contains(const std::vector<Device>& list,const std::wstring& id) {
    for(const auto& d:list) if(d.id==id) return true;
    return false;
}
std::wstring deviceJson(const std::vector<Device>& list) {
    std::wstring r=L"[";
    for(const auto& d:list) { if(r.size()>1)r+=L","; r+=L"{\"id\":"+quote(d.id)+L",\"name\":"+quote(d.name)+L"}"; }
    return r+L"]";
}
std::wstring profile(const std::filesystem::path& path,Channel channel) {
    const auto parsed=parseREWConfigurablePEQFile(path,channel);
    if(!parsed) return L"{\"valid\":false,\"summary\":\"REW 필터 파일을 준비해 주세요\",\"filters\":[]}";
    std::wstring r=L"{\"valid\":true,\"summary\":"+quote(std::to_wstring(parsed.filters.size())+L"개 필터")+L",\"filters\":[";
    for(std::size_t i=0;i<parsed.filters.size();++i) {
        const auto& f=parsed.filters[i]; if(i)r+=L",";
        std::wostringstream row; row.imbue(std::locale::classic());
        row<<L"{\"frequency\":"<<f.frequencyHz<<L",\"gain\":"<<f.gainDB<<L",\"q\":"<<f.q<<L"}"; r+=row.str();
    }
    return r+L"]}";
}
std::wstring meter(const Meter& m,bool connected) {
    LARGE_INTEGER now{},frequency{}; QueryPerformanceCounter(&now);QueryPerformanceFrequency(&frequency);
    const auto last=readWide(&m.lastQpc);
    const bool active=connected&&last>0&&now.QuadPart>=last&&now.QuadPart-last<=frequency.QuadPart*3;
    std::wstring state=!connected?L"장치 연결 대기":!active?L"오디오 처리 확인 대기":readWord(&m.enabled)?L"처리 중":L"바이패스";
    if(active&&readWord(&m.enabled)&&readWord(&m.neuralState)!=0){
        const auto neural=readWord(&m.neuralState);
        state=neural<0?L"NPU 오류 · 원음 출력":neural==1?L"NPU 모델 준비 중":
            readWord(&m.reference)?L"DTLN-AEC 256 · NPU 처리 중":L"NPU · 스피커 참조 대기";
    }
    return L"{\"state\":"+quote(state)+L",\"active\":"+(active?L"true":L"false")+
        L",\"rate\":"+std::to_wstring(readWord(&m.rate))+L",\"error\":"+std::to_wstring(readWord(&m.error))+L"}";
}
struct Session {
    SharedFile shared; std::wstring result,lastLog;
    LoopbackReference reference;
    Session(){reference.start();}
};
void logStatus(Session& s,const std::wstring& status) {
    if(status==s.lastLog) return;
    s.lastLog=status;
    const auto path=dataDirectory()/L"events.txt";
    std::vector<std::string> lines; std::ifstream input(path,std::ios::binary); std::string line;
    while(std::getline(input,line))lines.push_back(line); input.close();
    SYSTEMTIME t{};GetLocalTime(&t);wchar_t stamp[40]{};
    swprintf_s(stamp,L"%04u-%02u-%02u %02u:%02u:%02u ",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond);
    const auto text=std::wstring(stamp)+status;
    const int n=WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
    std::string utf8(n,' ');WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),utf8.data(),n,nullptr,nullptr);
    lines.push_back(utf8);if(lines.size()>64)lines.erase(lines.begin(),lines.end()-64);
    std::ofstream out(path,std::ios::binary|std::ios::trunc);for(const auto& item:lines)out<<item<<'\n';
}
}
#define API extern "C" __declspec(dllexport)
API void* __cdecl MT_Create() noexcept { try{return new Session;}catch(...){return nullptr;} }
API void __cdecl MT_Destroy(void* handle) noexcept { delete static_cast<Session*>(handle); }
// The returned UTF-16 string is owned by the session until the next call.
API const wchar_t* __cdecl MT_Snapshot(void* handle) noexcept {
    auto& s=*static_cast<Session*>(handle);
    try {
        Configuration c;LONG revision=0;
        const bool ready=s.shared.open()&&s.shared.read(c,revision);
        const auto outputs=devices(eRender),inputs=devices(eCapture);
        const auto directory=ready?dataDirectory():std::filesystem::path(L"E:\\Speaker");
        Meter empty{};const auto* data=ready?s.shared.data():nullptr;
        const auto render=meter(data?data->render:empty,contains(outputs,c.renderId));
        const auto capture=meter(data?data->capture:empty,contains(inputs,c.captureId));
        s.result=L"{\"ready\":"+std::wstring(ready?L"true":L"false")+L",\"revision\":"+std::to_wstring(revision)+
            L",\"eqEnabled\":"+(c.eqEnabled?L"true":L"false")+L",\"aecEnabled\":"+(c.aecEnabled?L"true":L"false")+
            L",\"renderId\":"+quote(c.renderId)+L",\"captureId\":"+quote(c.captureId)+
            L",\"outputs\":"+deviceJson(outputs)+L",\"inputs\":"+deviceJson(inputs)+
            L",\"left\":"+profile(directory/L"L.txt",Channel::left)+L",\"right\":"+profile(directory/L"R.txt",Channel::right)+
            L",\"render\":"+render+L",\"capture\":"+capture+
            L",\"reference\":"+((data&&readWord(&data->capture.reference))?L"true":L"false")+
            L",\"clipping\":"+((data&&readWord(&data->capture.clipping))?L"true":L"false")+L"}";
        if(ready)logStatus(s,L"EQ "+render+L" / AEC "+capture);
    }catch(const std::exception& e){s.result=L"{\"error\":"+quote(wide(e.what()))+L"}";}
    catch(...){s.result=L"{\"error\":\"오디오 상태를 읽지 못했습니다\"}";}
    return s.result.c_str();
}
API const wchar_t* __cdecl MT_SetEnabled(void* handle,int eq,int aec) noexcept {
    auto& s=*static_cast<Session*>(handle);s.result.clear();
    try {
        Configuration c;LONG rev=0;
        if(!s.shared.open()||!s.shared.read(c,rev))throw std::runtime_error("먼저 설치 및 적용을 완료해 주세요.");
        c.eqEnabled=eq!=0;c.aecEnabled=aec!=0;
        if(!s.shared.write(c))throw std::runtime_error("설정을 저장하지 못했습니다. 다시 시도해 주세요.");
    }catch(const std::exception& e){s.result=wide(e.what());}catch(...){s.result=L"설정 저장 실패";}
    return s.result.c_str();
}
API const wchar_t* __cdecl MT_Import(void* handle,int channel,const wchar_t* path) noexcept {
    auto& s=*static_cast<Session*>(handle);s.result.clear();
    try {
        Configuration c;LONG rev=0;
        if(!path||(channel!=0&&channel!=1))throw std::runtime_error("Invalid import argument");
        if(!s.shared.open()||!s.shared.read(c,rev))throw std::runtime_error("먼저 설치 및 적용을 완료해 주세요.");
        const auto ch=channel==0?Channel::left:Channel::right;
        auto parsed=parseREWConfigurablePEQFile(path,ch);if(!parsed)throw std::runtime_error(parsed.error);
        auto other=parseREWConfigurablePEQFile(dataDirectory()/(channel==0?L"R.txt":L"L.txt"),channel==0?Channel::right:Channel::left);
        if(!other)throw std::runtime_error(other.error);
        std::string error;
        if(!prepareConfiguration(c,channel==0?parsed.filters:other.filters,channel==0?other.filters:parsed.filters,error))throw std::runtime_error(error);
        parsed=importREWConfigurablePEQFile(path,dataDirectory()/(channel==0?L"L.txt":L"R.txt"),ch);
        if(!parsed)throw std::runtime_error(parsed.error);
        if(!s.shared.write(c))throw std::runtime_error("설정 갱신 실패. 파일을 다시 가져와 주세요.");
    }catch(const std::exception& e){s.result=wide(e.what());}catch(...){s.result=L"필터 가져오기 실패";}
    return s.result.c_str();
}
API int __cdecl MT_Initialize(const wchar_t* render,const wchar_t* capture) noexcept {
    try {
        if(!render||!capture||wcslen(render)>=512||wcslen(capture)>=512)return 7;
        if(!contains(devices(eRender),render)||!contains(devices(eCapture),capture))return 3;
        SharedFile shared;Configuration c;LONG revision=0;
        if(!shared.open(true)||!shared.read(c,revision))return 4;
        const auto dir=dataDirectory();
        auto l=parseREWConfigurablePEQFile(dir/L"L.txt",Channel::left);
        auto r=parseREWConfigurablePEQFile(dir/L"R.txt",Channel::right);
        if(!l)l=importREWConfigurablePEQFile(L"E:\\Speaker\\L.txt",dir/L"L.txt",Channel::left);
        if(!r)r=importREWConfigurablePEQFile(L"E:\\Speaker\\R.txt",dir/L"R.txt",Channel::right);
        if(!l||!r)return 5;
        std::string error;if(!prepareConfiguration(c,l.filters,r.filters,error))return 6;
        wcscpy_s(c.renderId,render);wcscpy_s(c.captureId,capture);c.eqEnabled=c.aecEnabled=1;
        return shared.write(c)?0:8;
    }catch(...){return 1;}
}
// Called by the protected recovery helper. Preserve filters and user toggles;
// refuse stale requests instead of overwriting a concurrent settings update.
API int __cdecl MT_RebindRender(const wchar_t* previous,const wchar_t* render,const wchar_t* capture) noexcept {
    try {
        if(!previous||!render||!capture||wcslen(render)>=512)return 7;
        if(!contains(devices(eRender),render))return 3;
        SharedFile shared;Configuration c;LONG revision=0;
        if(!shared.open()||!shared.read(c,revision))return 4;
        if(wcscmp(c.captureId,capture)!=0)return 9;
        if(wcscmp(c.renderId,render)==0)return 0;
        if(wcscmp(c.renderId,previous)!=0)return 9;
        wcscpy_s(c.renderId,render);
        return shared.write(c,revision)?0:8;
    }catch(...){return 1;}
}
