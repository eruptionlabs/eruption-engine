-- Nevoa local (world.fog_volume) e nuvem de script com base esfiapada (ragged): a API aceita,
-- cloud_set muda ragged sem refazer a nuvem, e valores fora de 0..1 nao derrubam o script.
local q, nuvem, nevoa = 0
on.start(function() q, nuvem, nevoa = 0, nil, nil end)
local function falha(m) log("OBJ-FALHOU nuvem: " .. m) end
on.update(function()
    q += 1
    if q == 20 then
        local c = vector.create(400, 0, 400)
        nevoa = world.fog_volume({pos = c, radius = 80, height = 40, density = 0.3, duration = 0, grow = 0.1, fade = 0.1})
        if not nevoa then falha("fog_volume sem id") return end
        nuvem = weather.cloud_spawn({preset = "snow", x = 400, z = 400, area_radius = 100, altitude = 150, ragged = 0.8, life = 0, fade_in = 0.1, drift = false, precip = false, climate = false})
        if not nuvem then falha("cloud_spawn sem id") return end
    elseif q == 30 and nuvem then
        weather.cloud_set(nuvem, {ragged = 0.2})
        weather.cloud_set(nuvem, {ragged = 7})
    elseif q == 40 and nuvem then
        weather.cloud_remove(nuvem)
        log("OBJ-OK nuvem")
    end
end)
