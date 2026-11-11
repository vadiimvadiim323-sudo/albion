// modules/capture/capture.cpp
#include "modules/app/app.hpp"

void App::CreateDevice()
{
        // FIX-V11-13: Choose an OUTPUT first, then create D3D on its adapter.
        // Default = Windows primary monitor, not arbitrary adapter/output index 0.
        // ALBION_CAPTURE_MONITOR may name a specific output, e.g. \\.\DISPLAY2.
        ComPtr<IDXGIFactory1> factory; HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
        ComPtr<IDXGIAdapter1> chosenAdapter; ComPtr<IDXGIOutput> chosenOutput;
        DXGI_OUTPUT_DESC chosen{};
        wchar_t requested[128]{};
        const DWORD n = GetEnvironmentVariableW(L"ALBION_CAPTURE_MONITOR", requested, 128);
        if (n >= 128) throw std::runtime_error("ALBION_CAPTURE_MONITOR is too long");
        const HMONITOR primary = MonitorFromPoint(POINT{0,0}, MONITOR_DEFAULTTOPRIMARY);
        for (UINT ai = 0;; ++ai) {
            ComPtr<IDXGIAdapter1> adapter;
            const HRESULT ah = factory->EnumAdapters1(ai, &adapter);
            if (ah == DXGI_ERROR_NOT_FOUND) break; HR(ah, "EnumAdapters1");
            for (UINT oi = 0;; ++oi) {
                ComPtr<IDXGIOutput> output;
                const HRESULT oh = adapter->EnumOutputs(oi, &output);
                if (oh == DXGI_ERROR_NOT_FOUND) break; HR(oh, "EnumOutputs");
                DXGI_OUTPUT_DESC desc{}; HR(output->GetDesc(&desc), "Output description");
                if (!desc.AttachedToDesktop) continue;
                const bool matches = n ? CompareStringOrdinal(desc.DeviceName, -1, requested, -1, TRUE) == CSTR_EQUAL : desc.Monitor == primary;
                if (matches) { chosenAdapter = adapter; chosenOutput = output; chosen = desc; break; }
            }
            if (chosenOutput) break;
        }
        if (!chosenOutput) throw std::runtime_error("Requested/primary capture monitor is not available");
        // A rotated duplication surface needs shader/copy rotation support. Do
        // not silently produce offset/rotated results on unsupported hardware.
        if (chosen.Rotation != DXGI_MODE_ROTATION_IDENTITY && chosen.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED)
            throw std::runtime_error("Portrait/rotated capture is not supported yet; select a landscape capture monitor");
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        HRESULT hr = D3D11CreateDevice(chosenAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
            levels, _countof(levels), D3D11_SDK_VERSION, &mDev, nullptr, &mCtx);
#ifdef _DEBUG
        if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING) hr = D3D11CreateDevice(chosenAdapter.Get(), D3D_DRIVER_TYPE_UNKNOWN,
            nullptr, flags & ~D3D11_CREATE_DEVICE_DEBUG, levels, _countof(levels), D3D11_SDK_VERSION, &mDev, nullptr, &mCtx);
#endif
        HR(hr, "D3D11CreateDevice on capture adapter");
        ComPtr<ID3D10Multithread> mt; if (SUCCEEDED(mDev.As(&mt))) mt->SetMultithreadProtected(TRUE);
        HR(chosenOutput.As(&mOutput1), "QI IDXGIOutput1");
        mOutputLeft = chosen.DesktopCoordinates.left; mOutputTop = chosen.DesktopCoordinates.top;
        DXGI_ADAPTER_DESC1 adapter{}; HR(chosenAdapter->GetDesc1(&adapter), "Adapter description");
        char identity[256]{};
        std::snprintf(identity, sizeof(identity), "vendor=0x%04X device=0x%04X LUID=%08X:%08X output-origin=%ld,%ld",
            unsigned(adapter.VendorId), unsigned(adapter.DeviceId), unsigned(adapter.AdapterLuid.HighPart),
            unsigned(adapter.AdapterLuid.LowPart), chosen.DesktopCoordinates.left, chosen.DesktopCoordinates.top);
        mCaptureDeviceDetails = WideToUtf8(adapter.Description) + "; monitor=" + WideToUtf8(chosen.DeviceName) + "; " + identity;
    }

void App::EnsureCaptureTexture()
{
        const UINT width = mCaptureRegion.width;
        const UINT height = mCaptureRegion.height;
        if (!width || !height) return;
        if (mFrameTex && mFrameSRV && mFrameW == width && mFrameH == height) return;

        mFrameSRV.Reset();
        mFrameTex.Reset();

        D3D11_TEXTURE2D_DESC td{};
        td.Width = width; td.Height = height; td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc = { 1,0 };
        td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        HR(mDev->CreateTexture2D(&td, nullptr, &mFrameTex), "Create capture texture");
        HR(mDev->CreateShaderResourceView(mFrameTex.Get(), nullptr, &mFrameSRV),
            "Create capture SRV");
        mFrameW = width;
        mFrameH = height;
        mFrameNeedsFullCopy = true;
    }

