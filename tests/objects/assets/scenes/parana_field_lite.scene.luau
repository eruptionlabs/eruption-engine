-- Declarações que os testes "cena" (pai, etiquetas e script de objeto) e "faixa" conferem.
return {
    added = {
        { prefab = "assets/prefabs/carroca.prefab.luau", name = "carroca_cena", position = {500, 40, 500}, rotation = {0, 0, 0}, scale = {1, 1, 1} },
    },
    objects = {
        ["namaqualand_boulder_04_1130"] = { parent = "namaqualand_boulder_06_1129", tags = {"cena", "pedra"}, scripts = {"comportamentos/contar"} },
    },
    characters = {
        { name = "assenta_1", sprite = "assets/sprites/default.png", position = {300, 0, 300}, facing = 0, ground = true },
        { name = "assenta_2", sprite = "assets/sprites/default.png", position = {900, 2000, 600}, facing = 0, ground = true },
        { name = "assenta_3", sprite = "assets/sprites/default.png", position = {600, 0, 1200}, facing = 0, ground = true },
    },
    ribbons = {
        { name = "anel_cena", closed = true, step = 4, uv_length = 32, lift = 0.100000001, width = 30,
          points = { {650, 500}, {500, 650}, {350, 500}, {500, 350} } },
    },
}
