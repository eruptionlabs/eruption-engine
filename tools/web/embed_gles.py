#!/usr/bin/env python3
"""Embute shaders GLSL ES 3.00 (saida do shaders_gles.py) num cabecalho C++.

A cena WebGL2 (src/core/EngineWebScene.cpp) desenha com os MESMOS shaders do
desktop, convertidos do SPIR-V no build; o wasm nao le arquivo de shader.
Cada entrada vira `inline const char* kGles_<nome>` com o texto do shader.

Uso: embed_gles.py <saida.hpp> <nome>=<arquivo.gles> [...]
"""
import sys


def main():
    out = sys.argv[1]
    parts = ["// Gerado por tools/web/embed_gles.py; nao editar.\n#pragma once\n\nnamespace eruption {\n"]
    for item in sys.argv[2:]:
        name, path = item.split("=", 1)
        with open(path) as f:
            text = f.read()
        if ")GLES\"" in text:
            raise SystemExit(f"{path}: texto contem o delimitador")
        parts.append(f'inline const char* kGles_{name} = R"GLES({text})GLES";\n')
    parts.append("} // namespace eruption\n")
    with open(out, "w") as f:
        f.write("".join(parts))


if __name__ == "__main__":
    main()