void App::StartDuplication()
{
        mDesktopSrvs.Reset();
        mDupl.Reset();
        DXGI_OUTPUT_DESC output{}; HR(mOutput1->GetDesc(&output), "Current output description");
        if (!output.AttachedToDesktop ||
            (output.Rotation != DXGI_MODE_ROTATION_IDENTITY && output.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED))
            throw std::runtime_error("Capture output changed/rotated; select a landscape monitor and use F8 recovery");
        mOutputLeft = output.DesktopCoordinates.left; mOutputTop = output.DesktopCoordinates.top;
        HRESULT hr = mOutput1->DuplicateOutput(mDev.Get(), &mDupl);
        if (hr == E_ACCESSDENIED) { Sleep(50); return; }
        HR(hr, "DuplicateOutput");

        DXGI_OUTDUPL_DESC d{}; mDupl->GetDesc(&d);
        if (d.ModeDesc.Width != mDeskW || d.ModeDesc.Height != mDeskH)
        {
            mDeskW = d.ModeDesc.Width; mDeskH = d.ModeDesc.Height;
            mFrameSRV.Reset(); mFrameTex.Reset();
            mFrameW = mFrameH = 0;
        }
        // Staraya tekstura bolshe nedeystvitelna - sbrasyvaem keshirovannyy SRV.
        mDirectCapture = false;
        // A same-resolution duplication restart is still a new image history.
        // Dirty rectangles cannot repair a stale ROI without a fresh base copy.
        mFrameNeedsFullCopy = true;
        mDirectSrvSupported = true;
        mCaptureRegion = MakeCenteredRegion(mDeskW, mDeskH, mAim.Config().radius);
        EnsureCaptureTexture();
    }

UINT App::CaptureTimeoutMs() const
{
        // Never block result delivery inside a DXGI acquisition. The pipeline
        // event/high-resolution timer supplies the bounded, cancellable wait.
        return 0u;
    }

ID3D11ShaderResourceView* App::DesktopSrv(ID3D11Texture2D* src)
{
        return mDesktopSrvs.Get(mDev.Get(), src, mDirectSrvSupported);
    }

#ifdef AIM_ENABLE_WGC
bool App::CaptureWgcFrame()
{
        const double begin=NowMs();
        // Leave the newest frame in the mailbox while all processing slots are
        // occupied. The producer can replace it; no stale active frame is reused.
        if(mDetectEnabled && mDet.Ready() && !HasPreprocessCapacity()) {
            mCaptureMs.store(NowMs()-begin,std::memory_order_relaxed);return false;
        }
        auto frame=mWgc.Take();
        if(!frame){mCaptureMs.store(NowMs()-begin,std::memory_order_relaxed);return false;}
        ++mCapturedFrameId;mCapturedFrameMs=frame->receivedMs;mPresentedFrameMs=frame->presentedMs;
        auto* src=frame->texture.Get();
        auto* direct=DesktopSrv(src);
        if(direct) {
            mDirectCapture=true;
            if(mDetectEnabled && mDet.Ready() && WantsPreprocess())
                SubmitPreprocess(direct,mDeskW,mDeskH,mCaptureRegion,mCaptureRegion.left,mCaptureRegion.top);
            if(mDetectEnabled&&mLastPreprocessFrameId!=mCapturedFrameId) {
                EnsureCaptureTexture();CopyRegionFull(src);mFrameNeedsFullCopy=false;mDirectCapture=false;
            }
        } else {
            mDirectCapture=false;EnsureCaptureTexture();CopyRegionFull(src);mFrameNeedsFullCopy=false;
        }
        // The owning WGC frame remains alive until all texture operations are issued.
        mCaptureMs.store(NowMs()-begin,std::memory_order_relaxed);return true;
    }
#endif

