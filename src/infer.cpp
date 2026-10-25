#include "infer.h"

#include <openvino/openvino.hpp>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// onnxruntime.dll is delay-loaded (CPU / Intel-only runs never load it or DirectML): no API lookup at start-up
#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>
#include <dml_provider_factory.h>

namespace mt {

static ov::Core& core() {
    static ov::Core c;
    return c;
}

void init_openvino(const fs::path& kernel_cache) {
    fs::create_directories(kernel_cache);
    core().set_property(ov::cache_dir(utf8(kernel_cache)));
}

std::vector<std::string> openvino_devices() {
    try {  // the GPU plugin needs Intel's OpenCL driver; without it OpenVINO just has the CPU
        return core().get_available_devices();
    } catch (const std::exception&) {
        return {"CPU"};
    }
}

static fs::path g_marks;

void set_gpu_marks(const fs::path& dir) { g_marks = dir; }

static fs::path mark_file(const std::string& device, const std::string& model) {
    std::string d = device;
    for (char& c : d)
        if (c == ':') c = '_';
    return g_marks / (d + "_" + model + ".bad");
}

bool gpu_marked_bad(const std::string& device, const std::string& model) {
    return !g_marks.empty() && fs::exists(mark_file(device, model));
}

void mark_gpu_bad(const std::string& device, const std::string& model, const std::string& why) {
    if (g_marks.empty()) return;
    std::error_code ec;
    fs::create_directories(g_marks, ec);
    write_text(mark_file(device, model), why + "\n");
}

fs::path model_path(const fs::path& models, const std::string& name, const std::string& device) {
    return models / (name + (is_dml(device) ? ".onnx" : ".xml"));
}

// ------------------------------------------------------------------ ONNX Runtime + DirectML

struct Net::OrtState {
    Ort::Session session{nullptr};
    std::vector<std::string> in_names, out_names;
    std::vector<std::vector<int64_t>> in_shapes, out_shapes;
};

static Ort::Env& ort_env() {
    static Ort::Env* env = [] {
        Ort::InitApi();
        return new Ort::Env(ORT_LOGGING_LEVEL_WARNING, "mtn");
    }();
    return *env;
}

static size_t count(const std::vector<int64_t>& s) {
    size_t n = 1;
    for (auto d : s) n *= (size_t)std::max<int64_t>(d, 1);
    return n;
}

Net::Net(const fs::path& model, const std::string& device, int requests, bool gpu_fp16) {
    if (!fs::exists(model)) die("model missing: %s", utf8(model).c_str());
    Timer t;
    device_ = device;
    if (is_dml(device_)) {
        int adapter = device_.size() > 4 ? std::stoi(device_.substr(4)) : 0;
        ort_env();  // loads onnxruntime.dll and its API before anything else touches it
        Ort::SessionOptions so;
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        so.DisableMemPattern();  // required by the DirectML provider
        so.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
        Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_DML(so, adapter));
        ort_ = std::make_unique<OrtState>();
        try {
            ort_->session = Ort::Session(ort_env(), model.c_str(), so);
        } catch (const std::exception& e) {
            die("could not load %s on DirectML adapter %d: %s", utf8(model).c_str(), adapter, e.what());
        }
        Ort::AllocatorWithDefaultOptions alloc;
        auto& s = ort_->session;
        for (size_t i = 0; i < s.GetInputCount(); i++) {
            ort_->in_names.push_back(s.GetInputNameAllocated(i, alloc).get());
            ort_->in_shapes.push_back(s.GetInputTypeInfo(i).GetTensorTypeAndShapeInfo().GetShape());
            in_sizes_.push_back(count(ort_->in_shapes.back()));
        }
        for (size_t i = 0; i < s.GetOutputCount(); i++) {
            ort_->out_names.push_back(s.GetOutputNameAllocated(i, alloc).get());
            ort_->out_shapes.push_back(s.GetOutputTypeInfo(i).GetTensorTypeAndShapeInfo().GetShape());
            out_sizes_.push_back(count(ort_->out_shapes.back()));
        }
        for (int r = 0; r < requests; r++) {
            for (size_t n : in_sizes_) ort_in_.emplace_back(n);
            for (size_t n : out_sizes_) ort_out_.emplace_back(n);
        }
        pending_.resize(requests);
        log("  %s on DirectML adapter %d (ready in %.1fs)", utf8(model.stem()).c_str(), adapter, t.s());
        return;
    }

