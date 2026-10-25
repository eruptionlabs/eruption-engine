#!/usr/bin/env python3
"""SPIR-V do motor -> GLSL ES 3.00 para o backend WebGL2 da RHI (src/rhi/).

Roda no build web, nunca no jogo: o wasm nao carrega compilador de shader.
Para cada <nome>.spv grafico (vert/frag) grava ao lado:
  <nome>.gles      texto GLSL ES 3.00
  <nome>.gles.json ligacoes: {"textures":[[set,binding,nome]], "ubos":[[set,binding,bloco]],
                    "push": tamanho em bytes ou 0}
Usa a variante sem bindless (<nome>.nb.spv) quando existe: o WebGL2 nao tem
descriptor indexing, e o caminho de camadas (TextureLayers) cobre o resto.

Tres diferencas do GLSL ES 3.00 que o texto precisa absorver:
- push constant vira `uniform Struct nome;` solto; reescrito como bloco
  std140 `PushBlock` (o backend copia o push para um UBO). Mesmo layout
  para escalares, vec2/vec4 e mat4; vec3 e arrays mudariam, e o script recusa.
- varyings casam por NOME (no Vulkan, por location): renomeados para
  v_loc<N> nos dois estagios.
- samplers e blocos nao tem binding no texto: o JSON leva set/binding e o
  backend liga pelo nome (glUniform1i / glUniformBlockBinding).
O Z do clip do Vulkan (0..1) vira o do GL (-1..1) com --fixup-clipspace.

Uso: shaders_gles.py <spirv-cross> <pasta shaders> [<saida>]
"""
import json
import os
import re
import subprocess
import sys

STAGES = (".vert.spv", ".frag.spv")
STD140_SAFE = {"float", "int", "uint", "bool", "vec2", "ivec2", "uvec2", "vec4", "ivec4", "uvec4", "mat4"}


def run(cross, args):
    return subprocess.run([cross] + args, check=True, capture_output=True, text=True).stdout


def push_block(text, refl):
    pcs = refl.get("push_constants", [])
    if not pcs:
        return text, 0
    pc = pcs[0]
    t = refl["types"][pc["type"]]
    size = 0
    for m in t["members"]:
        # Array so' de vec4/mat4: stride 16/64 nos dois layouts.
        arr = m.get("array", [1])
        if m["type"] not in STD140_SAFE or (m.get("array") and "4" not in m["type"]):
            raise ValueError(f"push com {m['type']} {m['name']}: layout std140 difere")
        elem = 64 if m["type"] == "mat4" else 16 if "4" in m["type"] else 8 if "2" in m["type"] else 4
        count = 1
        for n in arr:
            count *= n
        size = max(size, m["offset"] + elem * count)
    struct = t["name"]
    var = re.search(r"^uniform %s (\w+);$" % re.escape(struct), text, re.M)
    body = re.search(r"^struct %s\n\{\n(.*?)\n\};\n" % re.escape(struct), text, re.M | re.S)
    if not var or not body:
        raise ValueError("push constant fora do formato esperado do spirv-cross")
    text = text.replace(var.group(0), "")
    text = text.replace(body.group(0), "layout(std140) uniform PushBlock\n{\n%s\n} %s;\n" %
                        (body.group(1), var.group(1)))
    return text, size


def rename_varyings(text, refl, stage):
    key = "outputs" if stage == "vert" else "inputs"
    for io in refl.get(key, []):
        if stage == "frag" and key == "outputs":
            continue
        text = re.sub(r"\b%s\b" % re.escape(io["name"]), "v_loc%d" % io["location"], text)
    return text


def convert(cross, src, dst):
    stage = "vert" if src.endswith(".vert.spv") or src.endswith(".vert.nb.spv") else "frag"
    args = [src, "--es", "--version", "300"]
    if stage == "vert":
        args.append("--fixup-clipspace")
    text = run(cross, args)
    refl = json.loads(run(cross, [src, "--reflect"]))
    text, push = push_block(text, refl)
    text = rename_varyings(text, refl, stage)
    binds = {
        "textures": [[t.get("set", 0), t["binding"], t["name"]] for t in refl.get("textures", [])],
        "ubos": [[u.get("set", 0), u["binding"], refl["types"][u["type"]]["name"]] for u in refl.get("ubos", [])],
        "push": push,
    }
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst + ".gles", "w") as f:
        f.write(text)
    with open(dst + ".gles.json", "w") as f:
        json.dump(binds, f)


def main():
    cross, root = sys.argv[1], sys.argv[2]
    out = sys.argv[3] if len(sys.argv) > 3 else root
    ok, fail = 0, []
    for dirpath, _, files in os.walk(root):
        for name in sorted(files):
            if not name.endswith(STAGES):
                continue
            src = os.path.join(dirpath, name)
            nb = src[:-4] + ".nb.spv"
            if os.path.exists(nb):
                src = nb
            dst = os.path.join(out, os.path.relpath(os.path.join(dirpath, name), root))[:-4]
            try:
                convert(cross, src, dst)
                ok += 1
            except (subprocess.CalledProcessError, ValueError, KeyError) as e:
                msg = e.stderr.strip().splitlines()[-1] if isinstance(e, subprocess.CalledProcessError) else str(e)
                fail.append((name, msg))
    for name, msg in fail:
        print(f"FALHA {name}: {msg}")
    print(f"shaders_gles: {ok} convertidos, {len(fail)} falhas")
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
