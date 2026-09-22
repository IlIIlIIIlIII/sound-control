#include "AudioStream.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>
using namespace personaltools;
namespace {
int failures=0;
void expect(bool value,const char* message){if(!value){std::cerr<<"FAIL: "<<message<<'\n';++failures;}}
std::uint64_t timeAt(std::size_t frame){return 10000000ull+(frame*10000000ull+24000)/48000;}
void bypass(bool enabled) {
    auto stream=std::make_unique<EchoStream>();
    constexpr std::size_t n=10000;
    std::vector<float> input(n*2),output(n*2);
    for(std::size_t i=0;i<n;++i){input[i*2]=std::sin(i*0.037f)*0.2f;input[i*2+1]=0.8f;}
    const std::size_t sizes[]{1,47,480,127,256,441,960};
    std::size_t offset=0,index=0;
    while(offset<n){const auto count=std::min(sizes[index++%7],n-offset);
        stream->microphone(input.data()+offset*2,output.data()+offset*2,count,2,2,timeAt(offset),false,enabled);offset+=count;}
    for(std::size_t i=0;i<n;++i){const auto expected=i<EchoStream::latencyFrames?0.f:input[(i-EchoStream::latencyFrames)*2];
        if(output[i*2]!=expected||output[i*2+1]!=expected){expect(false,"variable buffers preserve input 1 with exact declared latency and stereo duplication");break;}}
}
void alignedEcho() {
    constexpr std::size_t n=48000*8;
    auto stream=std::make_unique<EchoStream>();
    std::vector<float> reference(n*2),mic(n),output(n);
    std::uint32_t seed=1721;
    for(std::size_t i=0;i<n;++i){seed=seed*1664525u+1013904223u;reference[i*2]=((seed>>8)/8388608.f-1)*0.1f;reference[i*2+1]=reference[i*2]*0.8f;
        if(i>=300)mic[i]=reference[(i-300)*2]*0.55f;}
    for(std::size_t offset=0;offset<n;offset+=480){
        stream->reference(reference.data()+offset*2,480,2,timeAt(offset),false);
        stream->microphone(mic.data()+offset,output.data()+offset,480,1,1,timeAt(offset),false,true);
    }
    double before=0,after=0;
    for(std::size_t i=n/2;i<n;++i){before+=mic[i-EchoStream::latencyFrames]*mic[i-EchoStream::latencyFrames];after+=output[i]*output[i];}
    const auto reduction=10*std::log10(before/std::max(after,1e-20));
    expect(reduction>20,"timestamped variable-packet AEC reduces synthetic echo by >20 dB");
    std::cout<<"Stream AEC reduction: "<<reduction<<" dB\n";
    // Discontinuity must throw away queued speech rather than playing stale data.
    std::array<float,480> silent{},out{};
    stream->microphone(silent.data(),out.data(),480,1,1,timeAt(n+48000),false,true);
    expect(std::all_of(out.begin(),out.end(),[](float f){return f==0;}),"clock jump clears stale microphone output");
}
void overflowAndConcurrent() {
    auto stream=std::make_unique<EchoStream>();
    std::array<float,256*2> reference{};
    for(std::size_t i=0;i<300;++i)stream->reference(reference.data(),256,2,timeAt(i*256),false);
    expect(stream->dropped()>0,"bounded reference queue reports overflow");
    std::array<float,256> mic{},out{};
    stream->microphone(mic.data(),out.data(),256,1,1,timeAt(300*256),false,true);
    stream->reset();
    std::thread producer([&]{for(std::size_t i=0;i<2000;++i)stream->reference(reference.data(),256,2,timeAt(i*256),false);});
    for(std::size_t i=0;i<2000;++i)stream->microphone(mic.data(),out.data(),256,1,1,timeAt(i*256),false,true);
    producer.join();
    expect(std::all_of(out.begin(),out.end(),[](float v){return std::isfinite(v);}),"concurrent callbacks and queue overflow remain finite");
}
}
int main(){bypass(false);bypass(true);alignedEcho();overflowAndConcurrent();return failures?1:0;}
