-- Mapa que muda durante o jogo: custo por celula da grade de caminhada
-- (world.set_walk_cost). Vale no mesmo quadro: a consulta logo depois da
-- escrita ja' ve a mudanca.
local q = 0
on.start(function() q = 0 end)
local function falha(m) log("OBJ-FALHOU terreno_vivo: " .. m) end

-- Algum ponto andavel perto de (x, z), numa espiral de passo 10.
local function andavel(x, z)
    for r = 0, 200, 10 do
        for a = 0, 7 do
            local px, pz = x + r * math.cos(a * math.pi / 4), z + r * math.sin(a * math.pi / 4)
            if world.walkable(px, pz) then return px, pz end
        end
    end
    return nil
end

-- Todo trecho do caminho passa so' por celula de custo 1?
local function caminhoBarato(path)
    for i = 1, #path - 1 do
        local a, b = path[i], path[i + 1]
        local n = math.max(1, math.ceil(vector.magnitude(b - a) / 2))
        for k = 0, n do
            local p = a + (b - a) * (k / n)
            if world.walk_cost(p.x, p.z) > 1 then return false end
        end
    end
    return true
end

on.update(function()
    q += 1
    if q ~= 40 then return end
    local x, z = andavel(1500, 1500)
    if not x then falha("nenhum ponto andavel perto de (1500, 1500)") return end

    -- Bloquear e liberar uma celula.
    if world.set_walk_cost(x, z, 0, math.huge) ~= 1 then falha("bloquear uma celula") return end
    if world.walkable(x, z) then falha("celula bloqueada continua andavel") return end
    if world.walk_cost(x, z) ~= math.huge then falha("custo da bloqueada " .. world.walk_cost(x, z)) return end
    world.set_walk_cost(x, z, 0, 1)
    if not world.walkable(x, z) or world.walk_cost(x, z) ~= 1 then falha("liberar a celula") return end
    if world.set_walk_cost(x, z, 0, 0.2) ~= 0 then falha("custo abaixo de 1 nao virou 1") return end

    -- Caminho reto de 120 u; um disco caro no meio: o caminho contorna.
    local a, b = vector.create(x - 60, 0, z), vector.create(x + 60, 0, z)
    if not (world.walkable(a.x, a.z) and world.walkable(b.x, b.z)) then
        log("OBJ-OK terreno_vivo (sem chao reto para o caminho; so' a celula)")
        return
    end
    local antes = world.path(a, b)
    if not antes then falha("sem caminho antes do custo") return end
    local t0 = os.clock()
    local n = world.set_walk_cost(x, z, 25, 50)
    local depois = world.path(a, b)
    local ms = (os.clock() - t0) * 1000
    if n < 5 then falha("disco de raio 25 mudou " .. n .. " celulas") return end
    if not depois then falha("sem caminho com o disco caro (deveria contornar)") return end
    if not caminhoBarato(depois) then falha("caminho atravessa o disco caro") return end
    -- Bloqueado: o caminho ainda existe (contorna) e nao pisa no disco.
    world.set_walk_cost(x, z, 25, math.huge)
    local bloq = world.path(a, b)
    if bloq and not caminhoBarato(bloq) then falha("caminho atravessa o disco bloqueado") return end
    world.set_walk_cost(x, z, 25, 1)
    if world.walk_cost(x, z) ~= 1 then falha("disco nao voltou a 1") return end
    log(string.format("terreno_vivo: custo + caminho em %.2f ms", ms))

    log("OBJ-OK terreno_vivo")
end)
