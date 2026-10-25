-- Os personagens assenta_* da cena (ground = true, Y 0 ou 2000) assentam no piso; set_position(..., true)
-- anima o andar e um salto grande (teleporte) não anima.
local q = 0
on.start(function() q = 0; falhou = false end)
local falhou = false
local function falha(m) falhou = true; log("OBJ-FALHOU andar_script: " .. m) end
local pontos = { vector.create(300, 0, 300), vector.create(900, 0, 600), vector.create(600, 0, 1200) }
local pos = vector.create(0, 0, 0)
on.update(function()
    if falhou then return end
    q += 1
    if q == 53 then
        for i, p in pontos do
            local y = characters.position("assenta_" .. i).y
            local piso = world.floor_height(p.x, p.z, y + 4)
            if math.abs(y - piso) > 0.05 then falha(`assenta_{i}: y {y} piso {piso}`) return end
        end
        characters.add("andarilho", "assets/sprites/default.png", vector.create(500, 0, 500))
        pos = characters.position("andarilho")
    elseif q > 53 and q <= 80 then
        pos += vector.create(1, 0, 0)
        characters.set_position("andarilho", pos, true)
        if q == 60 and not characters.walking("andarilho") then falha("set_position com andar não animou") return end
    elseif q == 81 then
        characters.set_position("andarilho", pos + vector.create(500, 0, 0), true)
    elseif q == 82 then
        if characters.walking("andarilho") then falha("teleporte animou") return end
        characters.set_position("andarilho", pos + vector.create(501, 0, 0))
    elseif q == 84 then
        if characters.walking("andarilho") then falha("set_position sem andar animou") return end
        log("OBJ-OK andar_script")
    end
end)
