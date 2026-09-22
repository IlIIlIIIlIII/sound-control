#pragma once
#include "DtlnNpu.hpp"
#include "AEC.hpp"
#include <atomic>
#include <thread>
#include <bit>

namespace personaltools::win {
// A missing result has no wet sample to crossfade from. Retain the last valid
// value during the short transition; substituting dry here bypasses the ramp.
class NeuralOutputMixer {
public:
    float process(float dry,float wet,bool available,bool enabled) {
        if(available)lastWet_=wet;
        const float aim=enabled&&available?1.f:0.f;
        blend_+=std::clamp(aim-blend_,-1.f/240.f,1.f/240.f);
        return dry+(lastWet_-dry)*blend_;
    }
private:
    float lastWet_=0.f,blend_=0.f;
};
// Per-stream SPSC queues isolate NPU scheduling/compilation from the audio thread.
// Missing/late model output fades to latency-matched original microphone audio.
class NeuralStream {
public:
    static constexpr unsigned latencyFrames=4096; // 85.3 ms, including NPU scheduling jitter.
    static constexpr unsigned hop=384, packetFrames=256, queueSize=128, ringSize=32768;
    static constexpr unsigned modelDelay=1152+62; // 24 ms model + two 63-tap FIRs.
    explicit NeuralStream(const std::filesystem::path& folder):worker_([this,folder]{run(folder);}){}
    ~NeuralStream(){quit_.store(true);if(worker_.joinable())worker_.join();}
    int state()const{return state_.load();}
    std::uint64_t misses()const{return misses_.load();}
    std::uint64_t completed()const{return completed_.load();}
    void reference(const float* input,unsigned frames,unsigned channels,std::uint64_t time,bool silent){
        for(unsigned off=0;off<frames;off+=packetFrames){
            Packet p;p.frames=std::min(packetFrames,frames-off);p.time=time+(off*10000000ull+24000)/48000;
            for(unsigned i=0;i<p.frames;++i)p.samples[i]=silent?0.f:0.5f*(input[(off+i)*channels]+input[(off+i)*channels+channels-1]);
            if(!reference_.push(p)){lost_.fetch_add(1);break;}
        }
    }
    void microphone(const float* input,float* output,unsigned frames,unsigned channels,unsigned outChannels,std::uint64_t time,bool silent,bool enabled){
        for(unsigned off=0;off<frames;off+=packetFrames){
            Packet p;p.frames=std::min(packetFrames,frames-off);p.time=time+(off*10000000ull+24000)/48000;p.index=position_+off;
            for(unsigned i=0;i<p.frames;++i){const auto v=silent?0.f:input[(off+i)*channels];p.samples[i]=std::isfinite(v)?v:0.f;}
            if(!microphone_.push(p))lost_.fetch_add(1);
        }
        for(unsigned i=0;i<frames;++i){
            const auto index=position_+i;
            const float dry=dry_[index%latencyFrames];
            const float sample=silent?0.f:input[i*channels];dry_[index%latencyFrames]=std::isfinite(sample)?sample:0.f;
            float wet=dry;bool found=false;
            if(index>=latencyFrames){
                const auto target=index-latencyFrames+modelDelay;
                const auto& s=results_[target%ringSize];
                const auto stamp=s.stamp.load(std::memory_order_acquire);
                if(stamp==target+1){
                    const float value=std::bit_cast<float>(s.sample.load(std::memory_order_relaxed));
                    found=s.stamp.load(std::memory_order_acquire)==stamp;
                    if(found)wet=value;
                }
            }
            const float value=mixer_.process(dry,wet,found,
                enabled&&state_.load(std::memory_order_relaxed)>=2);
            for(unsigned c=0;c<outChannels;++c)output[i*outChannels+c]=value;
            if(enabled&&!found&&index>=latencyFrames)misses_.fetch_add(1,std::memory_order_relaxed);
        }
        position_+=frames;
    }
private:
    struct Packet{std::uint64_t time=0,index=0;unsigned frames=0;std::array<float,packetFrames>samples{};};
    struct Queue{
        std::array<Packet,queueSize>p{};std::atomic<std::uint64_t>w{0},r{0};
        bool push(const Packet& item){auto x=w.load(std::memory_order_relaxed);if(x-r.load(std::memory_order_acquire)>=queueSize)return false;p[x%queueSize]=item;w.store(x+1,std::memory_order_release);return true;}
        bool peek(Packet& item){auto x=r.load(std::memory_order_relaxed);if(x==w.load(std::memory_order_acquire))return false;item=p[x%queueSize];return true;}
        void pop(){r.fetch_add(1,std::memory_order_release);}
    } reference_,microphone_;
    struct Result{std::atomic<std::uint64_t>stamp{0};std::atomic<std::uint32_t>sample{0};};
    std::array<Result,ringSize>results_{};
    std::array<float,latencyFrames>dry_{};std::uint64_t position_=0;NeuralOutputMixer mixer_;
    std::atomic<bool>quit_{false};std::atomic<int>state_{0};
    std::atomic<std::uint64_t>lost_{0},misses_{0},completed_{0};
    struct Fir{
        std::array<float,63>h{},history{};unsigned pos=0;
        Fir(){double sum=0;for(int k=0;k<63;++k){double x=k-31.;double s=x==0?0.30:std::sin(std::numbers::pi*0.30*x)/(std::numbers::pi*x);h[k]=static_cast<float>(s*(0.54-0.46*std::cos(2*std::numbers::pi*k/62)));sum+=h[k];}for(auto& v:h)v/=static_cast<float>(sum);}
        float tick(float x){history[pos]=x;float y=0;for(unsigned k=0;k<63;++k)y+=h[k]*history[(pos+63-k)%63];pos=(pos+1)%63;return y;}
        void reset(){history.fill(0);pos=0;}
    };
    void run(const std::filesystem::path& folder)noexcept{
        struct Timer{
            HANDLE handle=CreateWaitableTimerExW(nullptr,nullptr,0x00000002,TIMER_ALL_ACCESS);
            ~Timer(){if(handle)CloseHandle(handle);}
            void wait(){if(!handle){Sleep(1);return;}LARGE_INTEGER due{};due.QuadPart=-10000;
                if(SetWaitableTimer(handle,&due,0,nullptr,nullptr,FALSE))WaitForSingleObject(handle,20);else Sleep(1);}
        } timer;
        try{
            state_.store(1);DtlnNpu model;model.open(folder);state_.store(2);
            StereoReferenceTimeline timeline;Packet p,r;Fir micFir,refFir,outFir;
            std::array<float,packetFrames>left{},right{};
            std::array<float,128>mic{},ref{},out{};
            std::uint64_t expected=0,expectedTime=0,base=0,processed=0,seenLost=0;
            unsigned count=0;bool good=true;
            LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);
            while(!quit_.load()){
                while(reference_.peek(r)){timeline.push(r.samples.data(),r.samples.data(),r.frames,48000,r.time,10000000);reference_.pop();}
                if(!microphone_.peek(p)){timer.wait();continue;}
                LARGE_INTEGER qpc{};QueryPerformanceCounter(&qpc);
                const auto now=static_cast<std::uint64_t>(static_cast<long double>(qpc.QuadPart)*10000000/frequency.QuadPart);
                if(p.time+160000>now){timer.wait();continue;} // allow loopback packets to arrive
                // WASAPI can deliver the last loopback packet after the mic packet.
                // Retry within the latency budget instead of punching holes in output.
                const bool available=timeline.render(p.time,10000000,left.data(),right.data(),p.frames);
                if(!available&&p.time+300000>now){timer.wait();continue;}
                microphone_.pop();
                const auto losses=lost_.load();
                const auto drift=p.time>expectedTime?p.time-expectedTime:expectedTime-p.time;
                if(p.index!=expected||losses!=seenLost||(expectedTime&&drift>10000)){
                    model.reset();micFir.reset();refFir.reset();outFir.reset();count=0;processed=0;base=p.index;good=true;seenLost=losses;
                }
                expected=p.index+p.frames;expectedTime=p.time+(p.frames*10000000ull+24000)/48000;
                for(unsigned i=0;i<p.frames;++i){
                    const float m=micFir.tick(p.samples[i]),f=refFir.tick(available?left[i]:0.f);
                    good=good&&available;
                    if((p.index+i-base)%3==0){mic[count]=m;ref[count]=f;++count;}
                    if(count==128){
                        model.process(mic.data(),ref.data(),out.data());
                        for(unsigned j=0;j<hop;++j){
                            const float value=outFir.tick(j%3==0?out[j/3]*3.f:0.f);
                            const auto idx=base+processed+j;
                            if(good&&processed+j>=modelDelay){auto& s=results_[idx%ringSize];s.stamp.store(0,std::memory_order_release);s.sample.store(std::bit_cast<std::uint32_t>(value),std::memory_order_relaxed);s.stamp.store(idx+1,std::memory_order_release);}
                        }
                        completed_.fetch_add(1);processed+=hop;count=0;good=true;
                    }
                }
            }
        }catch(...){state_.store(-1);}
    }
    // Constructed last: worker must not observe uninitialized queue storage.
    std::thread worker_;
};
}
