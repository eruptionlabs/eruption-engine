"""Compila um plugin de Unreal Engine (codigo-fonte) contra os cabecalhos
de compatibilidade do Eruption (compat/unreal/include), gerando uma .so por
modulo que o host de compat carrega.

Faz o papel minimo de gerador de reflexao, com codigo proprio:
  * para cada cabecalho com GENERATED_BODY, escreve <Nome>.generated.h com a
    macro por arquivo e linha (Super/ThisClass), no esquema publico
    CURRENT_FILE_ID + __LINE__;
  * para cada UFUNCTION estatica com tipos simples, gera um "thunk" que o
    host chama pelo nome ("Classe.Funcao") com argumentos em texto.

    python3 compilar_plugin.py <Plugin.uplugin> <pasta_saida> [--cxx g++]
        [--forcar-plataforma] [--so-fontes Arquivo.cpp,Outro.cpp]

--forcar-plataforma ignora PlatformAllowList (para testar so' parte de um
plugin restrito a outra plataforma, junto com --so-fontes).

Saida: <pasta_saida>/<Plugin>/<Plugin>.uplugin e
       <pasta_saida>/<Plugin>/Binaries/EruptionCompat/lib<Modulo>.so
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys

AQUI = os.path.dirname(os.path.abspath(__file__))
INCLUDE_COMPAT = os.path.normpath(os.path.join(AQUI, "..", "include"))

# Tipos de parametro/retorno que o thunk sabe converter de/para texto.
TIPOS_SIMPLES = {"float", "double", "int32", "int64", "uint8", "int", "bool", "FString", "FName", "FVector", "void"}
MODULOS_SO_EDITOR = {"Editor", "EditorNoCommandlet", "Developer", "DeveloperTool", "UncookedOnly", "Program"}


def log(msg):
    print("[compilar_plugin] " + msg)


def ler_uplugin(caminho):
    with open(caminho, encoding="utf-8-sig") as f:
        return json.load(f)


def plataforma_permitida(mod):
    permitidas = mod.get("PlatformAllowList") or mod.get("WhitelistPlatforms")
    negadas = mod.get("PlatformDenyList") or mod.get("BlacklistPlatforms") or []
    if permitidas is not None and "Linux" not in permitidas:
        return False, "restrito a " + ",".join(permitidas)
    if "Linux" in negadas:
        return False, "negado para Linux"
    return True, ""


def sem_comentarios(texto):
    # Remove comentarios preservando as quebras de linha (numeros de linha valem).
    def troca(m):
        return re.sub(r"[^\n]", " ", m.group(0))
    texto = re.sub(r"/\*.*?\*/", troca, texto, flags=re.S)
    return re.sub(r"//[^\n]*", troca, texto)


def fechar_parenteses(texto, ini):
    """Indice logo apos o ')' que fecha o '(' em texto[ini]."""
    nivel = 0
    for i in range(ini, len(texto)):
        if texto[i] == "(":
            nivel += 1
        elif texto[i] == ")":
            nivel -= 1
            if nivel == 0:
                return i + 1
    return len(texto)


def dividir_topo(s, sep=","):
    partes, nivel, cur = [], 0, ""
    for c in s:
        if c in "(<[{":
            nivel += 1
        elif c in ")>]}":
            nivel -= 1
        if c == sep and nivel == 0:
            partes.append(cur)
            cur = ""
        else:
            cur += c
    if cur.strip():
        partes.append(cur)
    return partes


RE_CLASSE = re.compile(r"\b(class|struct)\s+(?:[A-Z0-9_]+_API\s+)?(\w+)\s*(?:final\s*)?(?::\s*(?:public|protected|private)?\s*([\w:]+))?\s*\{")


def analisar_cabecalho(caminho):
    """Classes com GENERATED_BODY, UFUNCTIONs estaticas e UENUMs."""
    texto = sem_comentarios(open(caminho, encoding="utf-8", errors="replace").read())
    classes = []  # (inicio, fim, tipo, nome, pai)
    for m in RE_CLASSE.finditer(texto):
        ini = m.end() - 1
        nivel, fim = 0, len(texto)
        for i in range(ini, len(texto)):
            if texto[i] == "{":
                nivel += 1
            elif texto[i] == "}":
                nivel -= 1
                if nivel == 0:
                    fim = i
                    break
        classes.append((ini, fim, m.group(1), m.group(2), m.group(3)))

    def classe_em(pos):
        dentro = [c for c in classes if c[0] <= pos <= c[1]]
        return max(dentro, key=lambda c: c[0]) if dentro else None

    corpos = []
    for m in re.finditer(r"\b(GENERATED_BODY|GENERATED_UCLASS_BODY|GENERATED_USTRUCT_BODY)\s*\(", texto):
        linha = texto.count("\n", 0, m.start()) + 1
        c = classe_em(m.start())
        if c:
            corpos.append((linha, m.group(1), c[2], c[3], c[4]))

    enums = {}
    for m in re.finditer(r"UENUM\s*\([^)]*\)\s*enum\s+(?:class\s+)?(\w+)\s*(?::\s*\w+)?\s*\{([^}]*)\}", texto):
        nomes = []
        for item in dividir_topo(re.sub(r"UMETA\s*\([^)]*\)", "", m.group(2))):
            nome = item.split("=")[0].strip()
            if nome:
                nomes.append(nome)
        enums[m.group(1)] = nomes

    funcoes = []
    for m in re.finditer(r"UFUNCTION\s*\(", texto):
        depois = fechar_parenteses(texto, m.end() - 1)
        dm = re.match(r"\s*static\s+([\w:<>\s\*&]+?)\s+(\w+)\s*\(", texto[depois:])
        if not dm:
            continue
        fim_args = fechar_parenteses(texto, depois + dm.end() - 1)
        args_txt = texto[depois + dm.end():fim_args - 1]
        c = classe_em(m.start())
        if not c:
            continue
        params = []
        for p in dividir_topo(args_txt):
            p = re.sub(r"UPARAM\s*\([^)]*\)", "", p).strip()
            padrao = None
            if "=" in p:
                p, padrao = p.split("=", 1)
                padrao = padrao.strip()
            p = p.strip()
            nm = re.match(r"(.*?)(\w+)\s*$", p)
            tipo = nm.group(1) if nm else p
            tipo = re.sub(r"\bconst\b|&", "", tipo).strip()
            params.append((tipo, padrao))
        ret = re.sub(r"\bconst\b|&", "", dm.group(1)).strip()
        funcoes.append((c[3], dm.group(2), ret, params))
    return corpos, funcoes, enums


def escrever_generated(cabecalho, pasta_gen, raiz_src):
    corpos, funcoes, enums = analisar_cabecalho(cabecalho)
    if not corpos and not re.search(r"\bU(CLASS|STRUCT|ENUM)\s*\(", open(cabecalho, encoding="utf-8", errors="replace").read()):
        return [], {}
    base = os.path.splitext(os.path.basename(cabecalho))[0]
    rel = os.path.relpath(cabecalho, raiz_src)
    file_id = "EUE_" + re.sub(r"\W", "_", rel)
    linhas = [
        "// GERADO por compat/unreal/ferramentas/compilar_plugin.py - nao editar.",
        "#pragma once",
        "#undef CURRENT_FILE_ID",
        "#define CURRENT_FILE_ID " + file_id,
    ]
    for linha, macro, tipo, nome, pai in corpos:
        if tipo == "struct":
            corpo = "typedef %s ThisClass;" % nome
            if pai:
                corpo += " typedef %s Super;" % pai
        else:
            corpo = "public: typedef %s ThisClass;" % nome
            if pai:
                corpo += " typedef %s Super;" % pai
            if macro == "GENERATED_UCLASS_BODY":
                corpo += " %s(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());" % nome
                corpo += " public:"
            else:
                corpo += " private:"
        sufixo = "_GENERATED_BODY_LEGACY" if macro == "GENERATED_UCLASS_BODY" else "_GENERATED_BODY"
        linhas.append("#define %s_%d%s %s" % (file_id, linha, sufixo, corpo))
    open(os.path.join(pasta_gen, base + ".generated.h"), "w", encoding="utf-8").write("\n".join(linhas) + "\n")
    return funcoes, enums


def conversor(tipo, enums):
    if tipo in enums:
        return "EUE_Enum_%s" % tipo
    return "::EruptionUE::Conv<%s>::De" % tipo


def gerar_thunks(modulo, cabecalhos_funcoes, enums, pasta_gen):
    out = [
        "// GERADO por compat/unreal/ferramentas/compilar_plugin.py - nao editar.",
        '#include "EruptionUE/Conversao.h"',
    ]
    for cab in sorted(cabecalhos_funcoes):
        out.append('#include "%s"' % cab)
    out.append("namespace {")
    for nome_enum, itens in enums.items():
        out.append("%s EUE_Enum_%s(const char* s) {" % (nome_enum, nome_enum))
        for item in itens:
            out.append('    if (std::strcmp(s, "%s") == 0) return %s::%s;' % (item, nome_enum, item))
        out.append("    return static_cast<%s>(std::atoi(s));" % nome_enum)
        out.append("}")
    n = 0
    registradas = []
    for cab, funcoes in sorted(cabecalhos_funcoes.items()):
        for classe, nome, ret, params in funcoes:
            tipos = [t for t, _ in params]
            if any(t not in TIPOS_SIMPLES and t not in enums for t in tipos + [ret]):
                log("  sem thunk (tipo complexo): %s.%s" % (classe, nome))
                continue
            minimo = sum(1 for _, d in params if d is None)
            corpo = []
            corpo.append("bool EUE_Thunk_%d(const char* const* a, int n, char* saida, int tam) {" % n)
            corpo.append("    if (n < %d || n > %d) return false;" % (minimo, len(params)))
            chamada = []
            for i, (tipo, padrao) in enumerate(params):
                conv = conversor(tipo, enums)
                if padrao is None:
                    corpo.append("    %s p%d = %s(a[%d]);" % (tipo, i, conv, i))
                else:
                    corpo.append("    %s p%d = n > %d ? %s(a[%d]) : %s(%s);" % (tipo, i, i, conv, i, tipo, padrao))
                chamada.append("p%d" % i)
            expr = "%s::%s(%s)" % (classe, nome, ", ".join(chamada))
            if ret == "void":
                corpo.append("    %s;" % expr)
                corpo.append('    std::snprintf(saida, static_cast<size_t>(tam), "(void)");')
            elif ret in enums:
                corpo.append('    std::snprintf(saida, static_cast<size_t>(tam), "%%d", static_cast<int>(%s));' % expr)
            else:
                corpo.append("    const std::string r = ::EruptionUE::Conv<%s>::Para(%s);" % (ret, expr))
                corpo.append('    std::snprintf(saida, static_cast<size_t>(tam), "%s", r.c_str());')
            corpo.append("    return true;")
            corpo.append("}")
            assinatura = "%s(%s)" % (ret, ",".join(tipos))
            corpo.append('::EruptionUE::FRegistroFuncao EUE_Reg_%d("%s.%s", "%s", &EUE_Thunk_%d);' % (n, classe, nome, assinatura, n))
            out.extend(corpo)
            registradas.append("%s.%s" % (classe, nome))
            n += 1
    out.append("} // namespace")
    caminho = os.path.join(pasta_gen, modulo + ".eruption.gen.cpp")
    open(caminho, "w", encoding="utf-8").write("\n".join(out) + "\n")
    return caminho, registradas


def compilar_modulo(plugin_dir, mod, saida_bin, cxx, so_fontes=None):
    nome = mod["Name"]
    src = os.path.join(plugin_dir, "Source", nome)
    if not os.path.isdir(src):
        log("modulo %s: pasta %s nao existe" % (nome, src))
        return False
    pasta_gen = os.path.join(saida_bin, "..", "..", "Gerado", nome)
    os.makedirs(pasta_gen, exist_ok=True)
    fontes, cab_funcoes, enums = [], {}, {}
    for raiz, dirs, arquivos in os.walk(src):
        dirs[:] = [d for d in dirs if d not in ("Intermediate", "Binaries")]
        for a in arquivos:
            caminho = os.path.join(raiz, a)
            if a.endswith(".cpp"):
                if not so_fontes or a in so_fontes:
                    fontes.append(caminho)
            elif a.endswith(".h"):
                funcoes, en = escrever_generated(caminho, pasta_gen, src)
                enums.update(en)
                if funcoes:
                    cab_funcoes[os.path.basename(caminho)] = funcoes
    gen_cpp, registradas = gerar_thunks(nome, cab_funcoes, enums, pasta_gen)
    fontes.append(gen_cpp)
    incs = [INCLUDE_COMPAT, pasta_gen, src]
    for sub in ("Public", "Private", "Classes"):
        if os.path.isdir(os.path.join(src, sub)):
            incs.append(os.path.join(src, sub))
    os.makedirs(saida_bin, exist_ok=True)
    alvo = os.path.join(saida_bin, "lib%s.so" % nome)
    cmd = [cxx, "-std=gnu++17", "-O2", "-fPIC", "-shared", "-fvisibility=hidden", "-w",
           "-D%s_API=" % nome.upper(), "-o", alvo]
    cmd += ["-I" + i for i in incs] + sorted(fontes)
    log("modulo %s: %d fonte(s), %d funcao(oes) refletida(s)" % (nome, len(fontes), len(registradas)))
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write(r.stderr[-4000:])
        log("modulo %s: FALHOU a compilacao" % nome)
        return False
    log("modulo %s: %s" % (nome, alvo))
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("uplugin")
    ap.add_argument("saida")
    ap.add_argument("--cxx", default=os.environ.get("CXX", "g++"))
    ap.add_argument("--forcar-plataforma", action="store_true")
    ap.add_argument("--so-fontes", default="")
    args = ap.parse_args()
    up = ler_uplugin(args.uplugin)
    plugin_dir = os.path.dirname(os.path.abspath(args.uplugin))
    nome_plugin = os.path.splitext(os.path.basename(args.uplugin))[0]
    destino = os.path.join(args.saida, nome_plugin)
    bin_dir = os.path.join(destino, "Binaries", "EruptionCompat")
    ok, total = 0, 0
    for mod in up.get("Modules", []):
        tipo = mod.get("Type", "Runtime")
        if tipo in MODULOS_SO_EDITOR:
            log("modulo %s pulado: tipo %s (so' editor)" % (mod["Name"], tipo))
            continue
        permitido, motivo = plataforma_permitida(mod)
        if not permitido and args.forcar_plataforma:
            log("modulo %s: %s, compilando mesmo assim (--forcar-plataforma)" % (mod["Name"], motivo))
            permitido = True
        if not permitido:
            log("modulo %s pulado: %s" % (mod["Name"], motivo))
            continue
        total += 1
        so_fontes = set(f for f in args.so_fontes.split(",") if f)
        ok += 1 if compilar_modulo(plugin_dir, mod, bin_dir, args.cxx, so_fontes) else 0
    log("%s: %d de %d modulo(s) compilado(s)" % (nome_plugin, ok, total))
    # O descritor vai junto dos binarios: e' ele que o host le para carregar.
    if ok:
        shutil.copy(args.uplugin, os.path.join(destino, nome_plugin + ".uplugin"))
    return 0 if total and ok == total else 1


if __name__ == "__main__":
    sys.exit(main())
