// Which GPUs this PC has, as DirectX sees them. A discrete GPU (its own memory: AMD Radeon, NVIDIA GeForce, Intel Arc)
// runs the networks through DirectML; an Intel iGPU through OpenVINO; otherwise the CPU does everything.
#pragma once
#include <optional>
#include <string>
#include <vector>

namespace mt {

struct Gpu {
    int index = -1;          // DXGI adapter index (= DirectML device id)
    std::string name;
    unsigned vendor = 0;     // 0x1002 AMD, 0x10DE NVIDIA, 0x8086 Intel
    unsigned long long vram = 0;
    bool discrete = false;   // D3D12 says it is not UMA: its own memory, not shared with the CPU
};

std::vector<Gpu> list_gpus();
std::optional<Gpu> discrete_gpu();  // the one with the most memory, if any

}  // namespace mt
