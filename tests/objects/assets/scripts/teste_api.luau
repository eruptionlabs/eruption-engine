-- API objects: achar, mexer, hierarquia, etiquetas, dados, destruir, estoque.
local NOME = "api"
local falhas = 0
local function confere(ok, oque) if not ok then falhas += 1; log("OBJ-FALHOU " .. NOME .. ": " .. oque) end end
local function perto(a, b) return vector.magnitude(a - b) < 0.05 end
local FONTE = "assets/external/parana_field_lite/parana_field_lite.glb#namaqualand_boulder_04_1128"
local pedra, idx, p0, grupo, filho, mexido, modelosAntes

on.start(function()
    confere(objects.count() > 8000, "objetos do mapa antes do primeiro quadro: " .. objects.count())
    pedra = objects.find("namaqualand_boulder_06_1133")
    confere(pedra ~= nil, "achar modelo do mapa")
    if not pedra then return end
    idx = models.find("namaqualand_boulder_06_1133")
    confere(pedra.model == idx and objects.of_model(idx) == pedra, "objeto <-> models.*")
    p0 = pedra.position
    pedra.position = p0 + vector.create(0, 50, 0)            -- antes do primeiro sync
    grupo = objects.create("grupo_api", p0 + vector.create(0, 0, 100))
    filho = objects.spawn(FONTE, p0 + vector.create(30, 0, 100), grupo, "filho_api")
    confere(filho ~= nil and filho.parent == grupo, "spawn dentro do grupo")
    if not filho then return end
    confere(perto(filho.local_position, vector.create(30, 0, 0)), "posição local")
    grupo.position = grupo.position + vector.create(0, 20, 0)
    confere(perto(filho.position, p0 + vector.create(30, 20, 100)), "filho segue o pai")
    filho:add_tag("quebravel")
    filho.data.vida = 3
    confere(filho:has_tag("quebravel") and #objects.with_tag("quebravel") == 1 and filho.data.vida == 3, "etiqueta e dados")
    confere(grupo:find("filho_api") == filho and objects.find("grupo_api/filho_api") == filho, "caminho")
    confere(not pcall(function() grupo.parent = filho end), "ciclo recusado")
    mexido = objects.spawn(FONTE, p0, nil, "mexido_api")
    mexido.position = p0 + vector.create(0, 0, -200)          -- mesmo quadro do spawn
    filho:destroy()
    confere(not filho:alive() and not pcall(function() return filho.position end), "destruído")
end)

local q = 0
-- O módulo vive entre um Play e outro do editor: cada jogo começa do zero.
on.start(function() q = 0; falhas = 0 end)
on.update(function()
    q += 1
    if not pedra or not mexido then return end
    if q == 3 then
        confere(perto(models.position(idx), pedra.position), "modelo seguiu o objeto")
        confere(perto(models.position(mexido.model), p0 + vector.create(0, 0, -200)), "mexer no quadro do spawn")
        models.set_position(idx, p0 + vector.create(10, 0, 0))
    elseif q == 5 then
        confere(perto(pedra.position, p0 + vector.create(10, 0, 0)), "objeto seguiu models.set_position")
        -- 50 criados e depois destruídos: os 50 seguintes só reaproveitam
        local antes = {}
        for i = 1, 50 do antes[i] = objects.spawn(FONTE, vector.create(i, 0, 0)) end
        for i = 1, 50 do antes[i]:destroy() end
        modelosAntes = models.count()
        local lote = {}
        for i = 1, 50 do lote[i] = objects.spawn(FONTE, vector.create(i, 0, 0)) end
        confere(models.count() == modelosAntes, "estoque: " .. modelosAntes .. " -> " .. models.count())
        for i = 1, 50 do lote[i]:destroy() end
        -- Renomear pelo jogo: o Stop do editor devolve o nome (o 2º Play acha).
        pedra.name = "renomeada_api"
        confere(objects.find("renomeada_api") == pedra, "renomear")
        log(falhas == 0 and ("OBJ-OK " .. NOME) or ("OBJ-FALHOU " .. NOME .. ": " .. falhas .. " falha(s)"))
    end
end)
