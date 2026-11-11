// capture_compare.cpp
// SDR monitor capture to CPU-visible BGRA ROI. No saved screenshots or input APIs.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include "wgc_capture.hpp"
#pragma comment(lib,"d3d11.lib")
#pragma comment(lib,"dxgi.lib")
#pragma comment(lib,"gdi32.lib")
#pragma comment(lib,"user32.lib")
using Microsoft::WRL::ComPtr;
using Clock=std::chrono::steady_clock;
static double Milliseconds(Clock::time_point start){return std::chrono::duration<double,std::milli>(Clock::now()-start).count();}
static void Check(HRESULT result,const char* operation){if(FAILED(result))throw std::runtime_error(std::string(operation)+" HRESULT="+std::to_string(static_cast<unsigned>(result)));}
struct Event {HANDLE handle=CreateEventW(nullptr,FALSE,FALSE,nullptr);~Event(){if(handle)CloseHandle(handle);}};
struct GdiResources {
 HDC screen=nullptr,memory=nullptr;HBITMAP bitmap=nullptr;HGDIOBJ original=nullptr;void* pixels=nullptr;
 ~GdiResources(){if(memory&&original)SelectObject(memory,original);if(bitmap)DeleteObject(bitmap);if(memory)DeleteDC(memory);if(screen)ReleaseDC(nullptr,screen);}
 void Init(int side){screen=GetDC(nullptr);if(!screen)throw std::runtime_error("GetDC failed");memory=CreateCompatibleDC(screen);if(!memory)throw std::runtime_error("CreateCompatibleDC failed");BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=side;info.bmiHeader.biHeight=-side;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;bitmap=CreateDIBSection(screen,&info,DIB_RGB_COLORS,&pixels,nullptr,0);if(!bitmap)throw std::runtime_error("CreateDIBSection failed");original=SelectObject(memory,bitmap);if(!original||original==HGDI_ERROR){original=nullptr;throw std::runtime_error("SelectObject failed");}}
};
struct FrameLease {IDXGIOutputDuplication* duplication=nullptr;~FrameLease(){if(duplication)duplication->ReleaseFrame();}};
struct MapLease {ID3D11DeviceContext* context=nullptr;ID3D11Resource* resource=nullptr;~MapLease(){if(context)context->Unmap(resource,0);}};
int wmain(int argc,wchar_t** argv){
 try{
  if(argc!=4)throw std::runtime_error("Usage: capture-compare.exe gdi|dxgi|wgc seconds output.csv");
  const std::wstring mode=argv[1];if(mode!=L"gdi"&&mode!=L"dxgi"&&mode!=L"wgc")throw std::runtime_error("Invalid capture mode");
  wchar_t* end=nullptr;const double seconds=wcstod(argv[2],&end);if(end==argv[2]||*end||!std::isfinite(seconds)||seconds<1||seconds>120)throw std::runtime_error("Seconds must be finite in [1,120]");
  const std::filesystem::path csvPath(argv[3]);if(std::filesystem::exists(csvPath))throw std::runtime_error("Refusing to overwrite CSV");
  std::ofstream csv(csvPath);csv.exceptions(std::ios::failbit|std::ios::badbit);csv<<"elapsed_s,acquire_ms,readback_ms,release_ms,total_ms,source_age_ms,first_pixel\n";
  winrt::init_apartment(winrt::apartment_type::multi_threaded);
  ComPtr<IDXGIFactory1> factory;Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"CreateDXGIFactory1");
  ComPtr<IDXGIAdapter1> adapter;ComPtr<IDXGIOutput> output;DXGI_OUTPUT_DESC monitor{};bool found=false;
  for(UINT index=0;!found;++index){ComPtr<IDXGIAdapter1> candidate;HRESULT hr=factory->EnumAdapters1(index,&candidate);if(hr==DXGI_ERROR_NOT_FOUND)break;Check(hr,"EnumAdapters1");for(UINT screen=0;;++screen){ComPtr<IDXGIOutput> candidateOutput;hr=candidate->EnumOutputs(screen,&candidateOutput);if(hr==DXGI_ERROR_NOT_FOUND)break;Check(hr,"EnumOutputs");DXGI_OUTPUT_DESC description{};Check(candidateOutput->GetDesc(&description),"GetDesc");MONITORINFO info{sizeof(MONITORINFO)};if(GetMonitorInfoW(description.Monitor,&info)&&(info.dwFlags&MONITORINFOF_PRIMARY)){adapter=candidate;output=candidateOutput;monitor=description;found=true;break;}}}
  if(!found)throw std::runtime_error("No attached primary monitor found");
  const int width=monitor.DesktopCoordinates.right-monitor.DesktopCoordinates.left,height=monitor.DesktopCoordinates.bottom-monitor.DesktopCoordinates.top;
  constexpr int side=256;if(width<side||height<side)throw std::runtime_error("Monitor smaller than 256px ROI");const int left=(width-side)/2,top=(height-side)/2;
  ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL level{};Check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context),"D3D11CreateDevice");
  ComPtr<ID3D11Texture2D> staging;D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=side;desc.MipLevels=desc.ArraySize=1;desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;Check(device->CreateTexture2D(&desc,nullptr,&staging),"CreateTexture2D");
  ComPtr<IDXGIOutputDuplication> duplication;WgcCapture capture;Event activity;GdiResources gdi;
  if(!activity.handle)throw std::runtime_error("CreateEvent failed");
  if(mode==L"gdi")gdi.Init(side);
  else if(mode==L"dxgi"){ComPtr<IDXGIOutput1> output1;Check(output.As(&output1),"IDXGIOutput1");Check(output1->DuplicateOutput(device.Get(),&duplication),"DuplicateOutput");}
  else capture.Start(device.Get(),monitor.Monitor,activity.handle);
  LARGE_INTEGER frequency{};QueryPerformanceFrequency(&frequency);
  const auto started=Clock::now();std::uint64_t completed=0,timeouts=0;double sum=0,maximum=0;
  while(Milliseconds(started)<seconds*1000){
   const auto began=Clock::now();ComPtr<ID3D11Texture2D> texture;FrameLease lease;std::shared_ptr<WgcCapture::Frame> wgcFrame;double sourceAge=-1;std::uint32_t pixel=0;
   if(mode==L"gdi"){if(!BitBlt(gdi.memory,0,0,side,side,gdi.screen,monitor.DesktopCoordinates.left+left,monitor.DesktopCoordinates.top+top,SRCCOPY|CAPTUREBLT))throw std::runtime_error("BitBlt failed");if(!GdiFlush())throw std::runtime_error("GdiFlush failed");pixel=*static_cast<std::uint32_t*>(gdi.pixels);}
   else if(mode==L"dxgi"){
    DXGI_OUTDUPL_FRAME_INFO info{};ComPtr<IDXGIResource> resource;HRESULT result=duplication->AcquireNextFrame(100,&info,&resource);if(result==DXGI_ERROR_WAIT_TIMEOUT){++timeouts;continue;}Check(result,"AcquireNextFrame");lease.duplication=duplication.Get();Check(resource.As(&texture),"ID3D11Texture2D");
    if(info.LastPresentTime.QuadPart){LARGE_INTEGER now{};QueryPerformanceCounter(&now);sourceAge=1000.*double(now.QuadPart-info.LastPresentTime.QuadPart)/double(frequency.QuadPart);}
   }else{
    wgcFrame=capture.Take();if(!wgcFrame){DWORD result=WaitForSingleObject(activity.handle,100);if(result==WAIT_FAILED)throw std::runtime_error("WaitForSingleObject failed");wgcFrame=capture.Take();}if(!wgcFrame){++timeouts;continue;}texture=wgcFrame->texture;LARGE_INTEGER now{};QueryPerformanceCounter(&now);sourceAge=1000.*double(now.QuadPart)/double(frequency.QuadPart)-wgcFrame->presentedMs;
   }
   const double acquire=Milliseconds(began);const auto copying=Clock::now();
   if(texture){D3D11_TEXTURE2D_DESC actual{};texture->GetDesc(&actual);if(actual.Width!=UINT(width)||actual.Height!=UINT(height)||actual.Format!=desc.Format)throw std::runtime_error("Capture surface format/size changed; recovery not exercised by this benchmark");D3D11_BOX box{UINT(left),UINT(top),0,UINT(left+side),UINT(top+side),1};context->CopySubresourceRegion(staging.Get(),0,0,0,0,texture.Get(),0,&box);D3D11_MAPPED_SUBRESOURCE mapped{};Check(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Map readback");MapLease mappedLease{context.Get(),staging.Get()};pixel=*static_cast<const std::uint32_t*>(mapped.pData);}
   const double readback=Milliseconds(copying);const auto releasing=Clock::now();texture.Reset();wgcFrame.reset();if(lease.duplication){auto* owned=lease.duplication;lease.duplication=nullptr;Check(owned->ReleaseFrame(),"ReleaseFrame");}const double release=Milliseconds(releasing),total=Milliseconds(began);sum+=total;maximum=std::max(maximum,total);++completed;
   csv<<Milliseconds(started)/1000<<','<<acquire<<','<<readback<<','<<release<<','<<total<<','<<sourceAge<<','<<pixel<<'\n';
  }
  const auto counts=capture.Stats();capture.Stop();csv.flush();std::cout<<"completed="<<completed<<" timeouts="<<timeouts<<" mean_ms="<<(completed?sum/completed:0)<<" max_ms="<<maximum<<" published="<<counts.published<<" replaced="<<counts.replaced<<"\n";
  std::cout<<"Scope: CPU-visible SDR BGRA 256x256 ROI. GDI samples may repeat unchanged pixels; iteration rate is NOT fresh-frame FPS. No screenshots persisted. Resize/device-loss recovery not certified.\n";
  return completed?0:3;
 }catch(const winrt::hresult_error& error){std::cerr<<"WGC ERROR: "<<winrt::to_string(error.message())<<'\n';return 2;}
 catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
