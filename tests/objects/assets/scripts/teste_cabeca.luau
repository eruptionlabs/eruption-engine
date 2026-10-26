-- Âncora de cabeça: a faixa transparente de cima da folha não conta. A mesma
-- arte com 48 linhas vazias em cima dá o mesmo topo na tela (até 1 px).
local q = 0
on.start(function() q = 0 end)
local function falha(m) log("OBJ-FALHOU cabeca: " .. m) end
on.update(function()
    q += 1
    if q == 60 then
        local p = camera.target()
        characters.add("teste_cortada", "assets/sprites/default.png", p)
        characters.add("teste_faixa", "assets/sprites/faixa_alta.png", p)
    elseif q == 66 then
        local xa, ya, va = characters.head_screen("teste_cortada")
        local xb, yb, vb = characters.head_screen("teste_faixa")
        if not va or not vb then falha("topo fora da tela") return end
        if math.abs(ya - yb) > 1 or math.abs(xa - xb) > 1 then
            falha(string.format("cortada (%.1f, %.1f), com faixa (%.1f, %.1f)", xa, ya, xb, yb)) return
        end
        objects.find("teste_cortada"):destroy()
        objects.find("teste_faixa"):destroy()
        log(string.format("OBJ-OK cabeca (y %.1f e %.1f)", ya, yb))
    end
end)
