#include "NeuralPbr.hpp"

#include "NeuralNet.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>

namespace eruption {

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

// O que o motor precisa saber da rede pra usar a saida dela.
struct Contract {
    std::string inTensor, outTensor;
    float inputScale = 1.0f / 255.0f;
    float inputBias = 0.0f;
    int padMultiple = 1;
    bool normalYUp = true;
    int normalX = -1, normalY = -1, normalZ = -1;
    int ao = -1, roughness = -1, metallic = -1, height = -1;
};

struct LoadedModel {
    std::string path;
    Contract contract;
    std::shared_ptr<NeuralNet> net;
    uint32_t key = 0;
};

std::string configPath() {
    const char* e = std::getenv("ERUPTION_NEURAL_PBR_CONFIG");
    return (e && *e) ? e : "data/launcher_config.json";
}

bool disabledByEnv() {
    const char* e = std::getenv("ERUPTION_NEURAL_PBR");
    return e && std::string(e) == "0";
}

fs::file_time_type modificationTime(const std::string& path) {
    std::error_code ec;
    const fs::file_time_type t = fs::last_write_time(path, ec);
    return ec ? fs::file_time_type{} : t;
}

uint32_t fnv1a(uint32_t seed, const std::string& bytes) {
    uint32_t h = seed;
    for (char c : bytes) {
        h ^= static_cast<uint8_t>(c);
        h *= 16777619u;
    }
    return h;
}

// Bloco "neural_pbr" da config, sempre em forma de objeto. Vazio = sem rede.
Json readConfigBlock(const std::string& cfgFile) {
    std::ifstream f(cfgFile);
    if (!f.is_open()) return {};
    const Json j = Json::parse(f, nullptr, false);
    if (j.is_discarded() || !j.contains("neural_pbr")) return {};
    Json block = j["neural_pbr"];
    if (block.is_string()) block = Json{{"model", block.get<std::string>()}};
    if (!block.is_object() || !block.contains("model") || !block["model"].is_string()) return {};
    return block;
}

// Campo do contrato: override na config > metadado "eruption.pbr.<nome>" do
// .onnx > nulo (o chamador aplica o padrao).
Json contractField(const Json& overrides, const NeuralNet& net, const std::string& name) {
    if (overrides.contains(name)) return overrides[name];
    const std::string md = net.metadata("eruption.pbr." + name);
    if (md.empty()) return nullptr;
    const Json parsed = Json::parse(md, nullptr, false);
    return parsed.is_discarded() ? Json(md) : parsed;
}

std::string fieldString(const Json& v, const std::string& def) { return v.is_string() ? v.get<std::string>() : def; }
double fieldNumber(const Json& v, double def) { return v.is_number() ? v.get<double>() : def; }
bool fieldBool(const Json& v, bool def) {
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number()) return v.get<double>() != 0.0;
    return def;
}

bool readContract(const Json& overrides, const NeuralNet& net, Contract& c) {
    const std::vector<std::string> inputs = net.inputNames(), outputs = net.outputNames();
    c.inTensor = fieldString(contractField(overrides, net, "input_tensor"), inputs.empty() ? "" : inputs[0]);
    c.outTensor = fieldString(contractField(overrides, net, "output_tensor"), outputs.empty() ? "" : outputs[0]);
    c.inputScale = static_cast<float>(fieldNumber(contractField(overrides, net, "input_scale"), c.inputScale));
    c.inputBias = static_cast<float>(fieldNumber(contractField(overrides, net, "input_bias"), c.inputBias));
    c.padMultiple = std::max(1, static_cast<int>(fieldNumber(contractField(overrides, net, "pad_multiple"), 1)));
    c.normalYUp = fieldBool(contractField(overrides, net, "normal_y_up"), true);

    const Json ch = contractField(overrides, net, "channels");
    if (!ch.is_object() || c.inTensor.empty() || c.outTensor.empty()) return false;
    c.normalX = ch.value("normal_x", -1);
    c.normalY = ch.value("normal_y", -1);
    c.normalZ = ch.value("normal_z", -1);
    c.ao = ch.value("ao", -1);
    c.roughness = ch.value("roughness", -1);
    c.metallic = ch.value("metallic", -1);
    c.height = ch.value("height", -1);
    return true;
}

