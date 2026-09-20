// Same APO implementation, with an isolated temporary settings directory.
// This tests COM/APO contracts; it does not register any endpoint or load audiodg.
#include "../Sources/Windows/Apo.cpp"
#include <fstream>
#include <iostream>
#include <limits>
using namespace soundcontrol;
using namespace soundcontrol::win;
namespace {
int failures=0;
void expect(bool ok,const char* text){if(!ok){std::cerr<<"FAIL: "<<text<<'\n';++failures;}}
IAudioMediaType* makeMedia(unsigned channels,unsigned rate=48000) {
    UNCOMPRESSEDAUDIOFORMAT f{floatFormat,channels,4,32,static_cast<float>(rate),channels==1?4u:3u};
    IAudioMediaType* media=nullptr;const auto factory=mediaFactory();
    if(factory)factory(&f,&media);return media;
}
Apo* initialize(bool capture) {
    auto* apo=new Apo(capture);
    APOInitSystemEffects3 init{};init.APOInit.clsid=capture?aecClass:eqClass;init.InitializeForDiscoveryOnly=TRUE;
    expect(apo->Initialize(sizeof(init),reinterpret_cast<BYTE*>(&init))==S_OK,"APO Initialize with Windows 11 discovery context");
    return apo;
}
void dllTest() {
    wchar_t path[32768]{}; GetModuleFileNameW(nullptr,path,32768);
    const auto dll=std::filesystem::path(path).parent_path()/L"SoundControlAPO.dll";
    HMODULE module=LoadLibraryExW(dll.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    expect(module!=nullptr,"built APO DLL loads with system-only dependency search");
    if(!module)return;
    using GetFactory=HRESULT(WINAPI*)(REFCLSID,REFIID,void**);
    using CanUnload=HRESULT(WINAPI*)();
    auto get=reinterpret_cast<GetFactory>(GetProcAddress(module,"DllGetClassObject"));
    auto unload=reinterpret_cast<CanUnload>(GetProcAddress(module,"DllCanUnloadNow"));
    expect(get&&unload,"COM exports present in built DLL");
    if(get&&unload) for(const auto& id:{eqClass,aecClass}) {
        IClassFactory* factory=nullptr;
        expect(get(id,__uuidof(IClassFactory),reinterpret_cast<void**>(&factory))==S_OK,"DLL class factory");
        if(factory) {
            IAudioProcessingObject* instance=nullptr;
            expect(factory->CreateInstance(nullptr,__uuidof(IAudioProcessingObject),reinterpret_cast<void**>(&instance))==S_OK,"DLL APO instance");
            expect(unload()==S_FALSE,"live objects prevent unload");
            if(instance)instance->Release();factory->Release();
        }
        expect(unload()==S_OK,"DLL can unload after COM release");
    }
    FreeLibrary(module);
}
void imports() {
    const auto root=dataDirectory()/L"\uc2a4\ud53c\ucee4";
    std::filesystem::create_directories(root);
    const auto source=root/L"source.txt",dest=root/L"L.txt";
    {std::ofstream f(source);f<<"Notes:left\nConfigurable_PEQ\n1 True Auto PK 66 -12 7.4\n";}
    expect(bool(importREWConfigurablePEQFile(source,dest,Channel::left)),"Unicode path import");
    {std::ofstream f(source);f<<"Notes:left\nConfigurable_PEQ\n1 True Auto PK 140 -6 3.21\n";}
    expect(bool(importREWConfigurablePEQFile(source,dest,Channel::left)),"Windows replaces an existing imported file");
    {std::ofstream f(source);f<<"invalid";}
    expect(!importREWConfigurablePEQFile(source,dest,Channel::left),"invalid replacement rejected");
    const auto parsed=parseREWConfigurablePEQFile(dest,Channel::left);
    expect(parsed&&parsed.filters.front().frequencyHz==140,"last valid filter survives failed import");
}
void eqTest(SharedFile& state,Configuration cfg) {
    auto* apo=initialize(false);auto* media=makeMedia(2);
    expect(media!=nullptr,"Windows media type factory available");if(!media){apo->Release();return;}
    HANDLE changed=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    AUDIO_SYSTEMEFFECT* effects=nullptr;UINT count=0;
    expect(apo->GetControllableSystemEffectsList(&effects,&count,changed)==S_OK && count==1,
           "effects discovery supplies supported control");
    CoTaskMemFree(effects);
    expect(apo->SetAudioSystemEffectState(eqClass,AUDIO_SYSTEMEFFECT_STATE_OFF)==S_OK &&
           WaitForSingleObject(changed,0)==WAIT_OBJECT_0,"effect state changes signal the duplicated event");
    apo->SetAudioSystemEffectState(eqClass,AUDIO_SYSTEMEFFECT_STATE_ON);CloseHandle(changed);
    void* marker=nullptr;
    expect(apo->QueryInterface(__uuidof(IApoAcousticEchoCancellation),&marker)==E_NOINTERFACE,"render APO must not advertise capture-only AEC interface");
    std::array<float,1024> samples{};
    for(std::size_t i=0;i<512;++i){samples[i*2]=0.2f*std::sin(i*0.2f);samples[i*2+1]=0.3f*std::sin(i*0.17f);}
    const auto original=samples;
    APO_CONNECTION_DESCRIPTOR input{APO_CONNECTION_BUFFER_TYPE_EXTERNAL,reinterpret_cast<UINT_PTR>(samples.data()),512,media,APO_CONNECTION_DESCRIPTOR_SIGNATURE};
    auto output=input;APO_CONNECTION_DESCRIPTOR* in[]{&input};APO_CONNECTION_DESCRIPTOR* out[]{&output};
    expect(apo->LockForProcess(1,in,1,out)==S_OK,"lock stereo render format");
    APO_CONNECTION_PROPERTY p{reinterpret_cast<UINT_PTR>(samples.data()),512,BUFFER_VALID,APO_CONNECTION_PROPERTY_SIGNATURE};
    APO_CONNECTION_PROPERTY* connection[]{&p};
    cfg.eqEnabled=0;state.write(cfg);
    apo->APOProcess(1,connection,1,connection);
    expect(samples==original&&p.u32ValidFrameCount==512&&p.u32BufferFlags==BUFFER_VALID,"in-place EQ bypass preserves data and aliased connection metadata");
    cfg.eqEnabled=1;state.write(cfg);
    samples=original;
    StereoDSP expected;std::string error;expected.configure({{66,-12,7.4},{140,-12,3.21}},{{65,-8.4,8},{142,-12,3.36}},48000,error);
    std::array<double,1024> target{};std::copy(original.begin(),original.end(),target.begin());expected.processInterleaved(target.data(),512);
    apo->APOProcess(1,connection,1,connection);
    double difference=0;for(std::size_t i=0;i<samples.size();++i)difference=std::max(difference,std::abs(samples[i]-target[i]));
    expect(difference<1e-6,"APO EQ matches core L/R response");
    p.u32ValidFrameCount=513;apo->APOProcess(1,connection,1,connection);
    expect(p.u32BufferFlags==BUFFER_INVALID&&p.u32ValidFrameCount==0,"oversized buffer rejected without processing");
    expect(apo->Reset()==APOERR_APO_LOCKED,"cannot reset while callbacks may be active");
    apo->UnlockForProcess();
    auto* unsupported=makeMedia(2,32000);IAudioMediaType* suggested=nullptr;
    expect(apo->IsInputFormatSupported(nullptr,unsupported,&suggested)==S_FALSE&&suggested,"unsupported EQ rate gets a valid suggestion");
    if(suggested)suggested->Release();if(unsupported)unsupported->Release();
    media->Release();apo->Release();
}
void aecTest(SharedFile& state,Configuration cfg) {
    cfg.aecEnabled=0;state.write(cfg);
    auto* apo=initialize(true);auto* stereo=makeMedia(2);auto* mono=makeMedia(1);
    if(!stereo||!mono){expect(false,"media types available");apo->Release();return;}
    HNSTIME latency=0;apo->GetLatency(&latency);expect(latency==213334,"AEC reports rounded-up 1024-frame latency");
    constexpr std::size_t frames=480,iterations=10;
    std::array<float,frames*2> input{},reference{};std::array<float,frames> output{};
    APO_CONNECTION_DESCRIPTOR src{APO_CONNECTION_BUFFER_TYPE_EXTERNAL,reinterpret_cast<UINT_PTR>(input.data()),frames,stereo,APO_CONNECTION_DESCRIPTOR_SIGNATURE};
    APO_CONNECTION_DESCRIPTOR dst{APO_CONNECTION_BUFFER_TYPE_EXTERNAL,reinterpret_cast<UINT_PTR>(output.data()),frames,mono,APO_CONNECTION_DESCRIPTOR_SIGNATURE};
    APO_CONNECTION_DESCRIPTOR ref{APO_CONNECTION_BUFFER_TYPE_EXTERNAL,reinterpret_cast<UINT_PTR>(reference.data()),frames,stereo,APO_CONNECTION_DESCRIPTOR_SIGNATURE};
    expect(apo->AddAuxiliaryInput(0,0,nullptr,&ref)==S_OK,"auxiliary ID zero is valid");
    expect(apo->AddAuxiliaryInput(1,0,nullptr,&ref)==APOERR_NUM_CONNECTIONS_INVALID,"only one auxiliary reference allowed");
    APO_CONNECTION_DESCRIPTOR* ins[]{&src};APO_CONNECTION_DESCRIPTOR* outs[]{&dst};
    expect(apo->LockForProcess(1,ins,1,outs)==S_OK,"48kHz stereo input to mono AEC output");
    LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);
    bool preserved=true;
    for(std::size_t n=0;n<iterations;++n) {
        for(std::size_t i=0;i<frames;++i){input[i*2]=static_cast<float>((n*frames+i)%131)*0.001f;input[i*2+1]=0.8f;}
        const auto time=static_cast<UINT64>(frequency.QuadPart)+(n*frames*frequency.QuadPart)/48000;
        APO_CONNECTION_PROPERTY_V2 a{{src.pBuffer,frames,BUFFER_VALID,APO_CONNECTION_PROPERTY_V2_SIGNATURE},time};
        APO_CONNECTION_PROPERTY_V2 b{{dst.pBuffer,0,BUFFER_INVALID,APO_CONNECTION_PROPERTY_V2_SIGNATURE},0};
        APO_CONNECTION_PROPERTY_V2 r{{ref.pBuffer,frames,BUFFER_SILENT,APO_CONNECTION_PROPERTY_V2_SIGNATURE},time};
        apo->AcceptInput(0,&r.property);
        APO_CONNECTION_PROPERTY* ai[]{&a.property};APO_CONNECTION_PROPERTY* bo[]{&b.property};apo->APOProcess(1,ai,1,bo);
        for(std::size_t i=0;i<frames;++i){const auto index=n*frames+i;const float wanted=index<1024?0.f:static_cast<float>((index-1024)%131)*0.001f;preserved=preserved&&output[i]==wanted;}
        expect(b.property.u32ValidFrameCount==frames,"AEC frame count preserved");
    }
    expect(preserved,"AEC disabled preserves microphone input 1 with reported latency");
    apo->UnlockForProcess();expect(apo->RemoveAuxiliaryInput(0)==S_OK,"auxiliary removed after unlock");
    mono->Release();stereo->Release();apo->Release();
}
}
int main(){
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    const auto directory=dataDirectory();
    {
        SharedFile file;expect(file.open(true),"create isolated shared settings");
        Configuration cfg;std::string error;
        expect(prepareConfiguration(cfg,{{66,-12,7.4},{140,-12,3.21}},{{65,-8.4,8},{142,-12,3.36}},error),"prepare provided L/R EQ");
        expect(file.write(cfg),"write shared settings");
        dllTest();imports();eqTest(file,cfg);aecTest(file,cfg);
    }
    expect(DllCanUnloadNow()==S_OK,"all COM instances released");
    std::error_code ignored;std::filesystem::remove_all(directory,ignored);
    CoUninitialize();if(!failures)std::cout<<"Windows APO contract tests passed (no endpoint registration)\n";return failures?1:0;
}
