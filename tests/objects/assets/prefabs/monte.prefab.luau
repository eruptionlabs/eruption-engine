-- Prefab composto: raiz sem modelo, etiqueta, script, dois nós e um prefab dentro.
return {
    tags = {"monte"},
    scripts = {"comportamentos/contar"},
    children = {
        { file = "assets/external/parana_field_lite/parana_field_lite.glb", node = "namaqualand_boulder_04_1132", name = "a", position = {-40, 0, 0} },
        { file = "assets/external/parana_field_lite/parana_field_lite.glb", node = "namaqualand_boulder_04_1132", name = "b", position = {40, 0, 0}, rotation = {0, 90, 0} },
        { prefab = "assets/prefabs/pedrinha.prefab.luau", name = "topo", position = {0, 30, 0} },
    },
}
