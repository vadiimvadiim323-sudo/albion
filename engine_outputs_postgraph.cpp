#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <cmath>
#include "gpu_infer_postgraph.hpp"
#pragma comment(lib,"d3d11.lib")
#pragma comment(lib,"dxgi.lib")
#pragma comment(lib,"shell32.lib")
#pragma comment(lib,"ole32.lib")
int wmain(int argc,wchar_t**argv){try{
 if(argc!=3)throw std::runtime_error("engine_outputs MODEL.engine OUTPUT.bin");
 SetEnvironmentVariableW(L"ALBION_IO_GRAPH",L"0");SetEnvironmentVariableW(L"ALBION_CUDA_POLL",L"0");SetEnvironmentVariableW(L"ALBION_GPU_TIMING",L"0");SetEnvironmentVariableW(L"ALBION_OUTPUT_GRAPH",L"1");
 TrtBackend backend;std::string error;if(!backend.Init(argv[1],640,error))throw std::runtime_error(error);
 const int side=backend.InputSide();if(side!=256)throw std::runtime_error("validation corpus defined for 256x256");
 std::ofstream out{std::filesystem::path(argv[2]),std::ios::binary|std::ios::trunc};if(!out)throw std::runtime_error("cannot create output");
 std::vector<float> input(size_t(3)*side*side),scratch;InferenceOutputView view{};int ch=0,anchors=0;
 for(uint32_t pattern=0;pattern<32;++pattern){
  for(size_t i=0;i<input.size();++i){unsigned byte=pattern<8?pattern*32:unsigned((i*17+pattern*29+i/(side*5))%256);input[i]=float(byte)/255.0f;}
  if(!backend.RunView(input.data(),scratch,view,ch,anchors,error))throw std::runtime_error(error);
  if(!pattern){uint32_t header[]={32,uint32_t(ch),uint32_t(anchors),uint32_t(side)};out.write(reinterpret_cast<const char*>(header),sizeof(header));}
  for(size_t i=0;i<view.count;++i)if(!std::isfinite(view.data[i]))throw std::runtime_error("nonfinite output");
  out.write(reinterpret_cast<const char*>(view.data),std::streamsize(view.count*sizeof(float)));
 }
 out.close();if(!out)throw std::runtime_error("output write failed");std::cout<<"PASS finite outputs: 32 synthetic patterns; shape="<<ch<<"x"<<anchors<<"\n";DevShutdownLog();return 0;
}catch(const std::exception&e){std::cerr<<"FAILED: "<<e.what()<<"\n";DevShutdownLog();return 1;}}