    auto m = core().read_model(utf8(model));
    if (device_ == "AUTO") {
        auto devs = openvino_devices();
        device_ = std::find(devs.begin(), devs.end(), "GPU") != devs.end() ? "GPU" : "CPU";
    }
    ov::AnyMap cfg;
    cfg[ov::hint::performance_mode.name()] = ov::hint::PerformanceMode::LATENCY;
    if (device_ == "GPU") cfg[ov::hint::inference_precision.name()] = gpu_fp16 ? ov::element::f16 : ov::element::f32;
    try {
        cm_ = std::make_unique<ov::CompiledModel>(core().compile_model(m, device_, cfg));
    } catch (const std::exception& e) {
        if (device_ == "CPU") die("could not load %s: %s", utf8(model).c_str(), e.what());
        log("  %s: GPU unavailable (%s) - using the CPU", utf8(model.stem()).c_str(), e.what());
        device_ = "CPU";
        cm_ = std::make_unique<ov::CompiledModel>(core().compile_model(m, device_));
    }
    for (int r = 0; r < requests; r++) rq_.push_back(std::make_unique<ov::InferRequest>(cm_->create_infer_request()));
    for (auto& p : cm_->inputs()) in_sizes_.push_back(ov::shape_size(p.get_shape()));
    for (auto& p : cm_->outputs()) out_sizes_.push_back(ov::shape_size(p.get_shape()));
    log("  %s on %s%s (ready in %.1fs)", utf8(model.stem()).c_str(), device_.c_str(),
        device_ == "GPU" ? (gpu_fp16 ? " fp16" : " fp32") : "", t.s());
}

Net::~Net() {
    for (auto& f : pending_)
        if (f.valid()) f.wait();
}

void Net::ort_run(int r) {
    if (getenv("MT_TEST_GPU_ZEROS")) {  // testing: what a GPU reset by Windows' watchdog returns, without a GPU run
        for (size_t o = 0; o < out_sizes_.size(); o++) std::fill(ort_out_[r * out_sizes_.size() + o].begin(), ort_out_[r * out_sizes_.size() + o].end(), 0.f);
        return;
    }
    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    const size_t ni = in_sizes_.size(), no = out_sizes_.size();
    std::vector<Ort::Value> ins, outs;
    std::vector<const char*> in_n, out_n;
    for (size_t i = 0; i < ni; i++) {
        auto& b = ort_in_[r * ni + i];
        ins.push_back(Ort::Value::CreateTensor<float>(mem, b.data(), b.size(), ort_->in_shapes[i].data(), ort_->in_shapes[i].size()));
        in_n.push_back(ort_->in_names[i].c_str());
    }
    for (size_t o = 0; o < no; o++) {
        auto& b = ort_out_[r * no + o];
        outs.push_back(Ort::Value::CreateTensor<float>(mem, b.data(), b.size(), ort_->out_shapes[o].data(), ort_->out_shapes[o].size()));
        out_n.push_back(ort_->out_names[o].c_str());
    }
    ort_->session.Run(Ort::RunOptions{nullptr}, in_n.data(), ins.data(), ni, out_n.data(), outs.data(), no);
}

float* Net::input(int i, int r) { return ort_ ? ort_in_[r * in_sizes_.size() + i].data() : rq_[r]->get_input_tensor(i).data<float>(); }
size_t Net::input_size(int i) const { return in_sizes_[i]; }
void Net::infer(int r) { ort_ ? ort_run(r) : rq_[r]->infer(); }
void Net::start(int r) {
    if (ort_) pending_[r] = std::async(std::launch::async, [this, r] { ort_run(r); });
    else rq_[r]->start_async();
}
void Net::wait(int r) {
    if (ort_) {
        if (pending_[r].valid()) pending_[r].get();  // rethrows a failed run
    } else {
        rq_[r]->wait();
    }
}
const float* Net::output(int o, int r) { return ort_ ? ort_out_[r * out_sizes_.size() + o].data() : rq_[r]->get_output_tensor(o).data<float>(); }
size_t Net::output_size(int o) const { return out_sizes_[o]; }

}  // namespace mt