bool App::CaptureFrame(UINT ms)
{
#ifdef AIM_ENABLE_WGC
        if(mUseWgc)return CaptureWgcFrame();
#endif
        const double captureStart = NowMs();
        auto finishCapture = [this, captureStart]() {
            mCaptureMs.store(NowMs() - captureStart, std::memory_order_relaxed);
            };
        if (!mDupl) { StartDuplication(); finishCapture(); return false; }
        ComPtr<IDXGIResource> res; DXGI_OUTDUPL_FRAME_INFO info{};
        HRESULT hr = mDupl->AcquireNextFrame(ms, &info, &res);
        const double acquiredAt=PerfNowMs();
        if (hr == DXGI_ERROR_WAIT_TIMEOUT)
        {
            mDxgiTimeouts.fetch_add(1, std::memory_order_relaxed);
            finishCapture();
            return false;
        }
        if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL)
        {
            // Aimmy posle 5 podryad sboev peresozdaet duplikaciyu - delaem tak zhe.
            if (++mCapFailures >= kMaxCaptureFailures) mCapFailures = 0;
            StartDuplication();
            finishCapture();
            return false;
        }
        mCapFailures = 0;
        HR(hr, "AcquireNextFrame");
        // Exceptions from resource/shader work must not leave an acquired
        // duplication frame locked and break all subsequent acquisitions.
        struct FrameLease {
            IDXGIOutputDuplication* duplication;
            void Release() noexcept {
                if (duplication) { duplication->ReleaseFrame(); duplication = nullptr; }
            }
            ~FrameLease() { Release(); }
        } frameLease{ mDupl.Get() };
        bool upd = false;
        if (info.LastPresentTime.QuadPart != 0)
        {
            ComPtr<ID3D11Texture2D> src;
            if (SUCCEEDED(res.As(&src)))
            {
                ++mCapturedFrameId;
                mAcquiredFrameMs=acquiredAt; mAccumulatedFrames=info.AccumulatedFrames;
                // Timestamp when this process obtained the desktop frame.
                // Display/scanout latency before acquisition is not included.
                mCapturedFrameMs = NowMs();
                static const double qpcToMs=[] {
                    LARGE_INTEGER f{};QueryPerformanceFrequency(&f);
                    return f.QuadPart>0 ? 1000.0/double(f.QuadPart) : 0.0;
                }();
                mPresentedFrameMs=double(info.LastPresentTime.QuadPart)*qpcToMs;
                ID3D11ShaderResourceView* direct = DesktopSrv(src.Get());
                if (direct)
                {
                    // Zero-copy: shader sam vyrezaet oblast FOV iz kadra.
                    // Delaem eto do ReleaseFrame - posle nego tekstura nevalidna.
                    mDirectCapture = true;
                    if (mDetectEnabled && mDet.Ready() && WantsPreprocess())
                    {
                        SubmitPreprocess(direct, mDeskW, mDeskH, mCaptureRegion,
                            mCaptureRegion.left, mCaptureRegion.top);
                    }
                    if(mDetectEnabled && mLastPreprocessFrameId!=mCapturedFrameId) {
                        EnsureCaptureTexture();CopyRegionFull(src.Get());
                        mFrameNeedsFullCopy=false;mDirectCapture=false;
                    }
                }
                else
                {
                    mDirectCapture = false;
                    EnsureCaptureTexture();
                    if (mWorkerCaptureMethod == CaptureMethod::DirectXFull || mFrameNeedsFullCopy)
                    {
                        CopyRegionFull(src.Get());
                        mFrameNeedsFullCopy = false;
                    }
                    else
                    {
                        CopyDirty(info, src.Get());
                    }
                }
                upd = true;
            }
        }
        frameLease.Release();
        finishCapture();
        return upd;
    }

void App::CopyDirty(const DXGI_OUTDUPL_FRAME_INFO& info, ID3D11Texture2D* src)
{
        UINT metaSize = info.TotalMetadataBufferSize;
        bool full = (metaSize == 0);
        static std::vector<uint8_t> meta;
        if (!full)
        {
            if (meta.size() < metaSize) meta.resize(metaSize);
            UINT mb = 0, db = 0;
            if (FAILED(mDupl->GetFrameMoveRects(metaSize,
                reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(meta.data()), &mb))) full = true;
            if (!full && mb > 0) full = true;
            if (!full)
            {
                if (FAILED(mDupl->GetFrameDirtyRects(metaSize,
                    reinterpret_cast<RECT*>(meta.data()), &db))) full = true;
                else
                {
                    const RECT* rects = reinterpret_cast<const RECT*>(meta.data());
                    for (UINT i = 0; i < db / sizeof(RECT); ++i)
                    {
                        // Kopiruem tolko peresechenie s oblastyu zakhvata.
                        const LONG cl = mCaptureRegion.left;
                        const LONG ct = mCaptureRegion.top;
                        const LONG cr = cl + LONG(mCaptureRegion.width);
                        const LONG cb = ct + LONG(mCaptureRegion.height);
                        const LONG l = std::max<LONG>(rects[i].left, cl);
                        const LONG t = std::max<LONG>(rects[i].top, ct);
                        const LONG r = std::min<LONG>(rects[i].right, cr);
                        const LONG b = std::min<LONG>(rects[i].bottom, cb);
                        if (r <= l || b <= t) continue;
                        D3D11_BOX box{ UINT(l),UINT(t),0,UINT(r),UINT(b),1 };
                        mCtx->CopySubresourceRegion(mFrameTex.Get(), 0,
                            UINT(l - cl), UINT(t - ct), 0, src, 0, &box);
                    }
                }
            }
        }
        if (full) CopyRegionFull(src);
    }

void App::CopyRegionFull(ID3D11Texture2D* src)
{
        D3D11_BOX box{ UINT(std::max<LONG>(mCaptureRegion.left, 0)),
                       UINT(std::max<LONG>(mCaptureRegion.top, 0)), 0,
                       UINT(std::min<LONG>(mCaptureRegion.left + LONG(mCaptureRegion.width), LONG(mDeskW))),
                       UINT(std::min<LONG>(mCaptureRegion.top + LONG(mCaptureRegion.height), LONG(mDeskH))), 1 };
        if (box.right > box.left && box.bottom > box.top)
            mCtx->CopySubresourceRegion(mFrameTex.Get(), 0, 0, 0, 0, src, 0, &box);
    }
