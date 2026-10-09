#include "NnVulkan.hpp"

#include "NnProgram.hpp"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>

namespace eruption {

namespace {

// ---- push constants (espelham os blocos dos shaders/ml/*.comp) -------------

struct CopyParams {
    uint32_t dims[6];
    int32_t srcStrides[6];
    int32_t dstStrides[6];
    uint32_t srcBase, dstBase, total;
};

struct BinaryParams {
    uint32_t dims[6];
    uint32_t stridesA[6];
    uint32_t stridesB[6];
    uint32_t baseA, baseB, baseOut, total, code;
};

struct UnaryParams {
    uint32_t baseIn, baseOut, total, code;
    float param0, param1;
};

struct GatherParams {
    uint32_t baseSrc, baseIdx, baseOut, outer, count, inner, axisSize, total;
};

struct ReduceParams {
    uint32_t dims[6];
    uint32_t strides[6];
    uint32_t mask, baseIn, baseOut, outCount, reduceCount, code;
};

struct PoolParams {
    uint32_t baseIn, baseOut, planes;
    int32_t height, width, outHeight, outWidth, kernelH, kernelW, strideH, strideW, padTop, padLeft;
    uint32_t countIncludePad, isMax, total;
};

struct ResizeParams {
    uint32_t baseIn, baseOut;
    int32_t height, width, outHeight, outWidth;
    uint32_t linear, coordMode, nearestMode;
    float scaleH, scaleW;
    uint32_t total;
};

struct ConvParams {
    uint32_t baseX, baseW, baseBias, baseOut;
    int32_t batch, channels, height, width, outChannels, channelsPerGroup, kernelH, kernelW, outHeight, outWidth;
    int32_t padTop, padLeft, strideH, strideW, dilationH, dilationW, groups;
    uint32_t hasBias, total;
};

struct MatMulParams {
    uint32_t baseA, baseB, baseOut, rows, cols, inner;
    uint32_t batchDims[4], stridesA[4], stridesB[4];
};

static_assert(sizeof(ConvParams) <= 128 && sizeof(BinaryParams) <= 128, "push constants passam do minimo garantido");

constexpr uint32_t kGroupSize = 256;
constexpr uint32_t kConvBlockPixels = 64;   // BLOCK_N em nn_conv2d.comp
constexpr uint32_t kMatMulTile = 64;  // BLOCK em nn_matmul.comp
constexpr uint32_t kMaxGroupsPerAxis = 65535;
// Orcamento de memoria de GPU dos programas em cache (um por forma de
// entrada); o menos usado sai primeiro. ERUPTION_NN_GPU_MEM_MB muda.
constexpr int kDefaultProgramBudgetMB = 512;
// Variantes da convolucao: THREAD_M = 1, 2, 4 -> blocos de 16, 32, 64 canais.
constexpr uint32_t kConvVariants = 3;
constexpr uint32_t kConvChannelsPerThreadRow = 16;

enum class Kernel { Copy, Binary, Unary, Conv2d, MatMul, Gather, Reduce, Pool2d, Resize, Count };

const char* kernelFile(Kernel k) {
    switch (k) {
    case Kernel::Copy: return "nn_copy";
    case Kernel::Binary: return "nn_binary";
    case Kernel::Unary: return "nn_unary";
    case Kernel::Conv2d: return "nn_conv2d";
    case Kernel::MatMul: return "nn_matmul";
    case Kernel::Gather: return "nn_gather";
    case Kernel::Reduce: return "nn_reduce";
    case Kernel::Pool2d: return "nn_pool2d";
    case Kernel::Resize: return "nn_resize";
    case Kernel::Count: break;
    }
    return "";
}

Kernel kernelFor(NnOpKind kind) {
    switch (kind) {
    case NnOpKind::Copy: return Kernel::Copy;
    case NnOpKind::Binary: return Kernel::Binary;
    case NnOpKind::Unary: return Kernel::Unary;
    case NnOpKind::Conv2d: return Kernel::Conv2d;
    case NnOpKind::MatMul: return Kernel::MatMul;
    case NnOpKind::Gather: return Kernel::Gather;
    case NnOpKind::Reduce: return Kernel::Reduce;
    case NnOpKind::Pool2d: return Kernel::Pool2d;
    case NnOpKind::Resize: return Kernel::Resize;
    }
    return Kernel::Copy;
}

bool check(VkResult r, const char* what, std::string& error) {
    if (r == VK_SUCCESS) return true;
    error = std::string(what) + " falhou (VkResult " + std::to_string(static_cast<int>(r)) + ")";
    return false;
}

int envInt(const char* name, int def) {
    const char* e = std::getenv(name);
    return (e && *e) ? std::atoi(e) : def;
}

std::vector<char> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return {};
    return std::vector<char>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// SPIR-V compilado pelo build ao lado do fonte (shaders/ml/*.comp.spv), na
// pasta atual ou ao lado do executavel.
std::vector<char> loadSpirv(const std::string& name) {
    namespace fs = std::filesystem;
    const std::string rel = "shaders/ml/" + name + ".comp.spv";
    std::vector<char> code = readFile(rel);
    if (!code.empty()) return code;
    std::error_code ec;
    const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) code = readFile((exe.parent_path() / rel).string());
    return code;
}

// ---- contexto Vulkan de compute (um por processo) ---------------------------

class ComputeContext {
public:
    static std::shared_ptr<ComputeContext> acquire(std::string& error) {
        static std::mutex mtx;
        static std::weak_ptr<ComputeContext> shared;
        std::lock_guard<std::mutex> lock(mtx);
        std::shared_ptr<ComputeContext> ctx = shared.lock();
        if (ctx) return ctx;
        ctx.reset(new ComputeContext());
        if (!ctx->init(error)) return nullptr;
        shared = ctx;
        return ctx;
    }

