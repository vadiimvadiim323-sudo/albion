#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <filesystem>
#include <iostream>
#include <vector>
#include <cmath>
#include "gpu_infer.hpp"
#pragma comment(lib,"d3d11.lib")
#pragma comment(lib,"dxgi.lib")
#pragma comment(lib,"shell32.lib")
#pragma comment(lib,"ole32.lib")
int wmain(int argc,wchar_t**argv) {
    try {
        if(argc!=2)throw std::runtime_error("validate_engine MODEL.engine");
        SetEnvironmentVariableW(L"ALBION_IO_GRAPH",L"0");
        SetEnvironmentVariableW(L"ALBION_OUTPUT_GRAPH",L"0");
        SetEnvironmentVariableW(L"ALBION_CUDA_POLL",L"0");
        SetEnvironmentVariableW(L"ALBION_GPU_TIMING",L"0");
        TrtBackend engine;std::string error;
        if(!engine.Init(argv[1],640,error))throw std::runtime_error(error);
        const int side=engine.InputSide();
        std::vector<float> input(size_t(3)*side*side),scratch;
        InferenceOutputView view{};int channels=0,anchors=0;
        for(unsigned pattern=0;pattern<8;++pattern) {
            for(size_t i=0;i<input.size();++i)input[i]=pattern<4?float(pattern)/4.f:float((i*17+pattern*29)%256)/255.f;
            if(!engine.RunView(input.data(),scratch,view,channels,anchors,error))throw std::runtime_error(error);
            // This converter's UI explicitly targets raw channel-first YOLO.
            // Shape alone cannot prove objectness/NMS semantics or accuracy.
            if(channels<5||channels>512||anchors<=channels)
                throw std::runtime_error("Unsupported raw channel-first output; expected [1,C,N], 5<=C<=512, N>C");
            if(!view.data||view.count!=size_t(channels)*size_t(anchors))throw std::runtime_error("Output tensor count mismatch");
            for(size_t i=0;i<view.count;++i)if(!std::isfinite(view.data[i]))throw std::runtime_error("Non-finite inference output");
        }
        std::cout<<"PASS engine runtime smoke: 8 synthetic patterns; input=[1,3,"<<side<<","<<side<<"]; output=[1,"<<channels<<","<<anchors<<"]\n";
        std::cout<<"NOT an accuracy test, ONNX parity test or speed comparison.\n";
        DevShutdownLog();return 0;
    } catch(const std::exception& e) {std::cerr<<"FAILED: "<<e.what()<<"\n";DevShutdownLog();return 1;}
}
