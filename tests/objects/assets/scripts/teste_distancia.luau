-- world.set_load_distance / world.load_distance: o valor vale e e' limitado
-- (50..3000 m), e o padrao volta ao fim para nao deixar vizinhos carregando
-- de mais nos outros testes.
local q = 0
on.start(function() q = 0 end)
local function falha(m) log("OBJ-FALHOU distancia: " .. m) end
on.update(function()
    q += 1
    if q ~= 10 then return end
    local padrao = world.load_distance()
    world.set_load_distance(900)
    if world.load_distance() ~= 900 then falha("900 virou " .. world.load_distance()) return end
    world.set_load_distance(1)
    if world.load_distance() ~= 50 then falha("1 virou " .. world.load_distance() .. " (piso 50)") return end
    world.set_load_distance(1e9)
    if world.load_distance() ~= 3000 then falha("1e9 virou " .. world.load_distance() .. " (teto 3000)") return end
    world.set_load_distance(padrao)
    log(string.format("OBJ-OK distancia (padrao %.0f)", padrao))
end)
