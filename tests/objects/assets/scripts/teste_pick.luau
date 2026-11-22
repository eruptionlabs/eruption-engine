-- Picking: world.screen_to_ground e characters.at_screen.
--  1. o centro da tela cai no chao (y = altura do chao) e sobre o raio da camera;
--  2. o ponto do chao reprojetado volta ao pixel (erro em unidades de mundo);
--  3. o personagem sob o pixel e' achado, o vizinho do lado nao, e fora -> nil.
local q = 0
local erroIda = 0
local pxA, pyA = 0, 0
on.start(function() q = 0 end)
local function falha(m) log("OBJ-FALHOU pick: " .. m) end
on.update(function()
    q += 1
    if q == 70 then
        characters.add("teste_pick", "assets/sprites/default.png", camera.target())
    elseif q == 80 then
        local w, h = ui.size()
        local hx, hy, vis = characters.head_screen("teste_pick")
        if not vis then falha("cabeca fora da tela") return end
        -- No editor a area do jogo e' o painel da vista: o centro da tela nao e'
        -- o do jogo, e a prova do centro so' vale com a janela inteira.
        local cheia = math.abs(hx - w / 2) < 2
        if cheia then
        local hit = world.screen_to_ground(w / 2, h / 2)
        if not hit then falha("centro da tela sem chao") return end
        local gy = world.floor_height(hit.X, hit.Z)
        if math.abs(hit.Y - gy) > 0.05 then falha(string.format("y %.3f, chao %.3f", hit.Y, gy)) return end
        -- Esperado: o raio do centro passa pelo alvo da camera; o ponto do chao
        -- fica na reta alvo-camera (a mesma direcao horizontal, como o alvo e' o
        -- centro da tela). Erro = distancia do acerto a' reta.
        local yaw, pitch, dist = camera.orbit()
        local t = camera.target()
        local cy, cp = math.rad(yaw), math.rad(pitch)
        -- direcao alvo->camera (convencao da orbita: yaw -90 poe a camera em -Z)
        local dir = vector.create(math.cos(cy) * math.cos(cp), math.sin(cp), math.sin(cy) * math.cos(cp))
        local v = hit - t
        local along = vector.dot(v, dir)
        local perp = vector.magnitude(v - dir * along)
        if perp > 2.0 then falha(string.format("acerto a %.3f da reta do centro (hit %s alvo %s yaw %.1f pitch %.1f dist %.1f)", perp, tostring(hit), tostring(t), yaw, pitch, dist)) return end
        -- Fora da tela por cima do horizonte: nil (olhando o ceu nao ha chao).
        if world.screen_to_ground(w / 2, -100000) ~= nil then falha("pixel absurdo achou chao") return end
        log(string.format("PICK-ERRO centro: %.4f unidades da reta, y %.4f", perp, hit.Y - gy))
        erroIda = perp
        end
        -- Personagem: um ponto abaixo da cabeca acha, longe nao.
        -- head_screen e' em pixels da interface; no desktop = janela.
        local ok = characters.at_screen(hx, hy + 6)
        if ok ~= "teste_pick" then falha("sob a cabeca: " .. tostring(ok)) return end
        if characters.at_screen(hx + 2000, hy) ~= nil then falha("longe achou personagem") return end
        if characters.at_screen(hx, hy - 400) ~= nil then falha("acima achou personagem") return end
        -- O pe' (alvo) ao reprojetar: clique no chao sob o personagem volta a ele.
        -- Ida e volta: o personagem posto no chao clicado e' achado naquele pixel.
        pxA, pyA = hx + 80, hy + 100
        local alvo = world.screen_to_ground(pxA, pyA)
        if not alvo then falha("pixel sem chao") return end
        characters.set_position("teste_pick", alvo)
    elseif q == 90 then
        local px, py = pxA, pyA
        local achou = characters.at_screen(px, py - 8)
        if achou ~= "teste_pick" then falha("ida e volta: " .. tostring(achou)) return end
        if characters.at_screen(px, py + 60) ~= nil then falha("abaixo do pe' achou personagem") return end
        local a = world.screen_to_ground(px, py)
        local p = characters.position("teste_pick")
        local e = vector.magnitude(a - p)
        if e > 2.0 then falha(string.format("ida e volta a %.3f", e)) return end
        objects.find("teste_pick"):destroy()
        log(string.format("OBJ-OK pick (erro reta %.4f, ida e volta %.4f)", erroIda, e))
    end
end)
