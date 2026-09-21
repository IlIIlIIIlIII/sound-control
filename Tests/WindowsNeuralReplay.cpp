// Manual local-model replay: runtime directory, interleaved 16 kHz float32
// microphone/reference file, output file, optional repeat count. Not in CTest:
// requires the explicitly installed Intel NPU runtime and private audio fixtures.
#include "DtlnNpu.hpp"
#include <cstdio>
#include <chrono>
#include <vector>
int main(int argc,char** argv){
    if(argc<4||argc>5)return 2;
    try{
        FILE* input=fopen(argv[2],"rb");if(!input)return 3;
        std::vector<float> samples;float chunk[256];
        while(fread(chunk,sizeof(float),256,input)==256)samples.insert(samples.end(),chunk,chunk+256);
        fclose(input);if(samples.empty())return 4;
        soundcontrol::win::DtlnNpu model;model.open(std::filesystem::absolute(argv[1]));
        FILE* output=fopen(argv[3],"wb");if(!output)return 5;
        const int repeats=argc==5?std::clamp(atoi(argv[4]),1,100):1;
        float mic[128],ref[128],out[128];unsigned blocks=0;
        const auto start=std::chrono::steady_clock::now();
        for(int pass=0;pass<repeats;++pass)for(size_t pos=0;pos<samples.size();pos+=256){
            for(int k=0;k<128;++k){mic[k]=samples[pos+k*2];ref[k]=samples[pos+k*2+1];}
            model.process(mic,ref,out);
            if(fwrite(out,sizeof(float),128,output)!=128){fclose(output);return 6;}
            ++blocks;
        }
        fclose(output);
        printf("device=NPU blocks=%u elapsed_seconds=%.3f\n",blocks,
            std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count());
    }catch(const std::exception& e){puts(e.what());return 1;}
}
