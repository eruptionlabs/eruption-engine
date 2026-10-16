-- Declarações que o teste "cena" confere (pai, etiquetas e script de objeto).
return {
    added = {
        { prefab = "assets/prefabs/carroca.prefab.luau", name = "carroca_cena", position = {500, 40, 500}, rotation = {0, 0, 0}, scale = {1, 1, 1} },
    },
    objects = {
        ["namaqualand_boulder_04_1130"] = { parent = "namaqualand_boulder_06_1129", tags = {"cena", "pedra"}, scripts = {"comportamentos/contar"} },
    },
}
