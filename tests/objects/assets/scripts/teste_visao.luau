-- Visao de equipe: regra do vision.visible (fontes, equipe, zonas block e
-- reveal) e a volta ao padrao. Termina com a visao LIGADA de proposito: no
-- editor, o segundo Play so' passa se o Stop a desligou.
local q = 0
on.start(function() q = 0 end)
local function falha(m) log("OBJ-FALHOU visao: " .. m) end
local function v(x, z) return vector.create(x, 0, z) end
on.update(function()
    q += 1
    if q ~= 30 then return end
    if vision.enabled() then falha("comecou ligada (o Stop nao desligou?)") return end
    if not vision.visible(v(9000, 9000)) then falha("desligada esconde") return end

    vision.enable(true, { dim = 0.4 })
    if vision.visible(v(500, 500)) then falha("ligada e sem fonte ve") return end
    vision.add_source("heroi", v(500, 500), 40)
    if not vision.visible(v(539, 500)) or vision.visible(v(541, 500)) then falha("raio da fonte") return end
    vision.add_source(7, v(800, 500), 40, 2)
    if vision.visible(v(800, 500)) then falha("fonte de outra equipe conta") return end
    vision.add_source("heroi", v(600, 500), 40)
    if vision.visible(v(500, 500)) or not vision.visible(v(600, 500)) then falha("mesmo id nao moveu") return end

    vision.zone("covil", v(640, 500), 20)
    if vision.visible(v(640, 500)) then falha("block: fonte de fora ve dentro") return end
    vision.add_source("sentinela", v(645, 500), 10)
    if not vision.visible(v(640, 500)) then falha("block: fonte de dentro nao ve") return end
    if not vision.remove_source("sentinela") or vision.remove_source("sentinela") then falha("remove_source") return end
    vision.zone("praca", v(300, 300), 50, { mode = "reveal" })
    if not vision.visible(v(310, 300)) then falha("reveal sem fonte") return end
    if not vision.remove_zone("praca") or vision.visible(v(310, 300)) then falha("remove_zone") return end
    if pcall(vision.zone, "x", v(0, 0), 5, { mode = "nevoa" }) then falha("modo invalido aceito") return end
    -- Efeito de cenario: a visao so' escurece; as duas formas de passar a opcao.
    local ok, id = pcall(vfx.play, "magma_vapor", v(900, 900), 1, { vision = false })
    if not ok or id == 0 then falha("vfx.play com {vision = false} depois da escala: " .. tostring(id)) return end
    vfx.stop(id, true)
    id = vfx.play("magma_vapor", v(900, 900), { scale = 1, vision = false })
    if id == 0 then falha("vfx.play com vision na tabela") return end
    vfx.stop(id, true)
    log("OBJ-OK visao")
end)
