// Ver VkTrace.hpp. As funcoes vk* abaixo SOBREPOEM as da libvulkan para as chamadas feitas de dentro do motor
// (o simbolo do proprio binario/biblioteca vem primeiro na busca do ligador dinamico) e repassam para a
// original com dlsym(RTLD_NEXT). Chamadas por ponteiro (VMA, ImGui) nao passam por aqui.
#if ERUPTION_DEV && (defined(__linux__) || defined(__ANDROID__))
#include "utils/VkTrace.hpp"
#include <vulkan/vulkan.h>
#include <atomic>
#include <dlfcn.h>

namespace eruption::vktrace {

static std::atomic<uint64_t> s_count[kOpCount];
static std::atomic<int64_t> s_ns[kOpCount];

void add(Op op, int64_t ns) {
    s_count[op].fetch_add(1, std::memory_order_relaxed);
    s_ns[op].fetch_add(ns, std::memory_order_relaxed);
}

void snapshot(uint64_t count[kOpCount], int64_t ns[kOpCount]) {
    for (int i = 0; i < kOpCount; ++i) {
        count[i] = s_count[i].load(std::memory_order_relaxed);
        ns[i] = s_ns[i].load(std::memory_order_relaxed);
    }
}

const char* name(Op op) {
    static const char* const kNames[kOpCount] = {"shader", "compute", "submit", "queueIdle", "devIdle", "fence",
                                                 "present", "acquire", "image", "memoria", "buffer", "spv",
                                                 "redesenho", "upload", "outro"};
    return kNames[op];
}

} // namespace eruption::vktrace

namespace {
using eruption::vktrace::Op;
using eruption::vktrace::Scope;

template <class Fn>
Fn realFn(const char* symbol) {
    return reinterpret_cast<Fn>(dlsym(RTLD_NEXT, symbol));
}
} // namespace

extern "C" {

VkResult vkCreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo* i, const VkAllocationCallbacks* a, VkShaderModule* o) {
    static const auto real = realFn<PFN_vkCreateShaderModule>("vkCreateShaderModule");
    Scope s(Op::ShaderModule);
    return real(d, i, a, o);
}

VkResult vkCreateComputePipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkComputePipelineCreateInfo* i,
                                  const VkAllocationCallbacks* a, VkPipeline* o) {
    static const auto real = realFn<PFN_vkCreateComputePipelines>("vkCreateComputePipelines");
    Scope s(Op::ComputePipeline);
    return real(d, c, n, i, a, o);
}

VkResult vkQueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo* i, VkFence f) {
    static const auto real = realFn<PFN_vkQueueSubmit>("vkQueueSubmit");
    Scope s(Op::Submit);
    return real(q, n, i, f);
}

VkResult vkQueueWaitIdle(VkQueue q) {
    static const auto real = realFn<PFN_vkQueueWaitIdle>("vkQueueWaitIdle");
    Scope s(Op::QueueWaitIdle);
    return real(q);
}

VkResult vkDeviceWaitIdle(VkDevice d) {
    static const auto real = realFn<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle");
    Scope s(Op::DeviceWaitIdle);
    return real(d);
}

VkResult vkWaitForFences(VkDevice d, uint32_t n, const VkFence* f, VkBool32 all, uint64_t t) {
    static const auto real = realFn<PFN_vkWaitForFences>("vkWaitForFences");
    Scope s(Op::WaitFences);
    return real(d, n, f, all, t);
}

VkResult vkQueuePresentKHR(VkQueue q, const VkPresentInfoKHR* i) {
    static const auto real = realFn<PFN_vkQueuePresentKHR>("vkQueuePresentKHR");
    Scope s(Op::Present);
    return real(q, i);
}

VkResult vkAcquireNextImageKHR(VkDevice d, VkSwapchainKHR sc, uint64_t t, VkSemaphore sem, VkFence f, uint32_t* idx) {
    static const auto real = realFn<PFN_vkAcquireNextImageKHR>("vkAcquireNextImageKHR");
    Scope s(Op::Acquire);
    return real(d, sc, t, sem, f, idx);
}

VkResult vkCreateImage(VkDevice d, const VkImageCreateInfo* i, const VkAllocationCallbacks* a, VkImage* o) {
    static const auto real = realFn<PFN_vkCreateImage>("vkCreateImage");
    Scope s(Op::CreateImage);
    return real(d, i, a, o);
}

VkResult vkAllocateMemory(VkDevice d, const VkMemoryAllocateInfo* i, const VkAllocationCallbacks* a, VkDeviceMemory* o) {
    static const auto real = realFn<PFN_vkAllocateMemory>("vkAllocateMemory");
    Scope s(Op::AllocMemory);
    return real(d, i, a, o);
}

VkResult vkCreateBuffer(VkDevice d, const VkBufferCreateInfo* i, const VkAllocationCallbacks* a, VkBuffer* o) {
    static const auto real = realFn<PFN_vkCreateBuffer>("vkCreateBuffer");
    Scope s(Op::CreateBuffer);
    return real(d, i, a, o);
}

} // extern "C"
#elif ERUPTION_DEV
#include "utils/VkTrace.hpp"
namespace eruption::vktrace {
void add(Op, int64_t) {}
void snapshot(uint64_t count[kOpCount], int64_t ns[kOpCount]) {
    for (int i = 0; i < kOpCount; ++i) count[i] = 0, ns[i] = 0;
}
const char* name(Op) { return ""; }
} // namespace eruption::vktrace
#endif
