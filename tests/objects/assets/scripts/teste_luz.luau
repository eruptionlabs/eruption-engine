-- Luz como objeto: segue o pai, desligar apaga, religar acende, segue lights.*.
local q, poste, luz = 0
-- O módulo vive entre um Play e outro do editor: cada jogo começa do zero.
on.start(function() q, poste, luz = 0, nil, nil end)
local function falha(m) log("OBJ-FALHOU luz: " .. m) end
on.update(function()
    q += 1
    if q == 40 then
        luz = objects.find("light_1")
        if not luz or luz.light ~= 1 then falha("luz como objeto") return end
        poste = objects.create("poste", vector.create(0, 0, 0))
        luz.parent = poste
        poste.position = vector.create(300, 50, 300)
        poste.active = false
    elseif q == 42 and luz then
        if vector.magnitude(lights.position(1) - luz.position) > 0.05 then falha("segue o pai") return end
        if lights.enabled(1) then falha("desligar apaga") return end
        poste.active = true
    elseif q == 44 and luz then
        if not lights.enabled(1) then falha("religar acende") return end
        lights.set_position(1, vector.create(10, 20, 30))
    elseif q == 46 and luz then
        if vector.magnitude(luz.position - vector.create(10, 20, 30)) > 0.05 then falha("segue lights.set_position") return end
        log("OBJ-OK luz")
    end
end)
