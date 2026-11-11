#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define AIM_ENABLE_ORT
#include <windows.h>
#include <d3d11.h>
#include <chrono>
#include <cmath>
#include <iostream>
#include <filesystem>
#include <stdexcept>
#include "gpu_infer.hpp"
int wmain(int argc,wchar_t** argv) {
    try {
        if(argc!=3) throw std::runtime_error("usage: ort_smoke MODEL.onnx dml|cpu");
        const bool cpu=std::wstring(argv[2])==L"cpu";
        SetEnvironmentVariableW(L"ALBION_ORT_CPU",cpu?L"1":L"0");
        OrtBackend backend;std::string error;
        if(!backend.Init(argv[1],640,error)) throw std::runtime_error(error);
        std::cout<<backend.Diagnostics()<<"\n";
        if(!cpu && std::string(backend.Name())!="ORT/DirectML") throw std::runtime_error("GPU test silently fell back to CPU");
        const int side=backend.InputSide();std::vector<float> input(size_t(side)*side*3),scratch,first;
        InferenceOutputView view{};int c=0,a=0;
        for(int pattern=0;pattern<3;++pattern) {
            for(size_t i=0;i<input.size();++i) input[i]=pattern?float((i*17+pattern*31)%256)/255.0f:0.0f;
            if(!backend.RunView(input.data(),scratch,view,c,a,error)) throw std::runtime_error(error);
            if(!view.data||view.count!=size_t(c)*a) throw std::runtime_error("bad output storage");
            bool changed=false;
            for(size_t i=0;i<view.count;++i){if(!std::isfinite(view.data[i]))throw std::runtime_error("nonfinite output");if(pattern&&view.data[i]!=first[i])changed=true;}
            if(!pattern) first.assign(view.data,view.data+view.count);
            if(pattern&&!changed)throw std::runtime_error("changed input produced identical output");
        }
        std::vector<float> expected(view.data,view.data+view.count);
        if(!backend.Run(input.data(),scratch,c,a,error))throw std::runtime_error(error);
        if(scratch.size()!=expected.size())throw std::runtime_error("copy/view mismatch");
        float maxError=0;
        for(size_t i=0;i<scratch.size();++i) maxError=(std::max)(maxError,std::abs(scratch[i]-expected[i]));
        if(maxError>0.001f)throw std::runtime_error("same-input repeatability check failed");
        std::cout<<"PASS side="<<side<<" dims="<<c<<","<<a<<" changed_input=yes repeat_max_error="<<maxError<<"\n";
        return 0;
    } catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<"\n";return 1;}
}
