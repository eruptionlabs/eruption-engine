"""Gera compat/godot/GdApiGerado.inc a partir do extension_api.json do
godot-cpp (MIT), que descreve a API publica de classes do Godot.

Do JSON so' saem fatos de interface: hierarquia de classes (nome, pai,
se e' RefCounted), singletons e os hashes dos metodos virtuais que o host
chama (o godot-cpp so' devolve o virtual quando o hash confere).

    python3 gera_godot_api.py <extension_api.json> <saida.inc>
"""
import json
import sys

# Classes cujos virtuais o host chama (ciclo de vida de no').
VIRTUAIS_DE = ("Node",)


def main(entrada, saida):
    api = json.load(open(entrada, encoding="utf-8"))
    cab = api["header"]
    linhas = [
        "// GERADO por compat/scripts/gera_godot_api.py - nao editar a mao.",
        "// Fonte: extension_api.json do godot-cpp (MIT), %s." % cab["version_full_name"],
        "// clang-format off",
        "#define ERUPTION_GD_API_MAJOR %d" % cab["version_major"],
        "#define ERUPTION_GD_API_MINOR %d" % cab["version_minor"],
        "#define ERUPTION_GD_API_PATCH %d" % cab["version_patch"],
        "",
        "// { classe, pai, e' RefCounted }",
        "static const GdClasseApi kClassesApi[] = {",
    ]
    for c in sorted(api["classes"], key=lambda c: c["name"]):
        linhas.append('    {"%s", "%s", %s},' % (c["name"], c.get("inherits", ""),
                                                "true" if c.get("is_refcounted") else "false"))
    linhas.append("};")
    linhas.append("")
    linhas.append("// { classe, metodo virtual, hash }")
    linhas.append("static const GdVirtualApi kVirtuaisApi[] = {")
    classes = {c["name"]: c for c in api["classes"]}
    for nome in VIRTUAIS_DE:
        for m in classes[nome].get("methods", []):
            if m.get("is_virtual"):
                linhas.append('    {"%s", "%s", %du},' % (nome, m["name"], m["hash"]))
    linhas.append("};")
    linhas.append("")
    linhas.append("static const char* const kSingletonsApi[] = {")
    for s in api["singletons"]:
        linhas.append('    "%s",' % s["name"])
    linhas.append("};")
    linhas.append("// clang-format on")
    open(saida, "w", encoding="utf-8").write("\n".join(linhas) + "\n")
    print("gerado:", saida, len(api["classes"]), "classes")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
