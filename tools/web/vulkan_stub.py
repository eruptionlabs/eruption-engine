#!/usr/bin/env python3
"""Gera o resto do dispositivo Vulkan nulo do build web (Emscripten).

Nao existe Vulkan no navegador. src/platform/web/VulkanNull.cpp responde as
consultas que importam (GPU, filas, swapchain, memoria); este script define
todas as outras vkXxx do vulkan_core.h:
- VkResult -> VK_SUCCESS;
- ultimo parametro ponteiro nao-const para handle (vkCreateFoo(..., VkFoo*),
  vkGetDeviceQueue(..., VkQueue*)) -> recebe um handle falso, nunca nulo;
- vkGetInstanceProcAddr/vkGetDeviceProcAddr -> tabela por nome (o VMA e o
  motor carregam funcoes assim).
O resto nao faz nada e devolve zero.

Uso: vulkan_stub.py <vulkan_core.h> <VulkanNull.cpp> <saida.cpp>
"""
import re
import sys

PROTO = re.compile(r"VKAPI_ATTR\s+(.+?)\s+VKAPI_CALL\s+(vk\w+)\s*\((.*?)\);", re.S)
HANDLE = re.compile(r"VK_DEFINE(?:_NON_DISPATCHABLE)?_HANDLE\((\w+)\)")
DEFINED = re.compile(r"VKAPI_CALL\s+(vk\w+)\s*\(")
PROC_ADDR = {"vkGetInstanceProcAddr", "vkGetDeviceProcAddr"}


def last_handle_out(params, handles):
    """Nome e tipo do ultimo parametro se for `VkFoo* nome` (nao-const) de handle."""
    last = params.split(",")[-1].strip()
    m = re.fullmatch(r"(\w+)\s*\*\s*(\w+)", last)
    if m and m.group(1) in handles:
        return m.group(1), m.group(2)
    return None


def main():
    header, null_cpp, out = sys.argv[1], sys.argv[2], sys.argv[3]
    text = open(header).read()
    handles = set(HANDLE.findall(text))
    handwritten = set(DEFINED.findall(open(null_cpp).read()))
    lines = [
        "// Gerado por tools/web/vulkan_stub.py: nao editar.",
        "#include <vulkan/vulkan.h>",
        "#include <cstring>",
        "void* eruptionVkFakeHandle(); // src/platform/web/VulkanNull.cpp",
        'extern "C" {',
    ]
    names = []
    for ret, name, params in PROTO.findall(text):
        if name in names:
            continue
        names.append(name)
        if name in handwritten or name in PROC_ADDR:
            continue
        params = " ".join(params.split())
        ret = " ".join(ret.split())
        body = []
        out_handle = last_handle_out(params, handles)
        if out_handle:
            body.append(f"if ({out_handle[1]}) *{out_handle[1]} = reinterpret_cast<{out_handle[0]}>(eruptionVkFakeHandle());")
        if ret == "VkResult":
            body.append("return VK_SUCCESS;")
        elif ret != "void":
            body.append("return {};")
        lines.append(f"VKAPI_ATTR {ret} VKAPI_CALL {name}({params}) {{ {' '.join(body)} }}")
    lines.append("struct EruptionVkProc { const char* name; PFN_vkVoidFunction fn; };")
    lines.append("static const EruptionVkProc kEruptionVkProcs[] = {")
    for name in names:
        lines.append(f'    {{"{name}", reinterpret_cast<PFN_vkVoidFunction>(&{name})}},')
    lines.append("};")
    lines.append("static PFN_vkVoidFunction eruptionVkFind(const char* name) {")
    lines.append("    for (const EruptionVkProc& p : kEruptionVkProcs) if (std::strcmp(p.name, name) == 0) return p.fn;")
    lines.append("    return nullptr;")
    lines.append("}")
    lines.append("VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance, const char* n) { return eruptionVkFind(n); }")
    lines.append("VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice, const char* n) { return eruptionVkFind(n); }")
    lines.append("}")
    with open(out, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"vulkan_stub: {len(names)} funcoes, {len(handwritten)} escritas a mao")


if __name__ == "__main__":
    main()
