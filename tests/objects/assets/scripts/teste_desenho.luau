-- world.draw_strip / world.draw_sprite: 20 "portais" por quadro, cada um com 2 sinos
-- aditivos de 4 camadas x 21 segmentos (cor por quad, topologia "quads"), um disco de
-- 10 fatias com o U rolando (strip) e 7 sprites: ~10 mil vertices por quadro. O teste
-- confere que a chamada nao da erro (nem com argumentos tortos) e que a imagem sai.
local vec = vector.create
local t = 0
local feito = false
local SEG, CAM = 21, 4
-- A camera do jogo pode ficar a 1000 de distancia (campo de ~800 m). O teste NAO mexe na
-- camera (os outros testes a usam); em vez disso escala o conteudo por distancia / 40, para
-- os portais aparecerem do mesmo tamanho na imagem em qualquer camera.
local ESC = 1

local function sino(cx, cy, cz, tempo, fase, raio0)
    local v = {}
    for c = 1, CAM do
        local f0, f1 = (c - 1) / CAM, c / CAM
        local r0 = raio0 * (1 - 0.6 * f0) * (1 + 0.1 * math.sin(tempo * 3 + fase))
        local r1 = raio0 * (1 - 0.6 * f1) * (1 + 0.1 * math.sin(tempo * 3 + fase + 1))
        local y0, y1 = cy + 2.5 * ESC * f0, cy + 2.5 * ESC * f1
        for s = 0, SEG - 1 do
            local a0 = s / SEG * math.pi * 2 + tempo
            local a1 = (s + 1) / SEG * math.pi * 2 + tempo
            local cr = 0.3 + 0.7 * math.abs(math.sin(a0 + fase))
            local alfa = 0.9 * (1 - f0)
            local ru, rv = s / SEG, f0
            local ru1, rv1 = (s + 1) / SEG, f1
            table.insert(v, { pos = vec(cx + math.cos(a0) * r0, y0, cz + math.sin(a0) * r0), u = ru, v = rv, r = cr, g = 0.5 + 0.5 * f0, b = 1.5, a = alfa })
            table.insert(v, { pos = vec(cx + math.cos(a1) * r0, y0, cz + math.sin(a1) * r0), u = ru1, v = rv, r = cr, g = 0.5 + 0.5 * f0, b = 1.5, a = alfa })
            table.insert(v, { pos = vec(cx + math.cos(a1) * r1, y1, cz + math.sin(a1) * r1), u = ru1, v = rv1, r = cr, g = 0.5 + 0.5 * f0, b = 1.5, a = alfa })
            table.insert(v, { pos = vec(cx + math.cos(a0) * r1, y1, cz + math.sin(a0) * r1), u = ru, v = rv1, r = cr, g = 0.5 + 0.5 * f0, b = 1.5, a = alfa })
        end
    end
    return v
end

local function disco(cx, cy, cz, tempo, raio)
    local v = {}
    for i = 0, 10 do
        local a = i / 10 * math.pi * 2
        local u = i / 10 * 3 + tempo * 0.5
        table.insert(v, { pos = vec(cx + math.cos(a) * raio, cy, cz + math.sin(a) * raio), u = u, v = 0, r = 1, g = 0.8, b = 0.3, a = 1 })
        table.insert(v, { pos = vec(cx, cy, cz), u = u, v = 1, r = 1, g = 0.8, b = 0.3, a = 1 })
    end
    return v
end

on.start(function()
    t = 0
    feito = false
    local _, _, d = camera.orbit()
    ESC = math.max(d / 40, 0.05)
end)
on.update(function(dt)
    t += dt
    -- Grade de 4x5 portais na frente da camera, uns 6..22 m a frente, no chao do jogador.
    local p = camera.target()
    local fwd, rgt = camera.basis()
    local fx, fz = fwd.x, fwd.z
    local fl = math.sqrt(fx * fx + fz * fz); if fl < 1e-3 then fx, fz, fl = 0, 1, 1 end
    fx, fz = fx / fl, fz / fl
    local total = 0
    for k = 0, 19 do
        local lado = ((k % 5) - 2) * 3.2 * ESC
        local prof = (math.floor(k / 5) - 1.5) * 3.4 * ESC
        local cx = p.x + fx * prof - fz * lado
        local cz = p.z + fz * prof + fx * lado
        local cy = p.y + 0.3
        local b1 = sino(cx, cy, cz, t, k, 1.6 * ESC)
        local b2 = sino(cx, cy, cz, -t * 1.3, k + 2, 1.1 * ESC)
        world.draw_strip("assets/sprites/energia.png", b1, { topology = "quads", blend = "additive" })
        world.draw_strip("assets/sprites/energia.png", b2, { topology = "quads" })
        world.draw_strip("assets/sprites/energia.png", disco(cx, cy + 0.05 * ESC, cz, t, 1.8 * ESC), { blend = "additive", depth_test = false })
        for s = 1, 7 do
            local a = t * 2 + s
            world.draw_sprite("assets/sprites/brilho.png", vec(cx + math.cos(a) * 0.9 * ESC, cy + (0.5 + s * 0.3) * ESC, cz + math.sin(a) * 0.9 * ESC), 0.5 * ESC, { 1, 0.7, 0.3, 0.9 }, "additive")
        end
        total += #b1 + #b2 + 22
    end
    -- sprite de tamanho fixo em pixels, "alpha" e rotacao
    world.draw_sprite("assets/sprites/brilho.png", vec(p.x, p.y + 2, p.z), vec(48, 48), { r = 0.2, g = 1, b = 0.3, a = 0.8 }, "alpha", { pixels = true, rotation = t, depth_test = false })
    if t > 1.5 and not feito then
        feito = true
        -- Casos tortos: textura que nao existe (so' aviso), strip curta, lista vazia.
        world.draw_strip("assets/sprites/nao_existe.png", { { pos = vec(0, 0, 0) } })
        world.draw_strip("assets/sprites/energia.png", {})
        world.draw_strip("assets/sprites/energia.png", { { pos = vec(0, 0, 0) }, { pos = vec(1, 0, 0) } })
        local ok = pcall(world.draw_strip, "assets/sprites/energia.png", { { u = 1 } })
        local ok2 = pcall(world.draw_strip, "assets/sprites/energia.png", {}, { blend = "xyz" })
        if not ok and not ok2 and total >= 9000 then log("OBJ-OK desenho") else log("OBJ-FALHOU desenho: " .. tostring(ok) .. tostring(ok2) .. total) end
    end
end)