    ~ComputeContext() {
        if (m_device) {
            vkDeviceWaitIdle(m_device);
            for (VkPipeline p : m_pipelines)
                if (p) vkDestroyPipeline(m_device, p, nullptr);
            if (m_pipelineLayout) vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
            if (m_setLayout) vkDestroyDescriptorSetLayout(m_device, m_setLayout, nullptr);
            if (m_commandPool) vkDestroyCommandPool(m_device, m_commandPool, nullptr);
            vkDestroyDevice(m_device, nullptr);
        }
        if (m_instance) vkDestroyInstance(m_instance, nullptr);
    }

    VkDevice device() const { return m_device; }
    VkPipeline pipeline(Kernel k, uint32_t variant) const {
        return m_pipelines[static_cast<size_t>(k) * kConvVariants + variant];
    }
    VkPipelineLayout pipelineLayout() const { return m_pipelineLayout; }
    VkDescriptorSetLayout setLayout() const { return m_setLayout; }
    VkCommandPool commandPool() const { return m_commandPool; }
    const std::string& name() const { return m_name; }
    const VkPhysicalDeviceLimits& limits() const { return m_limits; }
    bool hasTimestamps() const { return m_timestampBits > 0; }
    std::mutex& mutex() { return m_mutex; }

    bool findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags want, uint32_t& out) const {
        for (uint32_t i = 0; i < m_memory.memoryTypeCount; ++i)
            if ((typeBits & (1u << i)) && (m_memory.memoryTypes[i].propertyFlags & want) == want) {
                out = i;
                return true;
            }
        return false;
    }

    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, VkBuffer& buffer,
                      VkDeviceMemory& memory, std::string& error) const {
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = size;
        bi.usage = usage;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (!check(vkCreateBuffer(m_device, &bi, nullptr, &buffer), "vkCreateBuffer", error)) return false;
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(m_device, buffer, &req);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        if (!findMemoryType(req.memoryTypeBits, props, ai.memoryTypeIndex)) {
            error = "sem tipo de memoria compativel";
            return false;
        }
        if (!check(vkAllocateMemory(m_device, &ai, nullptr, &memory), "vkAllocateMemory", error)) return false;
        return check(vkBindBufferMemory(m_device, buffer, memory, 0), "vkBindBufferMemory", error);
    }

    bool submitAndWait(VkCommandBuffer cmd, std::string& error) {
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = VK_NULL_HANDLE;
        if (!check(vkCreateFence(m_device, &fi, nullptr, &fence), "vkCreateFence", error)) return false;
        bool ok;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            ok = check(vkQueueSubmit(m_queue, 1, &si, fence), "vkQueueSubmit", error);
        }
        if (ok) ok = check(vkWaitForFences(m_device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences", error);
        vkDestroyFence(m_device, fence, nullptr);
        return ok;
    }

    double timestampPeriodNs() const { return m_limits.timestampPeriod; }

private:
    ComputeContext() = default;

    bool init(std::string& error) {
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "eruption-nn";
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ici.pApplicationInfo = &app;
        if (!check(vkCreateInstance(&ici, nullptr, &m_instance), "vkCreateInstance", error)) return false;
        if (!pickDevice(error)) return false;
        if (!createDevice(error)) return false;
        return createPipelines(error);
    }

    // Prefere GPU dedicada; nunca usa rasterizador de software.
    bool pickDevice(std::string& error) {
        uint32_t count = 0;
        vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(m_instance, &count, devices.data());
        const int forced = envInt("ERUPTION_NN_GPU_INDEX", -1);
        int best = -1, bestScore = -1;
        for (uint32_t i = 0; i < count; ++i) {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(devices[i], &props);
            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) continue;
            const int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            if (forced == static_cast<int>(i) || (forced < 0 && score > bestScore)) {
                best = static_cast<int>(i);
                bestScore = score;
            }
        }
        if (best < 0) {
            error = "nenhuma GPU Vulkan";
            return false;
        }
        m_physical = devices[static_cast<size_t>(best)];
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(m_physical, &props);
        m_limits = props.limits;
        m_name = props.deviceName;
        vkGetPhysicalDeviceMemoryProperties(m_physical, &m_memory);
        uint32_t qcount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(m_physical, &qcount, nullptr);
        std::vector<VkQueueFamilyProperties> families(qcount);
        vkGetPhysicalDeviceQueueFamilyProperties(m_physical, &qcount, families.data());
        for (uint32_t i = 0; i < qcount; ++i)
            if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                m_queueFamily = i;
                m_timestampBits = families[i].timestampValidBits;
                return true;
            }
        error = "GPU sem fila de compute";
        return false;
    }

    bool createDevice(std::string& error) {
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo qi{};
        qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qi.queueFamilyIndex = m_queueFamily;
        qi.queueCount = 1;
        qi.pQueuePriorities = &priority;
        VkDeviceCreateInfo di{};
        di.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        di.queueCreateInfoCount = 1;
        di.pQueueCreateInfos = &qi;
        if (!check(vkCreateDevice(m_physical, &di, nullptr, &m_device), "vkCreateDevice", error)) return false;
        vkGetDeviceQueue(m_device, m_queueFamily, 0, &m_queue);
        VkCommandPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pi.queueFamilyIndex = m_queueFamily;
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        return check(vkCreateCommandPool(m_device, &pi, nullptr, &m_commandPool), "vkCreateCommandPool", error);
    }

    bool createPipelines(std::string& error) {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo li{};
        li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        li.bindingCount = 1;
        li.pBindings = &binding;
        if (!check(vkCreateDescriptorSetLayout(m_device, &li, nullptr, &m_setLayout), "vkCreateDescriptorSetLayout", error)) return false;
        VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 128};
        VkPipelineLayoutCreateInfo pli{};
        pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &m_setLayout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &range;
        if (!check(vkCreatePipelineLayout(m_device, &pli, nullptr, &m_pipelineLayout), "vkCreatePipelineLayout", error)) return false;
        for (size_t k = 0; k < static_cast<size_t>(Kernel::Count); ++k) {
            const uint32_t variants = static_cast<Kernel>(k) == Kernel::Conv2d ? kConvVariants : 1;
            for (uint32_t v = 0; v < variants; ++v)
                if (!createPipeline(static_cast<Kernel>(k), v, error)) return false;
        }
        return true;
    }

    // Variante = constante de especializacao 0 (so' a convolucao usa: THREAD_M).
    bool createPipeline(Kernel k, uint32_t variant, std::string& error) {
        const std::vector<char> code = loadSpirv(kernelFile(k));
        if (code.empty() || code.size() % 4) {
            error = std::string("shader ") + kernelFile(k) + ".comp.spv nao encontrado (compilar com o build)";
            return false;
        }
        VkShaderModuleCreateInfo mi{};
        mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        mi.codeSize = code.size();
        mi.pCode = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule module = VK_NULL_HANDLE;
        if (!check(vkCreateShaderModule(m_device, &mi, nullptr, &module), "vkCreateShaderModule", error)) return false;
        VkComputePipelineCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module;
        ci.stage.pName = "main";
        const int32_t threadM = 1 << variant;
        VkSpecializationMapEntry entry{0, 0, sizeof(int32_t)};
        VkSpecializationInfo spec{1, &entry, sizeof(int32_t), &threadM};
        if (k == Kernel::Conv2d) ci.stage.pSpecializationInfo = &spec;
        ci.layout = m_pipelineLayout;
        const bool ok = check(vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &ci, nullptr,
                                                       &m_pipelines[static_cast<size_t>(k) * kConvVariants + variant]),
                              "vkCreateComputePipelines", error);
        vkDestroyShaderModule(m_device, module, nullptr);
        return ok;
    }

    VkInstance m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    uint32_t m_queueFamily = 0;
    uint32_t m_timestampBits = 0;
    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    std::array<VkPipeline, static_cast<size_t>(Kernel::Count) * kConvVariants> m_pipelines{};
    VkPhysicalDeviceMemoryProperties m_memory{};
    VkPhysicalDeviceLimits m_limits{};
    std::string m_name;
    std::mutex m_mutex;      // uso do command pool
    std::mutex m_queueMutex; // vkQueueSubmit
};

