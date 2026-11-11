#pragma once
// d3d11.h and wrl/client.h must be included first.
#include <array>
#include <cstddef>

// DXGI/WGC may rotate capture surfaces. Each SRV owns its texture reference;
// the raw identity is therefore valid until that entry is reset. This does not
// own an acquired DXGI/WGC frame or extend its capture-session lease.
class DesktopSrvCache
{
public:
    void Reset() noexcept
    {
        for (auto& entry : mEntries) { entry.view.Reset(); entry.texture = nullptr; }
        mNext = 0;
    }

    ID3D11ShaderResourceView* Get(ID3D11Device* device, ID3D11Texture2D* src,
        bool& directSupported)
    {
        if (!directSupported || !device || !src) return nullptr;
        for (const auto& entry : mEntries)
            if (entry.texture == src && entry.view) return entry.view.Get();

        D3D11_TEXTURE2D_DESC desc{};
        src->GetDesc(&desc);
        if (!(desc.BindFlags & D3D11_BIND_SHADER_RESOURCE))
        {
            directSupported = false;
            return nullptr;
        }
        auto& entry = mEntries[mNext];
        entry.view.Reset(); entry.texture = nullptr;
        if (FAILED(device->CreateShaderResourceView(src, nullptr,
            entry.view.GetAddressOf())))
        {
            directSupported = false;
            return nullptr;
        }
        entry.texture = src;
        mNext = (mNext + 1) % mEntries.size();
        return entry.view.Get();
    }
private:
    struct Entry {
        ID3D11Texture2D* texture = nullptr;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    };
    std::array<Entry, 4> mEntries{};
    size_t mNext = 0;
};