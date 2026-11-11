#pragma once
// Experimental Win10 1903+ SDR monitor capture. Not included in default build.
// FrameArrived never touches the app's immediate D3D context.
#include "latest_frame.hpp"
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <memory>
#include <atomic>
#include <string>
#include <stdexcept>
#pragma comment(lib,"windowsapp.lib")

class WgcCapture {
    using FramePool=winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
    using CaptureItem=winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
    using CaptureSession=winrt::Windows::Graphics::Capture::GraphicsCaptureSession;
    using WinDevice=winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
    using CaptureFrame=winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame;
public:
    struct Frame {
        CaptureFrame owner{nullptr};
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        double receivedMs=0,presentedMs=0;
        ~Frame(){if(owner){try{owner.Close();}catch(...){}}}
    };
private:
    struct State {
        LatestFrameMailbox<std::shared_ptr<Frame>> mailbox;
        std::atomic<bool> accepting{true},failed{false};
        std::mutex errorMutex,callbackMutex;std::string error;
        HANDLE activity=nullptr;int width=0,height=0;
        ~State(){if(activity)CloseHandle(activity);}
        // FIX-V11-03/09: failure reporting inside a noexcept WinRT callback
        // must itself survive allocation/lock failures; keep a no-allocation bit.
        void Fail(const char* message) noexcept {
            accepting.store(false);failed.store(true);
            try {std::lock_guard<std::mutex> lock(errorMutex);if(error.empty())error=message;} catch(...){}
            try {mailbox.Close();} catch(...){}
            if(activity)SetEvent(activity);
        }
    };
    std::shared_ptr<State> mState;
    FramePool mPool{nullptr};CaptureSession mSession{nullptr};
    WinDevice mDevice{nullptr};winrt::event_token mToken{};bool mSubscribed=false;
    static double NowMs(){
        static const double scale=[](){LARGE_INTEGER f{};QueryPerformanceFrequency(&f);return f.QuadPart?1000.0/double(f.QuadPart):0.0;}();
        LARGE_INTEGER t{};QueryPerformanceCounter(&t);return double(t.QuadPart)*scale;
    }
public:
    ~WgcCapture(){Stop();}
    void Start(ID3D11Device* device,HMONITOR monitor,HANDLE activity) {
        Stop();
        if(!CaptureSession::IsSupported())throw std::runtime_error("WGC is not supported on this Windows installation");
        winrt::com_ptr<IDXGIDevice> dxgi;
        winrt::check_hresult(device->QueryInterface(__uuidof(IDXGIDevice),dxgi.put_void()));
        winrt::com_ptr<::IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(),inspectable.put()));
        mDevice=inspectable.as<WinDevice>();
        CaptureItem item{nullptr};
        auto factory=winrt::get_activation_factory<CaptureItem,IGraphicsCaptureItemInterop>();
        winrt::check_hresult(factory->CreateForMonitor(monitor,
            winrt::guid_of<winrt::Windows::Graphics::Capture::IGraphicsCaptureItem>(),winrt::put_abi(item)));
        auto size=item.Size();
        auto state=std::make_shared<State>();state->width=size.Width;state->height=size.Height;
        if(size.Width<=0 || size.Height<=0)throw std::runtime_error("WGC monitor has an invalid size");
        if(activity && !DuplicateHandle(GetCurrentProcess(),activity,GetCurrentProcess(),&state->activity,0,FALSE,DUPLICATE_SAME_ACCESS))
            throw std::runtime_error("Cannot duplicate capture notification event");
        mState=state;
        // Three surfaces: processing frame + pending newest frame + acquisition.
        mPool=FramePool::CreateFreeThreaded(mDevice,
            winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,3,size);
        mSession=mPool.CreateCaptureSession(item);
        try{mSession.IsCursorCaptureEnabled(false);}catch(const winrt::hresult_error&){}
        mToken=mPool.FrameArrived([state](const FramePool& sender,const winrt::Windows::Foundation::IInspectable&) noexcept {
            if(!state->accepting.load())return;
            try {
                std::lock_guard<std::mutex> callbackLock(state->callbackMutex);
                for(int drain=0;drain<8 && state->accepting.load();++drain) {
                    auto frame=sender.TryGetNextFrame();if(!frame)break;
                    auto owned=std::make_shared<Frame>();owned->owner=std::move(frame);
                    const auto size=owned->owner.ContentSize();
                    if(size.Width!=state->width || size.Height!=state->height) {
                        state->Fail("WGC monitor size changed; restart capture/application");return;
                    }
                    auto access=owned->owner.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                    winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D),
                        reinterpret_cast<void**>(owned->texture.GetAddressOf())));
                    owned->receivedMs=NowMs();
                    owned->presentedMs=double(owned->owner.SystemRelativeTime().count())/10000.0;
                    if(state->mailbox.Publish(std::move(owned)) && state->activity)SetEvent(state->activity);
                }
            } catch(const winrt::hresult_error& e) {
                try {const auto message=winrt::to_string(e.message());state->Fail(message.c_str());}
                catch(...){state->Fail("WGC HRESULT failure (message unavailable)");}
            } catch(const std::exception& e){state->Fail(e.what());}
              catch(...){state->Fail("WGC callback failed");}
        });
        mSubscribed=true;mSession.StartCapture();
    }
    void Stop() noexcept {
        auto state=mState;
        if(state){state->accepting.store(false);try{state->mailbox.Close();}catch(...){}}
        if(mPool && mSubscribed){try{mPool.FrameArrived(mToken);}catch(...){}mSubscribed=false;}
        if(mSession){try{mSession.Close();}catch(...){}mSession=nullptr;}
        if(mPool){try{mPool.Close();}catch(...){}mPool=nullptr;}
        mDevice=nullptr;mState.reset();
    }
    int Width()const{return mState?mState->width:0;}
    int Height()const{return mState?mState->height:0;}
    std::shared_ptr<Frame> Take() {
        auto state=mState;if(!state)return {};
        if(state->failed.load()) {
            std::lock_guard<std::mutex> lock(state->errorMutex);
            throw std::runtime_error(state->error.empty()?"WGC capture failed; use recovery in F8":state->error);
        }
        auto latest=state->mailbox.Take();return latest?std::move(*latest):std::shared_ptr<Frame>{};
    }
    LatestFrameMailbox<std::shared_ptr<Frame>>::Counts Stats()const{return mState?mState->mailbox.Stats():LatestFrameMailbox<std::shared_ptr<Frame>>::Counts{};}
};
