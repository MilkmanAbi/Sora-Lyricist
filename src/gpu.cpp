#include "gpu.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d12.lib")

using Microsoft::WRL::ComPtr;

namespace mt {

std::vector<Gpu> list_gpus() {
    std::vector<Gpu> out;
    ComPtr<IDXGIFactory1> f;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return out;
    ComPtr<IDXGIAdapter1> a;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++, a.Reset()) {
        DXGI_ADAPTER_DESC1 d;
        if (FAILED(a->GetDesc1(&d)) || (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
        Gpu g;
        g.index = (int)i;
        char name[256];
        WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, sizeof name, nullptr, nullptr);
        g.name = name;
        g.vendor = d.VendorId;
        g.vram = d.DedicatedVideoMemory;
        // "dedicated memory" alone misleads (AMD APUs carve some out of system RAM): ask D3D12 about the memory
        // architecture. An adapter without D3D12 can't run DirectML anyway.
        ComPtr<ID3D12Device> dev;
        if (SUCCEEDED(D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) {
            D3D12_FEATURE_DATA_ARCHITECTURE arch{};
            if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &arch, sizeof arch)))
                g.discrete = !arch.UMA && g.vram >= (512ull << 20);
        }
        out.push_back(g);
    }
    return out;
}

std::optional<Gpu> discrete_gpu() {
    std::optional<Gpu> best;
    for (auto& g : list_gpus())
        if (g.discrete && (!best || g.vram > best->vram)) best = g;
    return best;
}

}  // namespace mt
