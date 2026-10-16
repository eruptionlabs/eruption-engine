-- Script por objeto: start, update com estado em self.data, destruir a si mesmo.
local obj, vazio
on.start(function()
    obj = objects.spawn("assets/external/parana_field_lite/parana_field_lite.glb#namaqualand_boulder_04_1134", vector.create(0, 0, 0), nil, "girando")
    obj:attach("comportamentos/sumir")
    vazio = objects.create("vazio_com_script")
    vazio:attach("comportamentos/contar")
end)
local q = 0
-- O módulo vive entre um Play e outro do editor: cada jogo começa do zero.
on.start(function() q = 0 end)
on.update(function()
    q += 1
    if q == 2 and obj.data.comecou ~= true then log("OBJ-FALHOU comportamento: start") end
    if q == 10 then
        if obj:alive() then log("OBJ-FALHOU comportamento: destruir a si mesmo") return end
        if (vazio.data.n or 0) < 5 then log("OBJ-FALHOU comportamento: update") return end
        log("OBJ-OK comportamento")
    end
end)
