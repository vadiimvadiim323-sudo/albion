#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d10.h>
#include <iostream>
#include <filesystem>
#include <dxgi.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "gpu_infer.hpp"
#include "gpu_preprocessor.hpp"
#pragma comment(lib,"d3d11.lib")
#pragma comment(lib,"dxgi.lib")
#pragma comment(lib,"d3dcompiler.lib")
#pragma comment(lib,"shell32.lib")
#pragma comment(lib,"ole32.lib")
struct Region {LONG left=0,top=0;UINT width=256,height=256;};
struct Stats { std::vector<double> times; double seconds=0,cpuSeconds=0; void Print(const char* label) {auto t=times;std::sort(t.begin(),t.end());double mean=std::accumulate(t.begin(),t.end(),0.0)/t.size();auto q=[&](double x){return t[(std::min)(t.size()-1,size_t(std::ceil(x*t.size())-1))];};std::cout<<label<<" count="<<t.size()<<" seconds="<<seconds<<" fps="<<t.size()/seconds<<" mean_ms="<<mean<<" p50_ms="<<q(.50)<<" p95_ms="<<q(.95)<<" p99_ms="<<q(.99)<<" cpu_seconds="<<cpuSeconds<<"\n";}};
static double CpuSeconds(){FILETIME c,e,k,u;GetProcessTimes(GetCurrentProcess(),&c,&e,&k,&u);ULARGE_INTEGER a{},b{};a.LowPart=k.dwLowDateTime;a.HighPart=k.dwHighDateTime;b.LowPart=u.dwLowDateTime;b.HighPart=u.dwHighDateTime;return double(a.QuadPart+b.QuadPart)/1e7;}
int wmain(int argc,wchar_t**argv){
 try{
  if(argc<4)throw std::runtime_error("usage: performance_bench MODEL.engine cpu|io|shared SECONDS [CSV]");
  std::wstring mode=argv[2];const bool spin=mode.size()>5 && mode.substr(mode.size()-5)==L"-poll";if(spin)mode.resize(mode.size()-5);SetEnvironmentVariableW(L"ALBION_CUDA_POLL",spin?L"1":L"0");const double duration=_wtof(argv[3]);if(duration<1||duration>120)throw std::runtime_error("seconds must be 1..120");
  const bool shared=mode==L"shared",io=mode==L"io";if(!shared&&!io&&mode!=L"cpu")throw std::runtime_error("invalid mode");
  SetEnvironmentVariableW(L"ALBION_IO_GRAPH",io?L"1":L"0");SetEnvironmentVariableW(L"ALBION_GPU_TIMING",L"0");
  // The harness contains no authentication, capture images, input injection or HID.
  TrtBackend backend;std::string error;if(!backend.Init(argv[1],640,error))throw std::runtime_error(error);
  const int side=backend.InputSide();
  ComPtr<IDXGIFactory1> factory;PreprocessCheck(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"DXGI factory");
  ComPtr<IDXGIAdapter1> adapter;int activeCuda=-1;cudaGetDevice(&activeCuda);
  for(UINT i=0;;++i){ComPtr<IDXGIAdapter1> candidate;if(factory->EnumAdapters1(i,&candidate)==DXGI_ERROR_NOT_FOUND)break;int device=-2;if(cudaD3D11GetDevice(&device,candidate.Get())==cudaSuccess&&device==activeCuda){adapter=candidate;break;}}
  if(!adapter)throw std::runtime_error("No D3D11 adapter matching the active CUDA device");
  ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
  PreprocessCheck(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"D3D11CreateDevice");
  ComPtr<ID3D10Multithread> multi;if(SUCCEEDED(device.As(&multi)))multi->SetMultithreadProtected(TRUE);
  DXGI_ADAPTER_DESC1 desc{};adapter->GetDesc1(&desc);std::wcout<<L"adapter="<<desc.Description<<L"\n";
  GpuPreprocessor pre;pre.Init(device.Get());Region region{0,0,UINT(side),UINT(side)};
  std::vector<unsigned char> pixels(size_t(side)*side*4);
  auto fill=[&](int pattern){for(int y=0;y<side;++y)for(int x=0;x<side;++x){size_t i=(size_t(y)*side+x)*4;pixels[i]=static_cast<unsigned char>((x*3+pattern*29)%256);pixels[i+1]=static_cast<unsigned char>((y*7+pattern*17)%256);pixels[i+2]=static_cast<unsigned char>((x+y+pattern*41)%256);pixels[i+3]=255;}};fill(0);
  D3D11_TEXTURE2D_DESC td{};td.Width=side;td.Height=side;td.MipLevels=td.ArraySize=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA init{pixels.data(),UINT(side*4),0};ComPtr<ID3D11Texture2D> texture;PreprocessCheck(device->CreateTexture2D(&td,&init,&texture),"test texture");ComPtr<ID3D11ShaderResourceView> srv;PreprocessCheck(device->CreateShaderResourceView(texture.Get(),nullptr,&srv),"test SRV");
  std::vector<float> input(size_t(side)*side*3),scratch;InferenceOutputView output{};int channels=0,anchors=0;uint64_t frame=0;
  auto cpuTensor=[&](){pre.SetSharedMode(false);LetterboxInfo lb;pre.Submit(context.Get(),srv.Get(),side,side,region,0,0,++frame,0,0,0,0,side);const double until=AimBackendNowMs()+5000;while(!pre.TryFetch(context.Get(),input.data(),lb)){if(AimBackendNowMs()>until)throw std::runtime_error("preprocess timeout");SwitchToThread();}};
  if(shared){void* buffers[]={pre.TensorBuffer(0),pre.TensorBuffer(1)};if(!backend.BindSharedInputs(device.Get(),buffers,2,error))throw std::runtime_error(error);}
  auto run=[&](){
   if(shared){pre.SetSharedMode(true);const size_t slot=frame%2;const double until=AimBackendNowMs()+5000;while(!pre.CanWriteSharedSlot(context.Get(),slot)){if(AimBackendNowMs()>until)throw std::runtime_error("slot timeout");SwitchToThread();}pre.Submit(context.Get(),srv.Get(),side,side,region,0,0,++frame,0,0,0,slot,side);if(!backend.RunSharedSlotView(slot,scratch,output,channels,anchors,error))throw std::runtime_error(error);}
   else{cpuTensor();if(!backend.RunView(input.data(),scratch,output,channels,anchors,error))throw std::runtime_error(error);}
  };
  double maxError=0;
  for(int pattern=0;pattern<8;++pattern){fill(pattern);context->UpdateSubresource(texture.Get(),0,nullptr,pixels.data(),side*4,0);cpuTensor();if(!backend.RunView(input.data(),scratch,output,channels,anchors,error))throw std::runtime_error(error);std::vector<float> expected(output.data,output.data+output.count);run();if(output.count!=expected.size())throw std::runtime_error("output shape changed");for(size_t i=0;i<expected.size();++i){if(!std::isfinite(output.data[i])||!std::isfinite(expected[i]))throw std::runtime_error("nonfinite output");maxError=(std::max)(maxError,double(std::abs(output.data[i]-expected[i])));}}
  if(maxError>1e-4)throw std::runtime_error("correctness check failed");
  std::cout<<"correctness=PASS patterns=8 max_abs_error="<<maxError<<"\n"<<"backend="<<backend.Diagnostics()<<"\n";
  const double warm=AimBackendNowMs()+2000;while(AimBackendNowMs()<warm)run();
  Stats stats;stats.times.reserve(size_t(duration*3000));const double start=AimBackendNowMs(),cpuStart=CpuSeconds();
  while(AimBackendNowMs()-start<duration*1000){const double begin=AimBackendNowMs();run();stats.times.push_back(AimBackendNowMs()-begin);}
  stats.seconds=(AimBackendNowMs()-start)/1000;stats.cpuSeconds=CpuSeconds()-cpuStart;stats.Print("RESULT");
  if(argc>4){std::ofstream csv{std::filesystem::path(argv[4])};csv<<"iteration,preprocess_backend_ms\n";for(size_t i=0;i<stats.times.size();++i)csv<<i<<","<<std::setprecision(10)<<stats.times[i]<<"\n";}
  if(shared)backend.UnbindSharedInput();DevShutdownLog();return 0;
 }catch(const std::exception&e){std::cerr<<"FAILED: "<<e.what()<<"\n";DevShutdownLog();return 1;}
}