// Muda quando o modelo OU o contrato efetivo muda (invalida o cache).
uint32_t contractKey(const NeuralNet& net, const Contract& c) {
    const std::string desc = c.inTensor + "|" + c.outTensor + "|" + std::to_string(c.inputScale) + "|" +
                             std::to_string(c.inputBias) + "|" + std::to_string(c.padMultiple) + "|" +
                             std::to_string(c.normalYUp) + "|" + std::to_string(c.normalX) + "," +
                             std::to_string(c.normalY) + "," + std::to_string(c.normalZ) + "," +
                             std::to_string(c.ao) + "," + std::to_string(c.roughness) + "," +
                             std::to_string(c.metallic) + "," + std::to_string(c.height);
    const uint32_t h = fnv1a(net.key(), desc);
    return h > 1u ? h : 2u; // 0 e 1 sao "sem sintese" e "sintese classica" no cache
}

NeuralNetOptions optionsFromEnv() {
    NeuralNetOptions opts;
    if (const char* e = std::getenv("ERUPTION_NEURAL_PBR_THREADS")) opts.threadsPerRun = std::atoi(e);
    if (const char* e = std::getenv("ERUPTION_NEURAL_PBR_JOBS")) opts.maxConcurrent = std::atoi(e);
    return opts;
}

std::shared_ptr<const LoadedModel> loadFromConfig(const std::string& cfgFile) {
    const Json block = readConfigBlock(cfgFile);
    if (block.empty()) return nullptr;
    auto m = std::make_shared<LoadedModel>();
    m->path = block["model"].get<std::string>();
    m->net = NeuralNet::load(m->path, optionsFromEnv());
    if (!m->net) {
        std::printf("[NEURAL-PBR] modelo %s indisponivel - sintese classica\n", m->path.c_str());
        return nullptr;
    }
    if (!readContract(block, *m->net, m->contract)) {
        std::printf("[NEURAL-PBR] %s nao descreve os canais (metadado eruption.pbr.channels) - sintese classica\n",
                    m->path.c_str());
        return nullptr;
    }
    m->key = contractKey(*m->net, m->contract);
    std::printf("[NEURAL-PBR] modelo %s carregado (chave %08x)\n", m->path.c_str(), m->key);
    return m;
}

// Modelo atual, relendo config e .onnx quando mudam no disco. Checa no
// maximo uma vez por segundo pra nao fazer stat a cada textura.
class ModelSlot {
public:
    std::shared_ptr<const LoadedModel> get() {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto now = std::chrono::steady_clock::now();
        if (m_initialized && now - m_lastCheck < std::chrono::seconds(1)) return m_model;
        m_lastCheck = now;
        if (!m_initialized || filesChanged()) reload();
        return m_model;
    }

private:
    bool filesChanged() const {
        if (modificationTime(configPath()) != m_configTime) return true;
        return m_model && modificationTime(m_model->path) != m_modelTime;
    }

    void reload() {
        const bool isReload = m_initialized;
        m_initialized = true;
        m_configTime = modificationTime(configPath());
        m_model = loadFromConfig(configPath());
        m_modelTime = m_model ? modificationTime(m_model->path) : fs::file_time_type{};
        if (isReload) std::printf("[NEURAL-PBR] recarregado: %s\n", m_model ? "rede ativa" : "sintese classica");
    }

    std::mutex m_mutex;
    std::shared_ptr<const LoadedModel> m_model;
    fs::file_time_type m_configTime{}, m_modelTime{};
    std::chrono::steady_clock::time_point m_lastCheck{};
    bool m_initialized = false;
};

std::shared_ptr<const LoadedModel> currentModel() {
    if (disabledByEnv()) return nullptr;
    static ModelSlot slot;
    return slot.get();
}

int roundUp(int v, int multiple) { return (v + multiple - 1) / multiple * multiple; }

