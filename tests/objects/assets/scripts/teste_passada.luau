-- characters.set_stride: aceita nome ou nil, e o andarilho com passada anda sem erro.
-- (A conta do atraso = passada / velocidade / quadros e' conferida com
-- ERUPTION_TEST_SPRITE_TRACE: ver o comentario em Characters.cpp, drawCharacter.)
local q = 0
local pos = vector.create(0, 0, 0)
on.start(function() q = 0 end)
on.update(function()
    q += 1
    if q == 20 then
        characters.add("passante", "assets/sprites/andador.spr", vector.create(1500, 0, 500))
        pos = characters.position("passante")
        characters.set_stride(nil, 10)
        characters.set_stride("passante", 5)
        characters.set_walk_rate("passante", 5, 150)
    elseif q > 20 and q <= 50 then
        pos += vector.create(0.5, 0, 0)
        characters.set_position("passante", pos, true)
    elseif q == 51 then
        characters.set_stride("passante", 0)
        characters.set_stride(nil, 0)
        characters.set_walk_rate("passante", 0)
        log("OBJ-OK passada")
    end
end)
