-- Prefab composto posto na cena: raiz com modelo, dois filhos, etiqueta e script.
return {
    file = "assets/external/parana_field_lite/parana_field_lite.glb",
    node = "namaqualand_boulder_06_1141",
    tags = {"carroca"},
    scripts = {"comportamentos/contar"},
    children = {
        { prefab = "assets/prefabs/pedrinha.prefab.luau", name = "roda_e", position = {-30, 0, 0} },
        { prefab = "assets/prefabs/pedrinha.prefab.luau", name = "roda_d", position = {30, 0, 0} },
    },
}
