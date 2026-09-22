#pragma once
#include "ReferenceFeed.hpp"
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <thread>
#include <atomic>

namespace personaltools::win {
// Runs in the desktop app, never in audiodg. Windows performs the resampling
// from the selected speaker's native rate to the AEC's 48 kHz reference rate.
class LoopbackReference {
public:
    ~LoopbackReference(){stop();}
    void start(){if(!worker_.joinable()){quit_=false;worker_=std::thread([this]{run();});}}
    void stop(){quit_=true;if(worker_.joinable())worker_.join();}
private:
    void run() noexcept {
        const auto apartment=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        if(FAILED(apartment))return;
        try {while(!quit_){pump();for(int i=0;i<25&&!quit_;++i)Sleep(20);}}catch(...){}
        CoUninitialize();
    }
    void pump() {
        using Microsoft::WRL::ComPtr;
        SharedFile settings;Configuration config;LONG revision=0;
        if(!settings.open()||!settings.read(config,revision)||!config.aecEnabled)return;
        ReferenceFeed feed;if(!feed.open(config,false)||!feed.acquireWriter())return;
        ComPtr<IMMDeviceEnumerator> enumerator;ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> client;ComPtr<IAudioCaptureClient> capture;
        if(FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator)))||
           FAILED(enumerator->GetDevice(config.renderId,&device))||
           FAILED(device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(client.GetAddressOf()))))return;
        WAVEFORMATEX fmt{};fmt.wFormatTag=WAVE_FORMAT_IEEE_FLOAT;fmt.nChannels=2;fmt.nSamplesPerSec=48000;
        fmt.wBitsPerSample=32;fmt.nBlockAlign=8;fmt.nAvgBytesPerSec=384000;
        if(FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_LOOPBACK|AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM|
            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,1000000,0,&fmt,nullptr))||FAILED(client->GetService(IID_PPV_ARGS(&capture)))||FAILED(client->Start()))return;
        while(!quit_&&readWord(&settings.data()->sequence)==revision){
            UINT32 count=0;if(FAILED(capture->GetNextPacketSize(&count)))break;
            while(count&&!quit_){
                BYTE* bytes=nullptr;DWORD flags=0;UINT64 position=0,time=0;UINT32 frames=0;
                if(FAILED(capture->GetBuffer(&bytes,&frames,&flags,&position,&time))){client->Stop();return;}
                const bool silent=(flags&AUDCLNT_BUFFERFLAGS_SILENT)!=0;
                if(!(flags&AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR))for(unsigned offset=0;offset<frames;offset+=ReferenceFeed::framesPerPacket){
                    const auto length=std::min(ReferenceFeed::framesPerPacket,frames-offset);
                    feed.publish(silent?nullptr:reinterpret_cast<float*>(bytes)+offset*2,length,time+offset*10000000ull/48000,revision,silent);
                }
                capture->ReleaseBuffer(frames);
                if(FAILED(capture->GetNextPacketSize(&count))){client->Stop();return;}
            }
            Sleep(3);
        }
        client->Stop();
    }
    std::atomic<bool> quit_{false};std::thread worker_;
};
}
