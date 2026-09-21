#pragma once
#include "Shared.hpp"
#include <bit>
#include <aclapi.h>

namespace soundcontrol::win {
// Bounded, pagefile-backed IPC. No microphone samples enter this mapping.
// The audio service creates it; the desktop app only opens an existing mapping.
// Reuse the installed settings DACL rather than granting access to all users.
class ReferenceFeed {
public:
    static constexpr unsigned framesPerPacket=960, capacity=64;
    struct Packet { LONG revision=0; unsigned frames=0; std::uint64_t time=0; float samples[framesPerPacket*2]{}; };
    struct Slot { volatile LONG64 stamp=0,time=0; volatile LONG revision=0,frames=0; volatile LONG samples[framesPerPacket*2]{}; };
    struct Data {
        volatile LONG64 published=0; Slot slots[capacity];
        // Control diagnostics only: timestamps/counters, never microphone audio.
        volatile LONG64 micTime=0,rawTime=0,referenceTime=0,accepted=0,rejected=0;
    };
    ~ReferenceFeed(){close();}
    ReferenceFeed()=default;
    ReferenceFeed(const ReferenceFeed&)=delete;
    void close(){if(writer_){if(ownsWriter_)ReleaseSemaphore(writer_,1,nullptr);CloseHandle(writer_);}if(data_)UnmapViewOfFile(data_);if(mapping_)CloseHandle(mapping_);data_=nullptr;mapping_=writer_=nullptr;ownsWriter_=false;cursor_=0;}
    static std::wstring name(const Configuration& c) {
        std::uint64_t hash=14695981039346656037ull;
        for(const auto* id:{c.renderId,c.captureId})for(const auto* p=id;*p;++p){hash^=static_cast<unsigned>(*p);hash*=1099511628211ull;}
#ifdef SOUNDCONTROL_TESTING
        return L"Local\\SoundControlReference-test-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(hash);
#else
        return L"Global\\SoundControlReference-v1-"+std::to_wstring(hash);
#endif
    }
    bool open(const Configuration& c,bool create) {
        if(data_)return true;
        name_=name(c);
        // CreateFileMapping on an existing section asks for broader access than
        // the settings file's Modify ACL grants. Subsequent APO instances need
        // only read/write access, just like the desktop producer.
        mapping_=OpenFileMappingW(FILE_MAP_READ|FILE_MAP_WRITE,FALSE,name_.c_str());
        if(!mapping_&&create){
            PSECURITY_DESCRIPTOR descriptor=nullptr;PACL acl=nullptr;
            const auto settings=(dataDirectory()/L"state-v1.bin").wstring();
            if(GetNamedSecurityInfoW(settings.c_str(),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,&acl,nullptr,&descriptor)!=ERROR_SUCCESS)return false;
            SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),descriptor,FALSE};
            mapping_=CreateFileMappingW(INVALID_HANDLE_VALUE,&security,PAGE_READWRITE,0,sizeof(Data),name_.c_str());
            LocalFree(descriptor);
        }
        if(mapping_)data_=static_cast<Data*>(MapViewOfFile(mapping_,FILE_MAP_READ|FILE_MAP_WRITE,0,0,sizeof(Data)));
        if(!data_){close();return false;}
        cursor_=readWide(&data_->published);return true;
    }
    // Called only on the loopback worker, never on the audio callback.
    bool acquireWriter(){
        if(ownsWriter_)return true;if(!data_)return false;
        if(!writer_)writer_=CreateSemaphoreW(nullptr,1,1,(name_+L"-writer").c_str());
        ownsWriter_=writer_&&WaitForSingleObject(writer_,0)==WAIT_OBJECT_0;return ownsWriter_;
    }
    void publish(const float* samples,unsigned frames,std::uint64_t time,LONG revision,bool silent) {
        if(!data_||frames>framesPerPacket||(!silent&&!samples))return;
        const auto number=readWide(&data_->published)+1;auto& s=data_->slots[number%capacity];
        InterlockedExchange64(&s.stamp,-number);
        InterlockedExchange64(&s.time,time);InterlockedExchange(&s.revision,revision);InterlockedExchange(&s.frames,frames);
        for(unsigned i=0;i<frames*2;++i)InterlockedExchange(&s.samples[i],silent?0:std::bit_cast<LONG>(samples[i]));
        InterlockedExchange64(&s.stamp,number);InterlockedExchange64(&data_->published,number);
    }
    // Independent cursor per APO, atomic slot words, bounded copy and retry.
    // Old packets are rejected using their QPC timestamp by the caller.
    bool next(Packet& out) {
        if(!data_)return false;
        const auto latest=readWide(&data_->published);if(cursor_>=latest)return false;
        if(latest-cursor_>capacity)cursor_=latest-capacity;
        const auto wanted=cursor_+1;auto& s=data_->slots[wanted%capacity];
        if(readWide(&s.stamp)!=wanted){cursor_=wanted;return false;}
        out.frames=readWord(&s.frames);out.time=readWide(&s.time);out.revision=readWord(&s.revision);
        if(out.frames>framesPerPacket){cursor_=wanted;return false;}
        for(unsigned i=0;i<out.frames*2;++i)out.samples[i]=std::bit_cast<float>(readWord(&s.samples[i]));
        const bool valid=readWide(&s.stamp)==wanted;cursor_=wanted;return valid;
    }
    void observe(std::uint64_t mic,std::uint64_t raw){if(data_){InterlockedExchange64(&data_->micTime,mic);InterlockedExchange64(&data_->rawTime,raw);}}
    void observedReference(std::uint64_t time,bool accepted){if(data_){InterlockedExchange64(&data_->referenceTime,time);InterlockedIncrement64(accepted?&data_->accepted:&data_->rejected);}}
private:
    HANDLE mapping_=nullptr,writer_=nullptr;bool ownsWriter_=false;std::wstring name_;Data* data_=nullptr;LONG64 cursor_=0;
};
}
