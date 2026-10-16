-- Jogo salvo: objetos de script (pai, transform, etiqueta, dados, script),
-- dados de objeto do mapa, Object em vars, objeto do mapa destruído/desligado.
local q, falhas = 0, 0
-- O módulo vive entre um Play e outro do editor: cada jogo começa do zero.
on.start(function() q, falhas = 0, 0 end)
local function falha(m) falhas += 1; log("OBJ-FALHOU salvar: " .. m) end
local FONTE = "assets/external/parana_field_lite/parana_field_lite.glb#namaqualand_boulder_04_1136"
on.update(function()
    q += 1
    if q == 20 then
        local caixa = objects.create("caixa_salva", vector.create(100, 0, 100))
        local pedra = objects.spawn(FONTE, vector.create(120, 0, 100), caixa, "pedra_salva")
        pedra:add_tag("tesouro")
        pedra:attach("comportamentos/contar")
        pedra.data.ouro = 42
        pedra.data.dono = caixa
        local mapa = objects.find("namaqualand_boulder_06_1131")
        mapa.data.visitada = true
        mapa:add_tag("marco")
        vars.set("alvo", pedra)
        -- Nomes repetidos: cada um volta com os próprios dados.
        local i1 = objects.create("inimigo", vector.create(0, 0, 0))
        local i2 = objects.create("inimigo", vector.create(10, 0, 0))
        i1.data.vida, i2.data.vida = 1, 2
        vars.set("segundo", i2)
        if not game.save("teste_objetos") then falha("save") end
        i1:destroy()
        i2:destroy()
        pedra:destroy()
        mapa:destroy()
        objects.find("namaqualand_boulder_06_1137").active = false
    elseif q == 22 then
        if not game.load("teste_objetos") then falha("load") end
    elseif q == 24 then
        local pedra = objects.find("caixa_salva/pedra_salva")
        if not pedra then falha("objeto de script voltou") return end
        if pedra.data.ouro ~= 42 or pedra.data.dono ~= objects.find("caixa_salva") then falha("dados") end
        if not pedra:has_tag("tesouro") then falha("etiqueta") end
        if vector.magnitude(pedra.position - vector.create(120, 0, 100)) > 0.05 then falha("posição") end
        local mapa = objects.find("namaqualand_boulder_06_1131")
        if not mapa or mapa.data.visitada ~= true or not mapa:has_tag("marco") then falha("objeto do mapa destruído depois de salvar") end
        if vars.get("alvo") ~= pedra then falha("Object em vars") end
        local outros = objects.find_all("inimigo")
        local segundo = objects.find("inimigo#1")
        if #outros ~= 2 or not segundo or segundo.data.vida ~= 2 or objects.find("inimigo").data.vida ~= 1 then
            falha("nomes repetidos")
        end
        if vars.get("segundo") ~= segundo then falha("Object em vars com nome repetido") end
        if not objects.find("namaqualand_boulder_06_1137").active then falha("objeto desligado depois de salvar") end
        vars.set("n_depois", pedra.data.n)
    elseif q == 28 then
        local pedra = objects.find("caixa_salva/pedra_salva")
        if not pedra or (pedra.data.n or 0) <= (vars.get("n_depois") or 0) then falha("script de objeto voltou") end
        log(falhas == 0 and "OBJ-OK salvar" or "OBJ-FALHOU salvar: fim")
    end
end)
