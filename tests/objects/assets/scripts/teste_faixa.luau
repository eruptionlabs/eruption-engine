-- Faixa por spline: a da cena existe desde a carga; world.ribbon monta outra;
-- a consulta e' pura e da' lado, borda, t e altura com o sinal certo.
local q = 0
on.start(function() q = 0 end)
local function falha(m) log("OBJ-FALHOU faixa: " .. m) end
local function perto(a, b, tol) return math.abs(a - b) <= tol end
on.update(function()
    q += 1
    if q ~= 30 then return end
    -- Anel da cena: 4 pontos num losango de raio 150 em volta de (500, 500); a
    -- curva passa entre o losango (849 m) e o circulo (942 m).
    local volta = world.ribbon_length("anel_cena")
    if not (volta > 849 and volta < 942) then falha("anel da cena com " .. volta .. " m") return end

    -- Reta de 200 m andando para +z: a direita e' +x.
    world.ribbon("reta", { vector.create(400, 0, 300), vector.create(400, 0, 500) }, { width = 20, step = 2 })
    local p = vector.create(405, 0, 400)
    local r = world.ribbon_query("reta", p)
    if not perto(r.lateral, 5, 0.01) or not perto(r.edge, 5, 0.01) then falha("lateral " .. r.lateral .. " borda " .. r.edge) return end
    if not perto(r.t, 0.5, 0.002) or not perto(r.s, 100, 0.2) then falha("t " .. r.t .. " s " .. r.s) return end
    if r.normal.x > -0.99 then falha("normal da borda para dentro") return end
    if not perto(r.height, world.ground_height(405, 400) + 0.1, 0.5) then falha("altura " .. r.height) return end
    local fora = world.ribbon_query("reta", vector.create(430, 0, 400))
    if fora.edge >= 0 then falha("fora da faixa com borda " .. fora.edge) return end

    -- Pura: repetir da' os mesmos bits.
    local r2 = world.ribbon_query("reta", p)
    if r2.lateral ~= r.lateral or r2.s ~= r.s or r2.height ~= r.height then falha("consulta nao e' pura") return end

    local m = world.ribbon_point("reta", 0.25)
    if not perto(m.position.z, 350, 0.2) or not perto(m.position.x, 400, 0.01) or m.tangent.z < 0.99 then
        falha("ponto em t = 0,25") return
    end
    if pcall(world.ribbon_query, "nao_existe", p) then falha("faixa inexistente nao deu erro") return end
    if pcall(world.ribbon, "torta", { vector.create(1, 0, 1) }) then falha("um ponto so' virou faixa") return end
    log("OBJ-OK faixa")
end)