// ---- programa de uma forma de entrada na GPU ----------------------------------

uint32_t absoluteOffset(const NnProgram& prog, const NnView& v) {
    return static_cast<uint32_t>(prog.buffers[static_cast<size_t>(v.buffer)].offset + v.offset);
}

uint32_t u32(int64_t v) { return static_cast<uint32_t>(v); }

template <typename T>
void pushParams(VkCommandBuffer cmd, VkPipelineLayout layout, const T& params) {
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(T), &params);
}

// Despacho 1D de 'total' itens em grupos de 256, quebrado em 2D se precisar.
void dispatchLinear(VkCommandBuffer cmd, size_t total, uint32_t groupSize) {
    const size_t groups = (total + groupSize - 1) / groupSize;
    const uint32_t gx = static_cast<uint32_t>(std::min<size_t>(groups, kMaxGroupsPerAxis));
    const uint32_t gy = static_cast<uint32_t>((groups + gx - 1) / gx);
    vkCmdDispatch(cmd, gx, gy, 1);
}

void computeBarrier(VkCommandBuffer cmd) {
    VkMemoryBarrier mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0,
                         nullptr, 0, nullptr);
}

void transferBarrier(VkCommandBuffer cmd, VkPipelineStageFlags src, VkAccessFlags srcAccess, VkPipelineStageFlags dst,
                     VkAccessFlags dstAccess) {
    VkMemoryBarrier mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = srcAccess;
    mb.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cmd, src, dst, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

void recordCopy(VkCommandBuffer cmd, VkPipelineLayout layout, const NnProgram& prog, const NnOp& op) {
    CopyParams p{};
    size_t total = 1;
    for (int d = 0; d < kNnMaxRank; ++d) {
        p.dims[d] = u32(op.dims[static_cast<size_t>(d)]);
        p.srcStrides[d] = static_cast<int32_t>(op.strides0[static_cast<size_t>(d)]);
        p.dstStrides[d] = static_cast<int32_t>(op.stridesOut[static_cast<size_t>(d)]);
        total *= static_cast<size_t>(op.dims[static_cast<size_t>(d)]);
    }
    p.srcBase = absoluteOffset(prog, op.in[0]);
    p.dstBase = absoluteOffset(prog, op.out);
    p.total = static_cast<uint32_t>(total);
    pushParams(cmd, layout, p);
    dispatchLinear(cmd, total, kGroupSize);
}

void recordBinary(VkCommandBuffer cmd, VkPipelineLayout layout, const NnProgram& prog, const NnOp& op) {
    BinaryParams p{};
    for (int d = 0; d < kNnMaxRank; ++d) {
        p.dims[d] = u32(op.dims[static_cast<size_t>(d)]);
        p.stridesA[d] = u32(op.strides0[static_cast<size_t>(d)]);
        p.stridesB[d] = u32(op.strides1[static_cast<size_t>(d)]);
    }
    p.baseA = absoluteOffset(prog, op.in[0]);
    p.baseB = absoluteOffset(prog, op.in[1]);
    p.baseOut = absoluteOffset(prog, op.out);
    p.total = static_cast<uint32_t>(viewCount(op.out));
    p.code = op.code;
    pushParams(cmd, layout, p);
    dispatchLinear(cmd, p.total, kGroupSize);
}

void recordUnary(VkCommandBuffer cmd, VkPipelineLayout layout, const NnProgram& prog, const NnOp& op) {
    UnaryParams p{};
    p.baseIn = absoluteOffset(prog, op.in[0]);
    p.baseOut = absoluteOffset(prog, op.out);
    p.total = static_cast<uint32_t>(viewCount(op.out));
    p.code = op.code;
    p.param0 = op.param0;
    p.param1 = op.param1;
    pushParams(cmd, layout, p);
    dispatchLinear(cmd, p.total, kGroupSize);
}

void recordGather(VkCommandBuffer cmd, VkPipelineLayout layout, const NnProgram& prog, const NnOp& op) {
    GatherParams p{};
    p.baseSrc = absoluteOffset(prog, op.in[0]);
    p.baseIdx = absoluteOffset(prog, op.in[1]);
    p.baseOut = absoluteOffset(prog, op.out);
    p.outer = u32(op.ints[0]);
    p.count = u32(op.ints[1]);
    p.inner = u32(op.ints[2]);
    p.axisSize = u32(op.ints[3]);
    p.total = static_cast<uint32_t>(viewCount(op.out));
    pushParams(cmd, layout, p);
    dispatchLinear(cmd, p.total, kGroupSize);
}

void recordReduce(VkCommandBuffer cmd, VkPipelineLayout layout, const NnProgram& prog, const NnOp& op) {
    ReduceParams p{};
    size_t total = 1;
    for (int d = 0; d < kNnMaxRank; ++d) {
        p.dims[d] = u32(op.dims[static_cast<size_t>(d)]);
        p.strides[d] = u32(op.strides0[static_cast<size_t>(d)]);
        total *= static_cast<size_t>(op.dims[static_cast<size_t>(d)]);
    }
    p.mask = u32(op.ints[0]);
    p.baseIn = absoluteOffset(prog, op.in[0]);
    p.baseOut = absoluteOffset(prog, op.out);
    p.outCount = static_cast<uint32_t>(viewCount(op.out));
    p.reduceCount = static_cast<uint32_t>(total / std::max<size_t>(p.outCount, 1));
    p.code = op.code;
    pushParams(cmd, layout, p);
    dispatchLinear(cmd, p.outCount, 1); // um grupo por elemento de saida
}

void recordPool(VkCommandBuffer cmd, VkPipelineLayout layout, const NnProgram& prog, const NnOp& op) {
    const NnView& x = op.in[0];
    PoolParams p{};
    p.baseIn = absoluteOffset(prog, x);
    p.baseOut = absoluteOffset(prog, op.out);
    p.planes = u32(x.shape[0] * x.shape[1]);
    p.height = static_cast<int32_t>(x.shape[2]);
    p.width = static_cast<int32_t>(x.shape[3]);
    p.outHeight = static_cast<int32_t>(op.out.shape[2]);
    p.outWidth = static_cast<int32_t>(op.out.shape[3]);
    p.kernelH = static_cast<int32_t>(op.ints[0]);
    p.kernelW = static_cast<int32_t>(op.ints[1]);
    p.strideH = static_cast<int32_t>(op.ints[2]);
    p.strideW = static_cast<int32_t>(op.ints[3]);
    p.padTop = static_cast<int32_t>(op.ints[4]);
    p.padLeft = static_cast<int32_t>(op.ints[5]);
    p.countIncludePad = u32(op.ints[6]);
    p.isMax = op.code == static_cast<uint32_t>(NnPool::Max) ? 1u : 0u;
    p.total = static_cast<uint32_t>(viewCount(op.out));
    pushParams(cmd, layout, p);
    dispatchLinear(cmd, p.total, kGroupSize);
}

void recordResize(VkCommandBuffer cmd, VkPipelineLayout layout, const NnProgram& prog, const NnOp& op) {
    const NnView& x = op.in[0];
    ResizeParams p{};
    p.baseIn = absoluteOffset(prog, x);
    p.baseOut = absoluteOffset(prog, op.out);
    p.height = static_cast<int32_t>(x.shape[2]);
    p.width = static_cast<int32_t>(x.shape[3]);
    p.outHeight = static_cast<int32_t>(op.out.shape[2]);
    p.outWidth = static_cast<int32_t>(op.out.shape[3]);
    p.linear = op.code;
    p.coordMode = u32(op.ints[0]);
    p.nearestMode = u32(op.ints[1]);
    p.scaleH = op.param0;
    p.scaleW = op.param1;
    p.total = static_cast<uint32_t>(viewCount(op.out));
    pushParams(cmd, layout, p);
    dispatchLinear(cmd, p.total, kGroupSize);
}

// Menor bloco de canais que cobre a camada (ate' 64).
uint32_t convVariantFor(const NnOp& op) {
    const int64_t perGroup = op.in[1].shape[0] / std::max<int64_t>(op.ints[6], 1);
    uint32_t v = 0;
    while (v + 1 < kConvVariants && perGroup > static_cast<int64_t>(kConvChannelsPerThreadRow << v)) ++v;
    return v;
}

uint32_t variantFor(const NnOp& op) { return op.kind == NnOpKind::Conv2d ? convVariantFor(op) : 0; }

void recordConv(VkCommandBuffer cmd, VkPipelineLayout layout, const NnProgram& prog, const NnOp& op) {
    const NnView &x = op.in[0], &w = op.in[1];
    ConvParams p{};
    p.baseX = absoluteOffset(prog, x);
    p.baseW = absoluteOffset(prog, w);
    p.hasBias = op.in.size() > 2 ? 1u : 0u;
    p.baseBias = p.hasBias ? absoluteOffset(prog, op.in[2]) : 0u;
    p.baseOut = absoluteOffset(prog, op.out);
    p.batch = static_cast<int32_t>(x.shape[0]);
    p.channels = static_cast<int32_t>(x.shape[1]);
    p.height = static_cast<int32_t>(x.shape[2]);
    p.width = static_cast<int32_t>(x.shape[3]);
    p.outChannels = static_cast<int32_t>(w.shape[0]);
    p.channelsPerGroup = static_cast<int32_t>(w.shape[1]);
    p.kernelH = static_cast<int32_t>(w.shape[2]);
    p.kernelW = static_cast<int32_t>(w.shape[3]);
    p.outHeight = static_cast<int32_t>(op.out.shape[2]);
    p.outWidth = static_cast<int32_t>(op.out.shape[3]);
    p.padTop = static_cast<int32_t>(op.ints[0]);
    p.padLeft = static_cast<int32_t>(op.ints[1]);
    p.strideH = static_cast<int32_t>(op.ints[2]);
    p.strideW = static_cast<int32_t>(op.ints[3]);
    p.dilationH = static_cast<int32_t>(op.ints[4]);
    p.dilationW = static_cast<int32_t>(op.ints[5]);
    p.groups = static_cast<int32_t>(op.ints[6]);
    const uint32_t perGroup = u32(p.outChannels / p.groups);
    const uint32_t pixels = u32(p.outHeight) * u32(p.outWidth);
    p.total = u32(p.batch) * u32(p.outChannels) * pixels;
    pushParams(cmd, layout, p);
    const uint32_t blockChannels = kConvChannelsPerThreadRow << convVariantFor(op);
    vkCmdDispatch(cmd, (pixels + kConvBlockPixels - 1) / kConvBlockPixels,
                  (perGroup + blockChannels - 1) / blockChannels, u32(p.batch) * u32(p.groups));
}

void recordMatMul(VkCommandBuffer cmd, VkPipelineLayout layout, const NnProgram& prog, const NnOp& op) {
    MatMulParams p{};
    p.baseA = absoluteOffset(prog, op.in[0]);
    p.baseB = absoluteOffset(prog, op.in[1]);
    p.baseOut = absoluteOffset(prog, op.out);
    p.rows = u32(op.ints[0]);
    p.cols = u32(op.ints[1]);
    p.inner = u32(op.ints[2]);
    uint32_t batches = 1;
    for (int d = 0; d < 4; ++d) {
        p.batchDims[d] = u32(op.dims[static_cast<size_t>(d)]);
        p.stridesA[d] = u32(op.strides0[static_cast<size_t>(d)]);
        p.stridesB[d] = u32(op.strides1[static_cast<size_t>(d)]);
        batches *= p.batchDims[d];
    }
    pushParams(cmd, layout, p);
    vkCmdDispatch(cmd, (p.cols + kMatMulTile - 1) / kMatMulTile, (p.rows + kMatMulTile - 1) / kMatMulTile, batches);
}

void recordOp(VkCommandBuffer cmd, VkPipelineLayout layout, const NnProgram& prog, const NnOp& op) {
    switch (op.kind) {
    case NnOpKind::Copy: recordCopy(cmd, layout, prog, op); break;
    case NnOpKind::Binary: recordBinary(cmd, layout, prog, op); break;
    case NnOpKind::Unary: recordUnary(cmd, layout, prog, op); break;
    case NnOpKind::Conv2d: recordConv(cmd, layout, prog, op); break;
    case NnOpKind::MatMul: recordMatMul(cmd, layout, prog, op); break;
    case NnOpKind::Gather: recordGather(cmd, layout, prog, op); break;
    case NnOpKind::Reduce: recordReduce(cmd, layout, prog, op); break;
    case NnOpKind::Pool2d: recordPool(cmd, layout, prog, op); break;
    case NnOpKind::Resize: recordResize(cmd, layout, prog, op); break;
    }
}

class GpuProgram {
public:
    GpuProgram(std::shared_ptr<ComputeContext> ctx, NnProgram prog) : m_ctx(std::move(ctx)), m_prog(std::move(prog)) {}

    ~GpuProgram() {
        const VkDevice dev = m_ctx->device();
        if (m_cmd) vkFreeCommandBuffers(dev, m_ctx->commandPool(), 1, &m_cmd);
        if (m_queries) vkDestroyQueryPool(dev, m_queries, nullptr);
        if (m_descPool) vkDestroyDescriptorPool(dev, m_descPool, nullptr);
        if (m_mapped) vkUnmapMemory(dev, m_stagingMem);
        if (m_staging) vkDestroyBuffer(dev, m_staging, nullptr);
        if (m_stagingMem) vkFreeMemory(dev, m_stagingMem, nullptr);
        if (m_arena) vkDestroyBuffer(dev, m_arena, nullptr);
        if (m_arenaMem) vkFreeMemory(dev, m_arenaMem, nullptr);
    }

    GpuProgram(const GpuProgram&) = delete;
    GpuProgram& operator=(const GpuProgram&) = delete;

    bool init(std::string& error) {
        const VkDeviceSize arenaBytes = m_prog.arenaElements * sizeof(float);
        if (arenaBytes > m_ctx->limits().maxStorageBufferRange) {
            error = "rede pede " + std::to_string(arenaBytes >> 20) + " MB num buffer so', acima do limite da GPU";
            return false;
        }
        m_inBytes = viewCount(m_prog.input) * sizeof(float);
        m_outBytes = viewCount(m_prog.output) * sizeof(float);
        m_outStagingOffset = (m_inBytes + 255) / 256 * 256;
        const VkDeviceSize constBytes = m_prog.constantElements * sizeof(float);
        const VkDeviceSize stagingBytes = std::max<VkDeviceSize>(m_outStagingOffset + m_outBytes, constBytes);
        if (!m_ctx->createBuffer(arenaBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, m_arena, m_arenaMem, error))
            return false;
        if (!createStaging(stagingBytes, error)) return false;
        if (!check(vkMapMemory(m_ctx->device(), m_stagingMem, 0, VK_WHOLE_SIZE, 0, &m_mapped), "vkMapMemory", error)) return false;
        return createDescriptor(error) && allocateCommandBuffer(error) && uploadConstants(error) && recordInference(error);
    }

    bool run(const std::vector<float>& input, std::vector<float>& output, std::string& error) {
        if (input.size() * sizeof(float) != m_inBytes) {
            error = "entrada com tamanho errado";
            return false;
        }
        std::memcpy(m_mapped, input.data(), m_inBytes);
        if (!m_ctx->submitAndWait(m_cmd, error)) return false;
        output.resize(m_outBytes / sizeof(float));
        std::memcpy(output.data(), static_cast<const char*>(m_mapped) + m_outStagingOffset, m_outBytes);
        readTimestamps();
        return true;
    }

    const std::vector<int64_t>& outputShape() const { return m_prog.output.shape; }
    size_t memoryBytes() const { return m_prog.arenaElements * sizeof(float); }
    double lastGpuMs() const { return m_lastGpuMs; }

private:
    // Staging com cache no host quando existe: a leitura da saida de volta
    // de memoria sem cache (write-combined) custa dezenas de ms.
    bool createStaging(VkDeviceSize bytes, std::string& error) {
        const VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        const VkMemoryPropertyFlags coherent = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        std::string ignored;
        if (m_ctx->createBuffer(bytes, usage, coherent | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, m_staging, m_stagingMem, ignored))
            return true;
        if (m_staging) vkDestroyBuffer(m_ctx->device(), m_staging, nullptr);
        m_staging = VK_NULL_HANDLE;
        return m_ctx->createBuffer(bytes, usage, coherent, m_staging, m_stagingMem, error);
    }

    bool createDescriptor(std::string& error) {
        const VkDevice dev = m_ctx->device();
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
        VkDescriptorPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pi.maxSets = 1;
        pi.poolSizeCount = 1;
        pi.pPoolSizes = &size;
        if (!check(vkCreateDescriptorPool(dev, &pi, nullptr, &m_descPool), "vkCreateDescriptorPool", error)) return false;
        const VkDescriptorSetLayout layout = m_ctx->setLayout();
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = m_descPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &layout;
        if (!check(vkAllocateDescriptorSets(dev, &ai, &m_set), "vkAllocateDescriptorSets", error)) return false;
        VkDescriptorBufferInfo bi{m_arena, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = m_set;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
        return true;
    }

    bool allocateCommandBuffer(std::string& error) {
        std::lock_guard<std::mutex> lock(m_ctx->mutex());
        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = m_ctx->commandPool();
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (!check(vkAllocateCommandBuffers(m_ctx->device(), &ai, &m_cmd), "vkAllocateCommandBuffers", error)) return false;
        if (!m_ctx->hasTimestamps()) return true;
        VkQueryPoolCreateInfo qi{};
        qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = queryCount();
        return check(vkCreateQueryPool(m_ctx->device(), &qi, nullptr, &m_queries), "vkCreateQueryPool", error);
    }

    // Constantes (pesos e tabelas resolvidas na CPU) sobem uma vez.
    bool uploadConstants(std::string& error) {
        if (m_prog.constantElements == 0) return true;
        float* dst = static_cast<float*>(m_mapped);
        std::fill(dst, dst + m_prog.constantElements, 0.0f);
        for (const NnBuffer& b : m_prog.buffers)
            if (b.constant) std::copy(b.data.begin(), b.data.end(), dst + b.offset);
        std::lock_guard<std::mutex> lock(m_ctx->mutex());
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(m_cmd, &bi);
        VkBufferCopy region{0, 0, m_prog.constantElements * sizeof(float)};
        vkCmdCopyBuffer(m_cmd, m_staging, m_arena, 1, &region);
        vkEndCommandBuffer(m_cmd);
        if (!m_ctx->submitAndWait(m_cmd, error)) return false;
        return check(vkResetCommandBuffer(m_cmd, 0), "vkResetCommandBuffer", error);
    }

    // Grava a inferencia inteira uma vez: entrada -> ops -> saida.
    bool recordInference(std::string& error) {
        std::lock_guard<std::mutex> lock(m_ctx->mutex());
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        vkBeginCommandBuffer(m_cmd, &bi);
        if (m_queries) {
            vkCmdResetQueryPool(m_cmd, m_queries, 0, queryCount());
            vkCmdWriteTimestamp(m_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_queries, 0);
        }
        VkBufferCopy in{0, absoluteOffset(m_prog, m_prog.input) * sizeof(float), m_inBytes};
        vkCmdCopyBuffer(m_cmd, m_staging, m_arena, 1, &in);
        transferBarrier(m_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        const VkPipelineLayout layout = m_ctx->pipelineLayout();
        vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &m_set, 0, nullptr);
        VkPipeline bound = VK_NULL_HANDLE;
        for (const NnOp& op : m_prog.ops) {
            const VkPipeline pipeline = m_ctx->pipeline(kernelFor(op.kind), variantFor(op));
            if (pipeline != bound) {
                vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
                bound = pipeline;
            }
            recordOp(m_cmd, layout, m_prog, op);
            if (m_profile)
                vkCmdWriteTimestamp(m_cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, m_queries,
                                    static_cast<uint32_t>(2 + (&op - m_prog.ops.data())));
            computeBarrier(m_cmd);
        }
        transferBarrier(m_cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        VkBufferCopy out{absoluteOffset(m_prog, m_prog.output) * sizeof(float), m_outStagingOffset, m_outBytes};
        vkCmdCopyBuffer(m_cmd, m_arena, m_staging, 1, &out);
        transferBarrier(m_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
        if (m_queries) vkCmdWriteTimestamp(m_cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_queries, 1);
        return check(vkEndCommandBuffer(m_cmd), "vkEndCommandBuffer", error);
    }

    uint32_t queryCount() const { return m_profile ? static_cast<uint32_t>(m_prog.ops.size() + 2) : 2u; }

    void readTimestamps() {
        if (!m_queries) return;
        std::vector<uint64_t> ts(queryCount(), 0);
        if (vkGetQueryPoolResults(m_ctx->device(), m_queries, 0, queryCount(), ts.size() * sizeof(uint64_t), ts.data(),
                                  sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
            return;
        const double toMs = m_ctx->timestampPeriodNs() / 1e6;
        m_lastGpuMs = static_cast<double>(ts[1] - ts[0]) * toMs;
        if (m_profile) printProfile(ts, toMs);
    }

    // ERUPTION_NN_PROFILE=1: tempo de GPU somado por tipo de kernel.
    void printProfile(const std::vector<uint64_t>& ts, double toMs) const {
        std::array<double, static_cast<size_t>(Kernel::Count)> ms{};
        std::array<int, static_cast<size_t>(Kernel::Count)> count{};
        uint64_t prev = ts[0];
        for (size_t i = 0; i < m_prog.ops.size(); ++i) {
            const size_t k = static_cast<size_t>(kernelFor(m_prog.ops[i].kind));
            ms[k] += static_cast<double>(ts[2 + i] - prev) * toMs;
            ++count[k];
            prev = ts[2 + i];
        }
        std::printf("[NN] perfil GPU (%.2f ms):", m_lastGpuMs);
        for (size_t k = 0; k < ms.size(); ++k)
            if (count[k]) std::printf(" %s %.2f ms/%d", kernelFile(static_cast<Kernel>(k)), ms[k], count[k]);
        std::printf("\n");
    }

    std::shared_ptr<ComputeContext> m_ctx;
    NnProgram m_prog;
    VkBuffer m_arena = VK_NULL_HANDLE, m_staging = VK_NULL_HANDLE;
    VkDeviceMemory m_arenaMem = VK_NULL_HANDLE, m_stagingMem = VK_NULL_HANDLE;
    void* m_mapped = nullptr;
    VkDescriptorPool m_descPool = VK_NULL_HANDLE;
    VkDescriptorSet m_set = VK_NULL_HANDLE;
    VkCommandBuffer m_cmd = VK_NULL_HANDLE;
    VkQueryPool m_queries = VK_NULL_HANDLE;
    VkDeviceSize m_inBytes = 0, m_outBytes = 0, m_outStagingOffset = 0;
    double m_lastGpuMs = 0.0;
    bool m_profile = envInt("ERUPTION_NN_PROFILE", 0) != 0;
};

} // namespace

struct NnVulkanExecutor::Impl {
    std::shared_ptr<ComputeContext> ctx;
    std::shared_ptr<const OnnxModel> model;
    std::mutex mutex;
    std::map<std::vector<int64_t>, std::unique_ptr<GpuProgram>> programs;
    std::vector<std::vector<int64_t>> recency; // mais recente no fim
    size_t cachedBytes = 0;
    size_t budgetBytes = static_cast<size_t>(envInt("ERUPTION_NN_GPU_MEM_MB", kDefaultProgramBudgetMB)) << 20;
    double lastGpuMs = 0.0;

    GpuProgram* programFor(const std::vector<int64_t>& shape, std::string& error) {
        const auto it = programs.find(shape);
        if (it != programs.end()) {
            touch(shape);
            return it->second.get();
        }
        NnProgram prog;
        if (!buildNnProgram(*model, shape, prog, error)) return nullptr;
        evictToFit(prog.arenaElements * sizeof(float));
        auto gpu = std::make_unique<GpuProgram>(ctx, std::move(prog));
        if (!gpu->init(error)) return nullptr;
        cachedBytes += gpu->memoryBytes();
        GpuProgram* raw = gpu.get();
        programs[shape] = std::move(gpu);
        touch(shape);
        return raw;
    }

    void touch(const std::vector<int64_t>& shape) {
        recency.erase(std::remove(recency.begin(), recency.end(), shape), recency.end());
        recency.push_back(shape);
    }

    void evictToFit(size_t incoming) {
        while (!recency.empty() && cachedBytes + incoming > budgetBytes) {
            const auto it = programs.find(recency.front());
            cachedBytes -= it->second->memoryBytes();
            programs.erase(it);
            recency.erase(recency.begin());
        }
    }
};

std::unique_ptr<NnVulkanExecutor> NnVulkanExecutor::create(std::shared_ptr<const OnnxModel> model, std::string& error) {
    if (envInt("ERUPTION_NN_GPU", 1) == 0) {
        error = "GPU desligada (ERUPTION_NN_GPU=0)";
        return nullptr;
    }
    std::shared_ptr<ComputeContext> ctx = ComputeContext::acquire(error);
    if (!ctx) return nullptr;
    // Planeja um tamanho pequeno ja' aqui: op nao suportada aparece no load,
    // nao no meio do bake.
    NnProgram probe;
    if (!buildNnProgram(*model, {1, 3, 32, 32}, probe, error)) return nullptr;
    std::unique_ptr<NnVulkanExecutor> exec(new NnVulkanExecutor());
    exec->m_impl = std::make_unique<Impl>();
    exec->m_impl->ctx = std::move(ctx);
    exec->m_impl->model = std::move(model);
    return exec;
}

NnVulkanExecutor::~NnVulkanExecutor() = default;

bool NnVulkanExecutor::run(const std::vector<int64_t>& inputShape, const std::vector<float>& input,
                           std::vector<float>& output, std::vector<int64_t>& outputShape, std::string& error) {
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    GpuProgram* prog = m_impl->programFor(inputShape, error);
    if (!prog || !prog->run(input, output, error)) return false;
    outputShape = prog->outputShape();
    m_impl->lastGpuMs = prog->lastGpuMs();
    return true;
}

const std::string& NnVulkanExecutor::deviceName() const { return m_impl->ctx->name(); }

double NnVulkanExecutor::lastGpuMs() const { return m_impl->lastGpuMs; }

} // namespace eruption
