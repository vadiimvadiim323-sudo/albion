#pragma once
// Production GPU preprocessor, shared by the application and headless benchmark.
// Caller owns the immediate context. Never overwrite a CUDA-owned slot.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
using Microsoft::WRL::ComPtr;
inline void PreprocessCheck(HRESULT hr,const char* what) {
    if(FAILED(hr)){char b[32];sprintf_s(b,"0x%08X",unsigned(hr));throw std::runtime_error(std::string(what)+" failed, hr="+b);}
}
struct LetterboxInfo
{
    float ratio = 1, padX = 0, padY = 0, offsetX = 0, offsetY = 0;
    int net = 640;
    std::uint64_t frameId = 0;
    double capturedMs = 0.0, submittedMs = 0.0;
    double presentedMs = 0.0, acquiredMs = 0.0;
    std::uint32_t accumulatedFrames=0;
    float roiWidth=0, roiHeight=0;
};

static const char* kPreHLSL = R"(
Texture2D<float4> gSrc : register(t0);
SamplerState      gSmp : register(s0);
RWBuffer<float>   gDst : register(u0);
cbuffer Params : register(b0) {
    float2 gDesktopSize; float2 gRegionSize;
    float2 gRegionOffset; float2 gPad;
    float gRatio; uint gNet; float2 _pad;
};
[numthreads(8,8,1)]
void CSMain(uint3 tid : SV_DispatchThreadID) {
    if (tid.x >= gNet || tid.y >= gNet) return;
    float2 local = (float2(tid.xy) + 0.5 - gPad) / gRatio;
    float3 rgb = 114.0/255.0;
    if (all(local >= 0.0) && all(local < gRegionSize)) {
        float2 dp = gRegionOffset + local;
        rgb = gSrc.SampleLevel(gSmp, dp / gDesktopSize, 0).rgb;
    }
    uint plane = gNet*gNet, idx = tid.y*gNet + tid.x;
    gDst[idx] = rgb.r; gDst[plane+idx] = rgb.g; gDst[2*plane+idx] = rgb.b;
}
)";

static ComPtr<ID3DBlob> CompileSrc(const char* src, const char* entry, const char* target)
{
    ComPtr<ID3DBlob> code, err;
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr,
        entry, target,
        D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS,
        0, &code, &err);
    if (FAILED(hr))
        throw std::runtime_error(err
            ? static_cast<const char*>(err->GetBufferPointer()) : "D3DCompile failed");
    return code;
}

