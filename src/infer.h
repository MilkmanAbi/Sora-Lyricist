// Networks with fixed input shapes, on one of two backends behind the same interface:
//   OpenVINO       "CPU", "GPU" (an Intel iGPU/Arc), "AUTO" (GPU when there is one)    model: NAME.xml
//   ONNX Runtime   "DML" / "DML:<n>": DirectML on any DirectX 12 GPU - AMD, NVIDIA,   model: NAME.onnx
//                  Intel (n = DXGI adapter index; see gpu.h for how the discrete one is found)
// OpenVINO keeps compiled GPU kernels in the cache folder, so only the very first run pays the minutes-long compile.
#pragma once
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "common.h"

namespace ov {
class CompiledModel;
class InferRequest;
}  // namespace ov

namespace mt {

void init_openvino(const fs::path& kernel_cache);
std::vector<std::string> openvino_devices();
inline bool is_dml(const std::string& device) { return device.rfind("DML", 0) == 0; }
// A GPU that failed a model's self-check (wrong numbers, or too slow for Windows' 2 s GPU watchdog, which resets it -
// and five resets in a minute crash Windows) is remembered per adapter + model and not tried again.
void set_gpu_marks(const fs::path& dir);
bool gpu_marked_bad(const std::string& device, const std::string& model);
void mark_gpu_bad(const std::string& device, const std::string& model, const std::string& why);
// the model file a device runs: NAME.onnx for DirectML, NAME.xml (OpenVINO IR) otherwise
fs::path model_path(const fs::path& models, const std::string& name, const std::string& device);

class Net {
public:
    // gpu_fp16: run in fp16 on an OpenVINO GPU (2x faster) - only for networks verified not to overflow in it.
    // (DirectML runs the precision the .onnx file was exported in.)
    Net(const fs::path& model, const std::string& device, int requests = 1, bool gpu_fp16 = true);
    ~Net();
    const std::string& device() const { return device_; }
    // fill input i of request r, run, read output o (sizes are the model's fixed shapes)
    float* input(int i, int r = 0);
    size_t input_size(int i) const;
    void infer(int r = 0);
    void start(int r);  // async: start / wait, so the CPU prepares the next chunk while the GPU runs this one
    void wait(int r);
    const float* output(int o, int r = 0);
    size_t output_size(int o) const;

private:
    std::string device_;
    std::vector<size_t> in_sizes_, out_sizes_;
    // OpenVINO
    std::unique_ptr<ov::CompiledModel> cm_;
    std::vector<std::unique_ptr<ov::InferRequest>> rq_;
    // ONNX Runtime (DirectML)
    struct OrtState;
    std::unique_ptr<OrtState> ort_;
    std::vector<std::vector<float>> ort_in_, ort_out_;  // [request * n + i]
    std::vector<std::future<void>> pending_;
    void ort_run(int r);
};

}  // namespace mt
