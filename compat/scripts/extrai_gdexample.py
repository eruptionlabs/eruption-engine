"""Extrai os arquivos do exemplo GDExample do tutorial de GDExtension em C++
da documentacao oficial (texto CC-BY 3.0, baixado por baixar_testes.sh).

Pega cada bloco ``.. code-block:: cpp`` com ``:caption: .../src/<arquivo>`` e
grava <arquivo> em <saida>. So' a primeira ocorrencia de cada nome conta (a
versao basica do tutorial, antes das propriedades e sinais).

    python3 extrai_gdexample.py <tutorial.rst> <pasta_saida>
"""
import os
import sys


def extrai(rst, saida):
    linhas = open(rst, encoding="utf-8").read().splitlines()
    os.makedirs(saida, exist_ok=True)
    gravados = set()
    i = 0
    while i < len(linhas):
        if linhas[i].startswith(".. code-block:: cpp") and i + 1 < len(linhas) and ":caption:" in linhas[i + 1]:
            nome = linhas[i + 1].split(":caption:")[1].strip().split("/")[-1]
            i += 2
            corpo = []
            while i < len(linhas) and (linhas[i].startswith("    ") or not linhas[i].strip()):
                corpo.append(linhas[i][4:])
                i += 1
            while corpo and not corpo[0].strip():
                corpo.pop(0)
            if nome not in gravados:
                with open(os.path.join(saida, nome), "w", encoding="utf-8") as f:
                    f.write("\n".join(corpo).rstrip() + "\n")
                gravados.add(nome)
            continue
        i += 1
    print("extraidos:", ", ".join(sorted(gravados)))
    return len(gravados)


if __name__ == "__main__":
    sys.exit(0 if extrai(sys.argv[1], sys.argv[2]) >= 4 else 1)