class GpuPreprocessor
{
public:
    void Init(ID3D11Device* dev, bool profile = false)
    {
        // Normal mode samples 1/16 preprocesses instead of instrumenting every
        // dispatch. Developer profiling retains full per-frame GPU timings.
        mTimingStride = profile ? 1u : 16u;
        mTimingCountdown = 0;
        auto cs = CompileSrc(kPreHLSL, "CSMain", "cs_5_0");
        PreprocessCheck(dev->CreateComputeShader(cs->GetBufferPointer(), cs->GetBufferSize(), nullptr, &mCS), "CreateCS");

        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = UINT(size_t(640)*640*3 * sizeof(float));
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        for (auto& tensor : mTensor)
            PreprocessCheck(dev->CreateBuffer(&bd, nullptr, &tensor), "tensor buf");

        D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32_FLOAT;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = UINT(size_t(640)*640*3);
        for (size_t i = 0; i < mTensor.size(); ++i)
            PreprocessCheck(dev->CreateUnorderedAccessView(mTensor[i].Get(), &ud, &mUAV[i]), "tensor UAV");

        bd.Usage = D3D11_USAGE_STAGING; bd.BindFlags = 0;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (auto& s : mStaging) PreprocessCheck(dev->CreateBuffer(&bd, nullptr, &s), "staging");

        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth = 48; cb.Usage = D3D11_USAGE_DYNAMIC;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        PreprocessCheck(dev->CreateBuffer(&cb, nullptr, &mCB), "cbuffer");

        D3D11_QUERY_DESC qd{ D3D11_QUERY_EVENT, 0 };
        for (auto& q : mFence) PreprocessCheck(dev->CreateQuery(&qd, &q), "query");
        for (auto& q : mSharedWriteFence) PreprocessCheck(dev->CreateQuery(&qd, &q), "shared write query");
        for (auto& timing : mGpuTimes)
        {
            D3D11_QUERY_DESC disjoint{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
            D3D11_QUERY_DESC timestamp{ D3D11_QUERY_TIMESTAMP, 0 };
            timing.valid = SUCCEEDED(dev->CreateQuery(&disjoint, &timing.disjoint)) &&
                SUCCEEDED(dev->CreateQuery(&timestamp, &timing.start)) &&
                SUCCEEDED(dev->CreateQuery(&timestamp, &timing.end));
        }

        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        PreprocessCheck(dev->CreateSamplerState(&sd, &mSmp), "sampler");
    }

    // Bufer s gotovym tenzorom. Nuzhen backendu dlya zero-copy vkhoda.
    ID3D11Buffer* TensorBuffer(size_t slot = 0) const { return mTensor[slot].Get(); }

    // Producer-thread only. Prevents repeated writes from building up a GPU
    // queue when the pending slot's previous preprocess has not completed.
    bool CanWriteSharedSlot(ID3D11DeviceContext* ctx, size_t slot)
    {
        if (slot >= mTensor.size()) return false;
        if (!mSharedWritePending[slot]) return true;
        BOOL done = FALSE;
        const HRESULT query = ctx->GetData(mSharedWriteFence[slot].Get(), &done, sizeof(done),
            D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (FAILED(query)) mSharedWritePending[slot] = false;
        PreprocessCheck(query, "Shared preprocess fence");
        if (query != S_OK || !done) return false;
        mSharedWritePending[slot] = false;
        return true;
    }

    void DiscardCpuFrames() { mPending.fill(false); }


    // V zero-copy rezhime readback na CPU ne delaetsya voobshche.
    void SetSharedMode(bool on) { mShared = on; }

    // Vozvrashchaet letterbox otpravlennogo kadra; net == 0 znachit "ne otpravlen".
    template<class Region>
    LetterboxInfo Submit(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* srv,
        UINT dw, UINT dh, const Region& r,
        LONG outputLeft, LONG outputTop,
        std::uint64_t frameId, double capturedMs, double submittedMs, double presentedMs, size_t slot, int net, double acquiredMs=0, std::uint32_t accumulatedFrames=0)
    {
        // net == 0 - priznak "kadr ne otpravlen" (po umolchaniyu tam kNetMax).
        if (!dw || !dh || !r.width || !r.height || slot >= mTensor.size())
        {
            LetterboxInfo none{}; none.net = 0; return none;
        }
        if(net<64 || net>640) throw std::runtime_error("Invalid preprocess side");
        float ratio = std::min(float(net) / r.width, float(net) / r.height);
        LetterboxInfo lb{ ratio,
            (net - r.width * ratio) * .5f,
            (net - r.height * ratio) * .5f,
            float(outputLeft), float(outputTop), net };
        lb.frameId = frameId; lb.capturedMs = capturedMs; lb.submittedMs = submittedMs; lb.presentedMs=presentedMs;
        lb.acquiredMs=acquiredMs; lb.accumulatedFrames=accumulatedFrames;
        lb.roiWidth=float(r.width); lb.roiHeight=float(r.height);
        PollGpuTiming(ctx);

        D3D11_MAPPED_SUBRESOURCE m{};
        PreprocessCheck(ctx->Map(mCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m), "Map CB");
        float* p = static_cast<float*>(m.pData);
        p[0] = float(dw); p[1] = float(dh); p[2] = float(r.width); p[3] = float(r.height);
        p[4] = float(r.left); p[5] = float(r.top); p[6] = lb.padX; p[7] = lb.padY;
        p[8] = ratio; reinterpret_cast<UINT*>(p)[9] = UINT(net); p[10] = p[11] = 0;
        ctx->Unmap(mCB.Get(), 0);

        auto* uav = mUAV[slot].Get(); auto* cb = mCB.Get(); auto* smp = mSmp.Get();
        ctx->CSSetShader(mCS.Get(), nullptr, 0);
        ctx->CSSetShaderResources(0, 1, &srv);
        ctx->CSSetSamplers(0, 1, &smp);
        ctx->CSSetConstantBuffers(0, 1, &cb);
        ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        int timingSlot = -1;
        const bool sampleTiming = mTimingCountdown == 0;
        mTimingCountdown = sampleTiming ? mTimingStride - 1 : mTimingCountdown - 1;
        if (sampleTiming) for (size_t i = 0; i < mGpuTimes.size(); ++i)
        {
            auto& t = mGpuTimes[i];
            if (t.valid && !t.pending)
            {
                timingSlot = int(i); t.sequence = ++mGpuSequence;
                ctx->Begin(t.disjoint.Get()); ctx->End(t.start.Get());
                break;
            }
        }
        ctx->Dispatch((net + 7) / 8, (net + 7) / 8, 1);
        if (timingSlot >= 0)
        {
            auto& t = mGpuTimes[size_t(timingSlot)];
            ctx->End(t.end.Get()); ctx->End(t.disjoint.Get()); t.pending = true;
        }

        ID3D11UnorderedAccessView* nu = nullptr; ID3D11ShaderResourceView* ns = nullptr;
        ctx->CSSetUnorderedAccessViews(0, 1, &nu, nullptr);
        ctx->CSSetShaderResources(0, 1, &ns);
        ctx->CSSetShader(nullptr, nullptr, 0);

        if (mShared)
        {
            // Tenzor ostayotsya na GPU - ego prochtet sam backend. Flush nuzhen,
            // chtoby drayver ne otkladyval dispatch do sleduyushchego kadra.
            ctx->End(mSharedWriteFence[slot].Get());
            mSharedWritePending[slot] = true;
            ctx->Flush();
            return lb;
        }

        // Kopiruem tolko tu chast tenzora, kotoruyu realno zanyala model:
        // pri vkhode 320x320 eto vchetvero menshe, chem ves bufer pod 640x640.
        const UINT usedBytes = UINT(TensorCount(net) * sizeof(float));
        const D3D11_BOX usedRange{ 0, 0, 0, usedBytes, 1, 1 };
        ctx->CopySubresourceRegion(mStaging[mWrite].Get(), 0, 0, 0, 0,
            mTensor[slot].Get(), 0, &usedRange);
        ctx->End(mFence[mWrite].Get());
        mPending[mWrite] = true; mLb[mWrite] = lb; mWrite ^= 1;
        // No Present() drives this compute-only context. Submit explicitly so
        // DONOTFLUSH polling cannot leave this readback queued indefinitely.
        ctx->Flush();
        return lb;
    }

    bool TryFetch(ID3D11DeviceContext* ctx, float* dst, LetterboxInfo& lb)
    {
        int i = mWrite ^ 1;
        if (!mPending[i]) return false;
        BOOL done = FALSE;
        const HRESULT query = ctx->GetData(mFence[i].Get(), &done, sizeof(done),
            D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (FAILED(query)) { mPending[i] = false; PreprocessCheck(query, "Readback fence"); }
        if (query != S_OK || !done) return false;
        D3D11_MAPPED_SUBRESOURCE m{};
        const HRESULT mapped = ctx->Map(mStaging[i].Get(), 0, D3D11_MAP_READ,
            D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (mapped == DXGI_ERROR_WAS_STILL_DRAWING) return false;
        if (FAILED(mapped)) { mPending[i] = false; PreprocessCheck(mapped, "Readback Map"); }
        memcpy(dst, m.pData, TensorCount(mLb[i].net) * sizeof(float));
        ctx->Unmap(mStaging[i].Get(), 0);
        mPending[i] = false; lb = mLb[i]; return true;
    }

    bool HasPending() const { return mPending[0] || mPending[1]; }

    double GpuMs() const { return mLastGpuMs; } // negative means not measured yet
    void PollGpuTiming(ID3D11DeviceContext* ctx)
    {
        for (auto& t : mGpuTimes)
        {
            if (!t.pending) continue;
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};
            UINT64 begin = 0, end = 0;
            const UINT flags = D3D11_ASYNC_GETDATA_DONOTFLUSH;
            const HRESULT hd = ctx->GetData(t.disjoint.Get(), &d, sizeof(d), flags);
            if (FAILED(hd)) { t.pending = false; t.valid = false; continue; }
            if (hd != S_OK) continue;
            const HRESULT hb = ctx->GetData(t.start.Get(), &begin, sizeof(begin), flags);
            const HRESULT he = ctx->GetData(t.end.Get(), &end, sizeof(end), flags);
            if (FAILED(hb) || FAILED(he)) { t.pending = false; t.valid = false; continue; }
            if (hb != S_OK || he != S_OK) continue;
            if (!d.Disjoint && d.Frequency && end >= begin && t.sequence > mLastGpuSequence)
            {
                mLastGpuMs = double(end - begin) * 1000.0 / double(d.Frequency);
                mLastGpuSequence = t.sequence;
            }
            t.pending = false;
        }
    }

private:
    static size_t TensorCount(int side) noexcept { return size_t(side)*side*3; }
    ComPtr<ID3D11ComputeShader>         mCS;
    std::array<ComPtr<ID3D11Buffer>, 2> mTensor;
    ComPtr<ID3D11Buffer>                mCB;
    ComPtr<ID3D11SamplerState>          mSmp;
    std::array<ComPtr<ID3D11UnorderedAccessView>, 2> mUAV;
    std::array<ComPtr<ID3D11Query>, 2> mSharedWriteFence;
    std::array<bool, 2> mSharedWritePending{ false, false };
    std::array<ComPtr<ID3D11Buffer>, 2>  mStaging;
    std::array<ComPtr<ID3D11Query>, 2>   mFence;
    std::array<bool, 2>                  mPending{ false,false };
    std::array<LetterboxInfo, 2>         mLb{};
    int                                 mWrite = 0;
    bool                                mShared = false;
    struct GpuTimingSlot
    {
        ComPtr<ID3D11Query> disjoint, start, end;
        bool valid = false, pending = false;
        std::uint64_t sequence = 0;
    };
    std::array<GpuTimingSlot, 4> mGpuTimes;
    unsigned mTimingStride = 16, mTimingCountdown = 0;
    std::uint64_t mGpuSequence = 0, mLastGpuSequence = 0;
    double mLastGpuMs = -1.0;
};