// RGBA8 -> NCHW float RGB, estendido circularmente ate' o multiplo pedido
// (a textura e' periodica: a borda continua do outro lado).
NnTensor makeInputTensor(const uint8_t* rgba, int width, int height, const Contract& c) {
    const int W = roundUp(width, c.padMultiple), H = roundUp(height, c.padMultiple);
    const size_t plane = static_cast<size_t>(W) * H;
    NnTensor t;
    t.shape = {1, 3, H, W};
    t.data.resize(plane * 3);
    for (int y = 0; y < H; ++y) {
        const uint8_t* row = rgba + static_cast<size_t>(y % height) * width * 4;
        for (int x = 0; x < W; ++x) {
            const uint8_t* px = row + static_cast<size_t>(x % width) * 4;
            const size_t i = static_cast<size_t>(y) * W + x;
            for (int k = 0; k < 3; ++k) t.data[k * plane + i] = px[k] * c.inputScale + c.inputBias;
        }
    }
    return t;
}

// Copia um canal da saida (recortando o padding). Indice fora = vetor vazio.
void copyChannel(const NnTensor& out, int channel, int width, int height, std::vector<float>& dst) {
    dst.clear();
    if (channel < 0 || channel >= out.shape[1]) return;
    const size_t W = static_cast<size_t>(out.shape[3]);
    const float* src = out.data.data() + static_cast<size_t>(channel) * W * static_cast<size_t>(out.shape[2]);
    dst.resize(static_cast<size_t>(width) * height);
    for (int y = 0; y < height; ++y)
        std::copy_n(src + y * W, width, dst.data() + static_cast<size_t>(y) * width);
}

void interleaveNormal(const std::vector<float>& x, const std::vector<float>& y, const std::vector<float>& z,
                      bool yUp, std::vector<float>& dst) {
    dst.clear();
    if (x.empty() || y.empty() || z.empty()) return;
    const float ySign = yUp ? 1.0f : -1.0f;
    dst.resize(x.size() * 3);
    for (size_t i = 0; i < x.size(); ++i) {
        dst[i * 3 + 0] = x[i];
        dst[i * 3 + 1] = y[i] * ySign;
        dst[i * 3 + 2] = z[i];
    }
}

void subtractMean(std::vector<float>& v) {
    if (v.empty()) return;
    double sum = 0.0;
    for (float x : v) sum += x;
    const float mean = static_cast<float>(sum / static_cast<double>(v.size()));
    for (float& x : v) x -= mean;
}

std::atomic<uint64_t> g_gpuRuns{0}, g_cpuRuns{0};

void countBackend(NnBackend b) {
    if (b == NnBackend::Gpu) ++g_gpuRuns;
    else if (b == NnBackend::Cpu) ++g_cpuRuns;
}

bool outputMatches(const NnTensor& out, const NnTensor& in) {
    return out.shape.size() == 4 && out.shape[2] == in.shape[2] && out.shape[3] == in.shape[3];
}

} // namespace

bool neuralPbrAvailable() { return currentModel() != nullptr; }

NeuralPbrStats neuralPbrStats() { return {g_gpuRuns.load(), g_cpuRuns.load()}; }

uint32_t neuralPbrKey() {
    const auto m = currentModel();
    return m ? m->key : 0u;
}

bool neuralPbrInfer(const uint8_t* rgba, int width, int height, NeuralPbrMaps& out) {
    const auto m = currentModel();
    if (!m || !rgba || width < 4 || height < 4) return false;
    const Contract& c = m->contract;

    const NnTensor input = makeInputTensor(rgba, width, height, c);
    std::vector<NnTensor> outputs;
    const NnBackend backend = m->net->run({c.inTensor}, {&input}, {c.outTensor}, outputs);
    if (backend == NnBackend::None || outputs.empty()) return false;
    countBackend(backend);
    const NnTensor& result = outputs[0];
    if (!outputMatches(result, input)) {
        std::printf("[NEURAL-PBR] saida com forma inesperada pra %dx%d\n", width, height);
        return false;
    }

    out.width = width;
    out.height = height;
    std::vector<float> nx, ny, nz;
    copyChannel(result, c.normalX, width, height, nx);
    copyChannel(result, c.normalY, width, height, ny);
    copyChannel(result, c.normalZ, width, height, nz);
    interleaveNormal(nx, ny, nz, c.normalYUp, out.normal);
    copyChannel(result, c.ao, width, height, out.ao);
    copyChannel(result, c.roughness, width, height, out.roughness);
    copyChannel(result, c.metallic, width, height, out.metallic);
    copyChannel(result, c.height, width, height, out.heightMap);
    subtractMean(out.heightMap);
    return true;
}

} // namespace eruption
