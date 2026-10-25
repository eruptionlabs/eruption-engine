#!/usr/bin/env python3
"""Gera o stub do Vulkan para o build web (Emscripten).

Nao existe Vulkan no navegador. Os renderers que ainda nao passaram para a
RHI (src/rhi/) compilam contra os cabecalhos e precisam ligar; este stub
define cada vkXxx do vulkan_core.h: o que devolve VkResult devolve
VK_ERROR_INITIALIZATION_FAILED, o resto nao faz nada e devolve zero. O
VulkanContext ja trata a falha de vkCreateInstance, e o caminho WebGL2 assume.

Uso: vulkan_stub.py <vulkan_core.h> <saida.cpp>
"""
import re
import sys

PROTO = re.compile(r"VKAPI_ATTR\s+(.+?)\s+VKAPI_CALL\s+(vk\w+)\s*\((.*?)\);", re.S)


def main():
    header, out = sys.argv[1], sys.argv[2]
    text = open(header).read()
    lines = [
        "// Gerado por tools/web/vulkan_stub.py: nao editar.",
        "#include <vulkan/vulkan.h>",
        'extern "C" {',
    ]
    seen = set()
    for ret, name, params in PROTO.findall(text):
        if name in seen:
            continue
        seen.add(name)
        params = " ".join(params.split())
        ret = " ".join(ret.split())
        if ret == "void":
            body = "{}"
        elif ret == "VkResult":
            body = "{ return VK_ERROR_INITIALIZATION_FAILED; }"
        else:
            body = "{ return {}; }"
        lines.append(f"VKAPI_ATTR {ret} VKAPI_CALL {name}({params}) {body}")
    lines.append("}")
    with open(out, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"vulkan_stub: {len(seen)} funcoes")


if __name__ == "__main__":
    main()
