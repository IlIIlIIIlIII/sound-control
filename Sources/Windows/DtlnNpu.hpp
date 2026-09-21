#pragma once
// DTLN-AEC (Nils L. Westhausen), MIT-licensed weights. Inference is NPU-only.
// This object belongs exclusively to a background worker, never APOProcess.
#include "SignalOps.hpp"
#include <windows.h>
#include <array>
#include <filesystem>
#include <cstring>
#include <stdexcept>

namespace soundcontrol::win {
class DtlnNpu {
    using Status=int;
    using Create=Status(__cdecl*)(void**);
    using Compile=Status(__cdecl*)(const void*,const char*,const char*,size_t,void**,...);
    using Request=Status(__cdecl*)(const void*,void**);
    using Tensor=Status(__cdecl*)(const void*,size_t,void**);
    using Data=Status(__cdecl*)(const void*,void**);
    using Infer=Status(__cdecl*)(void*);
    using Free=void(__cdecl*)(void*);
    HMODULE dll_=nullptr;
    void* core_=nullptr;
    std::array<void*,2> models_{},requests_{};
    std::array<std::array<void*,5>,2> tensors_{};
    std::array<std::array<float*,5>,2> values_{};
    Create create_=nullptr;Compile compile_=nullptr;Request request_=nullptr;
    Tensor input_=nullptr,output_=nullptr;Data data_=nullptr;Infer infer_=nullptr;
    Free coreFree_=nullptr,modelFree_=nullptr,requestFree_=nullptr,tensorFree_=nullptr;
    signal::FFTSetup fft_=nullptr;
    std::array<float,512> mic_{},ref_{},overlap_{},real_{},imag_{},mr_{},mi_{};
    template<class T> void load(T& target,const char* name){
        target=reinterpret_cast<T>(GetProcAddress(dll_,name));
        if(!target)throw std::runtime_error(name);
    }
    static void check(Status status){if(status!=0)throw std::runtime_error("OpenVINO NPU error "+std::to_string(status));}
public:
    DtlnNpu()=default;
    DtlnNpu(const DtlnNpu&)=delete;
    ~DtlnNpu(){
        for(auto& ts:tensors_)for(auto t:ts)if(t&&tensorFree_)tensorFree_(t);
        for(auto r:requests_)if(r&&requestFree_)requestFree_(r);
        for(auto m:models_)if(m&&modelFree_)modelFree_(m);
        if(core_&&coreFree_)coreFree_(core_);
        if(fft_)signal::destroyFFT(fft_);
        if(dll_)FreeLibrary(dll_);
    }
    void open(const std::filesystem::path& directory){
        dll_=LoadLibraryExW((directory/L"openvino_c.dll").c_str(),nullptr,
                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if(!dll_)throw std::runtime_error("Cannot load OpenVINO C runtime");
        load(create_,"ov_core_create");load(compile_,"ov_core_compile_model_from_file");
        load(request_,"ov_compiled_model_create_infer_request");
        load(input_,"ov_infer_request_get_input_tensor_by_index");
        load(output_,"ov_infer_request_get_output_tensor_by_index");
        load(data_,"ov_tensor_data");load(infer_,"ov_infer_request_infer");
        load(coreFree_,"ov_core_free");load(modelFree_,"ov_compiled_model_free");
        load(requestFree_,"ov_infer_request_free");load(tensorFree_,"ov_tensor_free");
        check(create_(&core_));
        for(int i=0;i<2;++i){
            auto path=(directory/("dtln_aec_256_"+std::to_string(i+1)+".tflite")).string();
            check(compile_(core_,path.c_str(),"NPU",0,&models_[i]));
            check(request_(models_[i],&requests_[i]));
            for(size_t j=0;j<5;++j){
                check(j<3?input_(requests_[i],j,&tensors_[i][j]):output_(requests_[i],j-3,&tensors_[i][j]));
                check(data_(tensors_[i][j],reinterpret_cast<void**>(&values_[i][j])));
            }
        }
        fft_=signal::createFFT(9,signal::radix2);
        if(!fft_)throw std::bad_alloc();
        reset();
    }
    void reset(){
        mic_.fill(0);ref_.fill(0);overlap_.fill(0);
        for(auto& v:values_)if(v[1])std::fill_n(v[1],1024,0.f);
    }
    void process(const float* microphone,const float* reference,float* out){
        std::move(mic_.begin()+128,mic_.end(),mic_.begin());
        std::move(ref_.begin()+128,ref_.end(),ref_.begin());
        std::copy_n(microphone,128,mic_.begin()+384);
        std::copy_n(reference,128,ref_.begin()+384);
        signal::SplitComplex z{real_.data(),imag_.data()};
        real_=mic_;imag_.fill(0);signal::fft(fft_,&z,1,9,signal::forward);mr_=real_;mi_=imag_;
        for(int k=0;k<257;++k)values_[0][0][k]=std::hypot(real_[k],imag_[k]);
        real_=ref_;imag_.fill(0);signal::fft(fft_,&z,1,9,signal::forward);
        for(int k=0;k<257;++k)values_[0][2][k]=std::hypot(real_[k],imag_[k]);
        check(infer_(requests_[0]));std::copy_n(values_[0][4],1024,values_[0][1]);
        for(int k=0;k<512;++k){const auto bin=k<=256?k:512-k;real_[k]=mr_[k]*values_[0][3][bin];imag_[k]=mi_[k]*values_[0][3][bin];}
        signal::fft(fft_,&z,1,9,signal::inverse);
        for(int k=0;k<512;++k){values_[1][0][k]=real_[k]/512.f;values_[1][2][k]=ref_[k];}
        check(infer_(requests_[1]));std::copy_n(values_[1][4],1024,values_[1][1]);
        std::move(overlap_.begin()+128,overlap_.end(),overlap_.begin());std::fill(overlap_.begin()+384,overlap_.end(),0.f);
        for(int k=0;k<512;++k)overlap_[k]+=values_[1][3][k];
        float inputEnergy=0.f,outputEnergy=0.f,referenceEnergy=0.f;
        for(int k=0;k<128;++k){
            if(!std::isfinite(overlap_[k]))throw std::runtime_error("Nonfinite NPU output");
            out[k]=overlap_[k];
            // The first 128 samples of the rolling window correspond to this
            // output hop; comparing with the newest input misclassifies onsets.
            inputEnergy+=mic_[k]*mic_[k];outputEnergy+=out[k]*out[k];
        }
        for(float v:ref_)referenceEnergy+=v*v;
        // A near-end utterance can bias the recurrent states long after it ends.
        // Relax that memory only on hops already strongly suppressed by the
        // model, with an active reference. Do not gate audio, reset overlap, or
        // damp states while the model is passing speech. The 0.995 factor is
        // validated against local voice-on/off and quiet-voice replay fixtures.
        if(referenceEnergy>1e-7f&&outputEnergy<inputEnergy*.1f)
            for(auto& v:values_)for(int k=0;k<1024;++k)v[1][k]*=.995f;
    }
};
}
