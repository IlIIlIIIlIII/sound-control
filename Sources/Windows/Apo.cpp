#include "Shared.hpp"
#include "AudioStream.hpp"
#include <audioenginebaseapo.h>
#include <audioengineextensionapo.h>
#include <memory>
#include <new>
#include <atomic>

namespace soundcontrol::win {
namespace {
std::atomic<long> objects{0}, serverLocks{0};
constexpr GUID floatFormat{3,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
// KSPROPSETID_AudioEffectsDiscovery standard effect GUIDs.
constexpr GUID echoEffect{0x6f64adbe,0x8211,0x11e2,{0x8c,0x70,0x2c,0x27,0xd7,0xf0,0x01,0xfa}};
// A plain float media type avoids linking the WDK's static base-APO library.
// Format negotiation is on the non-realtime thread.
class FloatMediaType final : public IAudioMediaType {
public:
    explicit FloatMediaType(const UNCOMPRESSEDAUDIOFORMAT& format) : format_(format) {
        ++objects;
        wave_.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wave_.Format.nChannels = static_cast<WORD>(format.dwSamplesPerFrame);
        wave_.Format.nSamplesPerSec = static_cast<DWORD>(format.fFramesPerSecond);
        wave_.Format.wBitsPerSample = 32;
        wave_.Format.nBlockAlign = wave_.Format.nChannels * 4;
        wave_.Format.nAvgBytesPerSec = wave_.Format.nSamplesPerSec * wave_.Format.nBlockAlign;
        wave_.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        wave_.Samples.wValidBitsPerSample = 32;
        wave_.dwChannelMask = format.dwChannelMask;
        wave_.SubFormat = floatFormat;
    }
    ~FloatMediaType() { --objects; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id != __uuidof(IUnknown) && id != __uuidof(IAudioMediaType)) return E_NOINTERFACE;
        *out = static_cast<IAudioMediaType*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { auto n = --refs_; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE IsCompressedFormat(BOOL* compressed) override {
        if (!compressed) return E_POINTER; *compressed = FALSE; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE IsEqual(IAudioMediaType* other, DWORD* flags) override {
        if (!other || !flags) return E_POINTER;
        *flags = 0; UNCOMPRESSEDAUDIOFORMAT f{};
        if (FAILED(other->GetUncompressedAudioFormat(&f))) return S_FALSE;
        if (f.guidFormatType != format_.guidFormatType) return S_FALSE;
        *flags = AUDIOMEDIATYPE_EQUAL_FORMAT_TYPES;
        if (f.dwSamplesPerFrame != format_.dwSamplesPerFrame ||
            f.dwBytesPerSampleContainer != 4 || f.dwValidBitsPerSample != 32 ||
            f.fFramesPerSecond != format_.fFramesPerSecond || f.dwChannelMask != format_.dwChannelMask) return S_FALSE;
        *flags |= AUDIOMEDIATYPE_EQUAL_FORMAT_DATA | AUDIOMEDIATYPE_EQUAL_FORMAT_USER_DATA;
        return S_OK;
    }
    const WAVEFORMATEX* STDMETHODCALLTYPE GetAudioFormat() override { return &wave_.Format; }
    HRESULT STDMETHODCALLTYPE GetUncompressedAudioFormat(UNCOMPRESSEDAUDIOFORMAT* out) override {
        if (!out) return E_POINTER; *out = format_; return S_OK;
    }
private:
    std::atomic<ULONG> refs_{1};
    UNCOMPRESSEDAUDIOFORMAT format_{};
    WAVEFORMATEXTENSIBLE wave_{};
};
using CreateMedia = HRESULT (WINAPI*)(const UNCOMPRESSEDAUDIOFORMAT*, IAudioMediaType**);
HRESULT WINAPI createFloatMedia(const UNCOMPRESSEDAUDIOFORMAT* format, IAudioMediaType** out) {
    if (!format || !out) return E_POINTER;
    *out = nullptr;
    if (format->guidFormatType != floatFormat || format->dwSamplesPerFrame < 1 ||
        format->dwSamplesPerFrame > 2 || format->dwBytesPerSampleContainer != 4 ||
        format->dwValidBitsPerSample != 32 || !std::isfinite(format->fFramesPerSecond) ||
        format->fFramesPerSecond < 8000 || format->fFramesPerSecond > 192000) return E_INVALIDARG;
    *out = new(std::nothrow) FloatMediaType(*format);
    return *out ? S_OK : E_OUTOFMEMORY;
}
CreateMedia mediaFactory() { return createFloatMedia; }
bool format(IAudioMediaType* type, UNCOMPRESSEDAUDIOFORMAT& value) {
    return type && SUCCEEDED(type->GetUncompressedAudioFormat(&value)) &&
        std::isfinite(value.fFramesPerSecond) && value.fFramesPerSecond > 0 && value.fFramesPerSecond <= 384000;
}
bool floating(const UNCOMPRESSEDAUDIOFORMAT& f) {
    return f.guidFormatType==floatFormat && f.dwBytesPerSampleContainer==4 && f.dwValidBitsPerSample==32;
}
std::uint64_t timestamp(const APO_CONNECTION_PROPERTY* p) {
    return p->u32Signature==APO_CONNECTION_PROPERTY_V2_SIGNATURE
        ? reinterpret_cast<const APO_CONNECTION_PROPERTY_V2*>(p)->u64QPCTime : 0;
}
bool containsEndpoint(IMMDeviceCollection* collection, const wchar_t* wanted) {
    UINT count = 0;
    if (!collection || FAILED(collection->GetCount(&count))) return false;
    for (UINT i = 0; i < count; ++i) {
        IMMDevice* device = nullptr; LPWSTR id = nullptr; bool matches = false;
        if (SUCCEEDED(collection->Item(i, &device))) {
            if (SUCCEEDED(device->GetId(&id))) {
                matches = wcsncmp(id, wanted, 512) == 0; CoTaskMemFree(id);
            }
            device->Release();
        }
        if (matches) return true;
    }
    return false;
}
APO_REG_PROPERTIES properties(bool capture) {
    APO_REG_PROPERTIES p{};
    p.clsid=capture?aecClass:eqClass;
    p.Flags=static_cast<APO_FLAG>((capture?0:APO_FLAG_INPLACE)|APO_FLAG_FRAMESPERSECOND_MUST_MATCH|
        APO_FLAG_BITSPERSAMPLE_MUST_MATCH|(capture?0:APO_FLAG_SAMPLESPERFRAME_MUST_MATCH));
    wcscpy_s(p.szFriendlyName,capture?L"SoundControl microphone echo cancellation":L"SoundControl stereo EQ");
    wcscpy_s(p.szCopyrightInfo,L"SoundControl");
    p.u32MajorVersion=1;
    p.u32MinInputConnections=p.u32MaxInputConnections=p.u32MinOutputConnections=p.u32MaxOutputConnections=1;
    p.u32MaxInstances=UINT_MAX;
    p.u32NumAPOInterfaces=1;
    p.iidAPOInterfaceList[0]=__uuidof(IAudioProcessingObject);
    return p;
}
class Apo final : public IAudioProcessingObject, public IAudioProcessingObjectRT,
                  public IAudioProcessingObjectConfiguration, public IAudioSystemEffects3,
                  public IApoAcousticEchoCancellation, public IApoAuxiliaryInputConfiguration,
                  public IApoAuxiliaryInputRT {
public:
    explicit Apo(bool capture): capture_(capture) {
        ++objects;
    }
    ~Apo() { if (effectsEvent_) CloseHandle(effectsEvent_); --objects; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
        if(!out) return E_POINTER; *out=nullptr;
        if(iid==__uuidof(IUnknown)||iid==__uuidof(IAudioProcessingObject)) *out=static_cast<IAudioProcessingObject*>(this);
        else if(iid==__uuidof(IAudioProcessingObjectRT)) *out=static_cast<IAudioProcessingObjectRT*>(this);
        else if(iid==__uuidof(IAudioProcessingObjectConfiguration)) *out=static_cast<IAudioProcessingObjectConfiguration*>(this);
        else if(iid==__uuidof(IAudioSystemEffects)||iid==__uuidof(IAudioSystemEffects2)||iid==__uuidof(IAudioSystemEffects3)) *out=static_cast<IAudioSystemEffects3*>(this);
        else if(capture_&&iid==__uuidof(IApoAcousticEchoCancellation)) *out=static_cast<IApoAcousticEchoCancellation*>(this);
        else if(capture_&&iid==__uuidof(IApoAuxiliaryInputConfiguration)) *out=static_cast<IApoAuxiliaryInputConfiguration*>(this);
        else if(capture_&&iid==__uuidof(IApoAuxiliaryInputRT)) *out=static_cast<IApoAuxiliaryInputRT*>(this);
        else return E_NOINTERFACE;
        AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { const auto n=--refs_;if(!n)delete this;return n; }
    HRESULT STDMETHODCALLTYPE Initialize(UINT32 bytes,BYTE* data) override {
        if(initialized_)return APOERR_ALREADY_INITIALIZED;
        if(!data)return E_POINTER;
        IMMDeviceCollection* collection=nullptr;
        bool discovery=false;
        if(bytes==sizeof(APOInitSystemEffects3)) {
            const auto* init=reinterpret_cast<const APOInitSystemEffects3*>(data);
            if(init->APOInit.clsid!=(capture_?aecClass:eqClass))return APOERR_INVALID_APO_CLSID;
            collection=init->pDeviceCollection;discovery=init->InitializeForDiscoveryOnly!=FALSE;
        } else if(bytes==sizeof(APOInitSystemEffects2)) {
            if(capture_)return E_NOTIMPL; // CAPX AEC requires Windows 11 initialization.
            const auto* init=reinterpret_cast<const APOInitSystemEffects2*>(data);
            if(init->APOInit.clsid!=eqClass)return APOERR_INVALID_APO_CLSID;
            collection=init->pDeviceCollection;discovery=init->InitializeForDiscoveryOnly!=FALSE;
        } else return E_INVALIDARG;
        try {
            if(!shared_.open())return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
            if(!shared_.read(config_,revision_))return E_FAIL;
            if(!discovery) {
                // Only the explicitly selected endpoint may use these settings.
                if (!containsEndpoint(collection, capture_ ? config_.captureId : config_.renderId))
                    return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
            }
            if(capture_) {
                stream_=std::make_unique<EchoStream>();
                if (!stream_->valid()) return E_OUTOFMEMORY;
            }
            LARGE_INTEGER frequency{};
            if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) return E_FAIL;
            qpcFrequency_ = static_cast<std::uint64_t>(frequency.QuadPart);
            createMedia_=mediaFactory();
            if(!createMedia_)return E_NOTIMPL;
            discovery_=discovery;initialized_=true;return S_OK;
        } catch(...) { return E_OUTOFMEMORY; }
    }
    HRESULT STDMETHODCALLTYPE Reset() override {
        if(locked_)return APOERR_APO_LOCKED;
        eq_.reset();if(stream_)stream_->reset();return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetLatency(HNSTIME* value) override {
        if(!value)return E_POINTER;
        *value=capture_?static_cast<HNSTIME>((EchoStream::latencyFrames*10000000ull+47999)/48000):0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRegistrationProperties(APO_REG_PROPERTIES** out) override {
        if(!out)return E_POINTER;
        *out=static_cast<APO_REG_PROPERTIES*>(CoTaskMemAlloc(sizeof(APO_REG_PROPERTIES)));
        if(!*out)return E_OUTOFMEMORY;**out=properties(capture_);return S_OK;
    }
    HRESULT negotiate(IAudioMediaType* opposite,IAudioMediaType* requested,IAudioMediaType** out,bool auxiliary,bool output) {
        if(!out||!requested)return E_POINTER;*out=nullptr;
        UNCOMPRESSEDAUDIOFORMAT f{},other{};
        if(!format(requested,f))return E_INVALIDARG;
        bool ok=floating(f);
        if(capture_)ok=ok&&f.fFramesPerSecond==48000&&(f.dwSamplesPerFrame==1||f.dwSamplesPerFrame==2);
        else ok=ok&&f.dwSamplesPerFrame==2&&std::find(rates.begin(),rates.end(),static_cast<unsigned>(f.fFramesPerSecond))!=rates.end()&&f.fFramesPerSecond==static_cast<unsigned>(f.fFramesPerSecond);
        if(opposite) {
            if(!format(opposite,other))return E_INVALIDARG;
            ok=ok&&floating(other)&&other.fFramesPerSecond==f.fFramesPerSecond;
            if(!capture_)ok=ok&&other.dwSamplesPerFrame==f.dwSamplesPerFrame;
        }
        if(ok) { requested->AddRef();*out=requested;return S_OK; }
        UNCOMPRESSEDAUDIOFORMAT preferred{};
        preferred.guidFormatType=floatFormat;preferred.dwSamplesPerFrame=(capture_&&output&&!auxiliary)?1:2;
        preferred.dwBytesPerSampleContainer=4;preferred.dwValidBitsPerSample=32;
        preferred.fFramesPerSecond=48000;preferred.dwChannelMask=preferred.dwSamplesPerFrame==1?4:3;
        // Do not suggest a format incompatible with an already fixed opposite side.
        if(opposite&&other.fFramesPerSecond!=48000)return APOERR_FORMAT_NOT_SUPPORTED;
        const auto factory=createMedia_?createMedia_:mediaFactory();
        if(!factory)return E_NOTIMPL;
        const auto hr=factory(&preferred,out);return FAILED(hr)?hr:S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE IsInputFormatSupported(IAudioMediaType* o,IAudioMediaType* r,IAudioMediaType** s) override {return negotiate(o,r,s,false,false);}
    HRESULT STDMETHODCALLTYPE IsOutputFormatSupported(IAudioMediaType* o,IAudioMediaType* r,IAudioMediaType** s) override {return negotiate(o,r,s,false,true);}
    HRESULT STDMETHODCALLTYPE IsInputFormatSupported(IAudioMediaType* r,IAudioMediaType** s) override {return negotiate(nullptr,r,s,true,false);}
    HRESULT STDMETHODCALLTYPE GetInputChannelCount(UINT32* n) override {if(!n)return E_POINTER;*n=inputChannels_;return S_OK;}
    HRESULT STDMETHODCALLTYPE LockForProcess(UINT32 ni,APO_CONNECTION_DESCRIPTOR** in,UINT32 no,APO_CONNECTION_DESCRIPTOR** out) override {
        if(!initialized_)return APOERR_NOT_INITIALIZED;
        if(locked_)return APOERR_APO_LOCKED;
        if(ni!=1||no!=1)return APOERR_NUM_CONNECTIONS_INVALID;
        if(!in||!out||!in[0]||!out[0])return E_POINTER;
        UNCOMPRESSEDAUDIOFORMAT a{},b{};
        if(!format(in[0]->pFormat,a)||!format(out[0]->pFormat,b)||!floating(a)||!floating(b)||a.fFramesPerSecond!=b.fFramesPerSecond)
            return APOERR_INVALID_CONNECTION_FORMAT;
        if(capture_) {
            if(a.fFramesPerSecond!=48000||a.dwSamplesPerFrame<1||a.dwSamplesPerFrame>2||b.dwSamplesPerFrame<1||b.dwSamplesPerFrame>2)
                return APOERR_INVALID_CONNECTION_FORMAT;
        } else if(a.dwSamplesPerFrame!=2||b.dwSamplesPerFrame!=2||
            a.fFramesPerSecond!=static_cast<unsigned>(a.fFramesPerSecond)||!eq_.configure(config_,static_cast<unsigned>(a.fFramesPerSecond)))return APOERR_INVALID_CONNECTION_FORMAT;
        if (capture_ && in[0]->pBuffer && in[0]->pBuffer==out[0]->pBuffer) return APOERR_BUFFERS_OVERLAP;
        if(in[0]->u32MaxFrameCount==0||out[0]->u32MaxFrameCount<in[0]->u32MaxFrameCount)return APOERR_INVALID_OUTPUT_MAXFRAMECOUNT;
        inputChannels_=a.dwSamplesPerFrame;outputChannels_=b.dwSamplesPerFrame;maxFrames_=in[0]->u32MaxFrameCount;rate_=static_cast<unsigned>(a.fFramesPerSecond);
        eq_.reset();if(stream_)stream_->reset();
        auto& m=meter();InterlockedExchange(&m.rate,rate_);InterlockedExchange(&m.error,capture_ && auxiliary_ && !referenceMatches_ ? ERROR_NOT_FOUND : 0);
        locked_=true;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE UnlockForProcess() override {if(!locked_)return APOERR_ALREADY_UNLOCKED;locked_=false;return S_OK;}
    UINT32 STDMETHODCALLTYPE CalcInputFrames(UINT32 n) override {return n;}
    UINT32 STDMETHODCALLTYPE CalcOutputFrames(UINT32 n) override {return n;}
    void STDMETHODCALLTYPE APOProcess(UINT32 ni,APO_CONNECTION_PROPERTY** in,UINT32 no,APO_CONNECTION_PROPERTY** out) override {
        if(no!=1||!out||!out[0])return;
        auto* dest=out[0];
        if(!locked_||ni!=1||!in||!in[0]) {dest->u32ValidFrameCount=0;dest->u32BufferFlags=BUFFER_INVALID;return;}
        const auto sourceCopy=*in[0];
        const auto rawTime=timestamp(in[0]);
        const auto* source=&sourceCopy;const auto frames=source->u32ValidFrameCount;
        dest->u32ValidFrameCount=0;dest->u32BufferFlags=BUFFER_INVALID;
        if(frames>maxFrames_||!dest->pBuffer||source->u32BufferFlags==BUFFER_INVALID)return;
        const bool silent=source->u32BufferFlags==BUFFER_SILENT;
        if(!silent&&!source->pBuffer)return;
        LARGE_INTEGER start{},end{};QueryPerformanceCounter(&start);
        if(readWord(&shared_.data()->sequence)!=revision_) {
            Configuration updated;LONG revision=0;
            if(shared_.read(updated,revision)) {
                if(capture_||eq_.configure(updated,rate_)) {config_=updated;revision_=revision;}
                else InterlockedExchange(&meter().error,ERROR_INVALID_DATA);
            }
        }
        const bool enabled=osEnabled_.load(std::memory_order_relaxed)&&(capture_?config_.aecEnabled:config_.eqEnabled);
        const auto* src=reinterpret_cast<const float*>(source->pBuffer);
        auto* dst=reinterpret_cast<float*>(dest->pBuffer);
        if(capture_)stream_->microphone(src,dst,frames,inputChannels_,outputChannels_,normalizedTime(rawTime),silent,enabled);
        else eq_.process(src,dst,frames,silent,enabled);
        dest->u32ValidFrameCount=frames;dest->u32BufferFlags=BUFFER_VALID;
        if(dest->u32Signature==APO_CONNECTION_PROPERTY_V2_SIGNATURE) {
            auto* v2=reinterpret_cast<APO_CONNECTION_PROPERTY_V2*>(dest);
            const auto t=rawTime;
            const auto latency=capture_?(EchoStream::latencyFrames*qpcFrequency_+24000)/48000:0;
            v2->u64QPCTime=t>latency?t-latency:0;
        }
        auto& m=meter();InterlockedIncrement64(&m.callbacks);InterlockedAdd64(&m.frames,frames);
        InterlockedExchange(&m.enabled,enabled?1:0);
        if(capture_) {
            const auto metric=stream_->metrics();
            InterlockedExchange(&m.reference,metric.active?1:0);
            InterlockedExchange(&m.clipping,metric.inputClipping?1:0);
            InterlockedExchange(&m.reductionDB100,static_cast<LONG>(metric.reductionDB*100));
            InterlockedExchange64(&m.missingFrames,metric.referenceUnderruns);
            InterlockedExchange64(&m.dropped,stream_->dropped());
        }
        QueryPerformanceCounter(&end);InterlockedExchange64(&m.lastQpc,end.QuadPart);
        const auto elapsed=end.QuadPart-start.QuadPart;
        auto previous=readWide(&m.maxCallbackTicks);
        for (unsigned attempt=0; attempt<3 && elapsed>previous; ++attempt) {
            const auto observed=InterlockedCompareExchange64(&m.maxCallbackTicks,elapsed,previous);
            if(observed==previous)break;previous=observed;
        }
    }
    HRESULT STDMETHODCALLTYPE AddAuxiliaryInput(DWORD id,UINT32 bytes,BYTE* data,APO_CONNECTION_DESCRIPTOR* d) override {
        if(!initialized_)return APOERR_NOT_INITIALIZED;
        if(locked_)return APOERR_APO_LOCKED;
        if(auxiliary_)return APOERR_NUM_CONNECTIONS_INVALID;
        UNCOMPRESSEDAUDIOFORMAT f{};
        if(!d||!format(d->pFormat,f)||!floating(f)||f.fFramesPerSecond!=48000||(f.dwSamplesPerFrame!=1&&f.dwSamplesPerFrame!=2))return APOERR_FORMAT_NOT_SUPPORTED;
        referenceMatches_ = discovery_;
        if (data && bytes >= sizeof(APOInitSystemEffects3)) {
            const auto* init = reinterpret_cast<const APOInitSystemEffects3*>(data);
            referenceMatches_ = containsEndpoint(init->pDeviceCollection, config_.renderId);
        }
        InterlockedExchange(&meter().error, referenceMatches_ ? 0 : ERROR_NOT_FOUND);
        auxiliary_=true;auxId_=id;auxChannels_=f.dwSamplesPerFrame;auxMaxFrames_=d->u32MaxFrameCount;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE RemoveAuxiliaryInput(DWORD id) override {
        if(locked_)return APOERR_APO_LOCKED;
        if(!auxiliary_||id!=auxId_)return APOERR_INVALID_INPUTID;
        auxiliary_=false;if(stream_)stream_->reset();return S_OK;
    }
    void STDMETHODCALLTYPE AcceptInput(DWORD id,const APO_CONNECTION_PROPERTY* p) override {
        if(!locked_||!auxiliary_||!referenceMatches_||id!=auxId_||!p||p->u32ValidFrameCount>auxMaxFrames_||p->u32BufferFlags==BUFFER_INVALID)return;
        stream_->reference(reinterpret_cast<const float*>(p->pBuffer),p->u32ValidFrameCount,auxChannels_,normalizedTime(timestamp(p)),p->u32BufferFlags==BUFFER_SILENT);
    }
    HRESULT STDMETHODCALLTYPE GetEffectsList(GUID** effects,UINT* count,HANDLE event) override {
        const auto hr = rememberEvent(event); if (FAILED(hr)) return hr;
        if(!effects||!count)return E_POINTER;*count=0;*effects=static_cast<GUID*>(CoTaskMemAlloc(sizeof(GUID)));
        if(!*effects)return E_OUTOFMEMORY;**effects=capture_?echoEffect:eqClass;*count=1;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetControllableSystemEffectsList(AUDIO_SYSTEMEFFECT** effects,UINT* count,HANDLE event) override {
        const auto hr = rememberEvent(event); if (FAILED(hr)) return hr;
        if(!effects||!count)return E_POINTER;*count=0;
        *effects=static_cast<AUDIO_SYSTEMEFFECT*>(CoTaskMemAlloc(sizeof(AUDIO_SYSTEMEFFECT)));
        if(!*effects)return E_OUTOFMEMORY;
        **effects={capture_?echoEffect:eqClass,TRUE,osEnabled_.load()?AUDIO_SYSTEMEFFECT_STATE_ON:AUDIO_SYSTEMEFFECT_STATE_OFF};*count=1;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetAudioSystemEffectState(GUID id,AUDIO_SYSTEMEFFECT_STATE state) override {
        if(id!=(capture_?echoEffect:eqClass)||(state!=AUDIO_SYSTEMEFFECT_STATE_ON&&state!=AUDIO_SYSTEMEFFECT_STATE_OFF))return E_INVALIDARG;
        AcquireSRWLockExclusive(&eventLock_);
        const bool on = state==AUDIO_SYSTEMEFFECT_STATE_ON;
        const bool changed = osEnabled_.exchange(on) != on;
        if (changed && effectsEvent_) SetEvent(effectsEvent_);
        ReleaseSRWLockExclusive(&eventLock_);
        return S_OK;
    }
private:
    // Effects discovery/control runs outside the realtime callbacks.
    HRESULT rememberEvent(HANDLE event) {
        HANDLE copy = nullptr;
        if (event && !DuplicateHandle(GetCurrentProcess(), event, GetCurrentProcess(),
                                      &copy, EVENT_MODIFY_STATE, FALSE, 0))
            return HRESULT_FROM_WIN32(GetLastError());
        AcquireSRWLockExclusive(&eventLock_);
        if (effectsEvent_) CloseHandle(effectsEvent_);
        effectsEvent_ = copy;
        ReleaseSRWLockExclusive(&eventLock_);
        return S_OK;
    }
    SRWLOCK eventLock_ = SRWLOCK_INIT;
    HANDLE effectsEvent_ = nullptr;
    std::uint64_t normalizedTime(std::uint64_t raw) const {
        return (raw/qpcFrequency_)*10000000ull + (raw%qpcFrequency_)*10000000ull/qpcFrequency_;
    }
    std::uint64_t qpcFrequency_=10000000;
    Meter& meter() {return capture_?shared_.data()->capture:shared_.data()->render;}
    std::atomic<ULONG> refs_{1};
    const bool capture_;
    bool initialized_=false,locked_=false,auxiliary_=false,discovery_=false,referenceMatches_=false;
    std::atomic<bool> osEnabled_{true};
    unsigned inputChannels_=2,outputChannels_=2,maxFrames_=0,rate_=48000,auxChannels_=2,auxMaxFrames_=0;
    DWORD auxId_=0;
    SharedFile shared_;Configuration config_{};LONG revision_=0;
    RealtimeEQ eq_;std::unique_ptr<EchoStream> stream_;
    CreateMedia createMedia_=nullptr;
};
class Factory final:public IClassFactory {
public:
    explicit Factory(bool capture):capture_(capture){++objects;}
    ~Factory(){--objects;}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {
        if(!out)return E_POINTER;*out=nullptr;
        if(id!=__uuidof(IUnknown)&&id!=__uuidof(IClassFactory))return E_NOINTERFACE;
        *out=static_cast<IClassFactory*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return ++refs_;}
    ULONG STDMETHODCALLTYPE Release() override{const auto n=--refs_;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer,REFIID id,void** out) override {
        if(!out)return E_POINTER;*out=nullptr;if(outer)return CLASS_E_NOAGGREGATION;
        try {auto* apo=new Apo(capture_);const auto hr=apo->QueryInterface(id,out);apo->Release();return hr;}
        catch(...){return E_OUTOFMEMORY;}
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override{if(lock)++serverLocks;else --serverLocks;return S_OK;}
private:
    std::atomic<ULONG> refs_{1};bool capture_;
};
}
}
extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID clsid,REFIID iid,void** out) {
    using namespace soundcontrol::win;
    if(!out)return E_POINTER;*out=nullptr;
    if(clsid!=eqClass&&clsid!=aecClass)return CLASS_E_CLASSNOTAVAILABLE;
    auto* factory=new(std::nothrow) Factory(clsid==aecClass);
    if(!factory)return E_OUTOFMEMORY;
    const auto hr=factory->QueryInterface(iid,out);factory->Release();return hr;
}
extern "C" HRESULT __stdcall DllCanUnloadNow() {
    return soundcontrol::win::objects.load()==0&&soundcontrol::win::serverLocks.load()==0?S_OK:S_FALSE;
}
