#include "NeuralStream.hpp"
#include <memory>
#include <cstdio>

int main(){
    using soundcontrol::win::NeuralStream;
    soundcontrol::win::NeuralOutputMixer mixer;
    for(unsigned i=0;i<250;++i)mixer.process(.9f,.05f,true,true);
    float previous=.05f;
    for(unsigned i=0;i<240;++i){
        const float value=mixer.process(.9f,.9f,false,true);
        if(std::abs(value-previous)>.004f){puts("Missing neural frame jumped directly to raw audio");return 1;}
        previous=value;
    }
    if(std::abs(previous-.9f)>.0001f){puts("Failure fallback did not reach raw audio");return 1;}
    for(unsigned i=0;i<240;++i){
        const float value=mixer.process(.9f,.05f,true,true);
        if(std::abs(value-previous)>.004f){puts("Recovered neural output jumped");return 1;}
        previous=value;
    }
    if(std::abs(previous-.05f)>.0001f){puts("Recovered model remained bypassed");return 1;}
    auto stream=std::make_unique<NeuralStream>(L"Z:\\SoundControl-absent-test-model");
    std::array<float,514> input{},output{};
    // Irregular packet sizes span both callback and 384-sample neural hop boundaries.
    // A missing runtime must preserve input 1 with fixed latency, including toggle changes.
    unsigned index=0;
    for(unsigned callback=0;callback<100;++callback){
        const unsigned frames=callback%2?257:113;
        for(unsigned i=0;i<frames;++i){input[i*2]=float((index+i)%997)/2000.f;input[i*2+1]=-0.9f;}
        stream->microphone(input.data(),output.data(),frames,2,2,10000000ull+index*10000000ull/48000,false,callback%3!=0);
        for(unsigned i=0;i<frames;++i){
            auto sample=index+i;const float wanted=sample<NeuralStream::latencyFrames?0.f:float((sample-NeuralStream::latencyFrames)%997)/2000.f;
            if(output[i*2]!=wanted||output[i*2+1]!=wanted){puts("Neural failure fallback changed microphone samples/latency");return 1;}
        }
        index+=frames;
    }
    puts("Neural failure fallback and channel/latency preservation passed");
}
