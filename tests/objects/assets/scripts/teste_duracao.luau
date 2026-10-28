-- characters.play com `seconds`: a animação inteira dura o pedido. A folha
-- golpe.spr (tools/test_objects.sh) tem "golpe" com 6 quadros de 40 ms:
-- sem seconds dura 0,24 s; com 0,6 s, duas vezes e meia, sem ser cortada.
-- Curto de proposito: o Play do editor no teste dura 90 quadros.
local q = 0
local fase, t, inicio = 0, 0.0, 0.0
local medida = {}
on.start(function() q = 0; fase = 0; t = 0.0 end)
local function falha(m) log("OBJ-FALHOU duracao: " .. m) end
local function toca(seg)
    if not characters.play("teste_golpe", "golpe", false, seg) then falha("play recusou") return false end
    inicio = t
    return true
end
on.update(function(dt)
    q += 1
    t += dt
    if q == 5 then
        characters.add("teste_golpe", "assets/sprites/golpe.spr", camera.target())
    elseif q == 8 then
        if toca(nil) then fase = 1 end
    elseif fase == 1 and characters.animation("teste_golpe") == "" then
        medida.folha = t - inicio
        if toca(0.6) then fase = 2 end
    elseif fase == 2 and characters.animation("teste_golpe") == "" then
        medida.pedida = t - inicio
        fase = 3
        objects.find("teste_golpe"):destroy()
        -- Um quadro de tela de folga em cada ponta (o fim e' visto no
        -- quadro seguinte ao da troca).
        local folga = 0.05
        if math.abs(medida.folha - 0.24) > folga then
            falha(string.format("sem seconds durou %.3f s (folha: 0,24)", medida.folha))
        elseif math.abs(medida.pedida - 0.6) > folga then
            falha(string.format("com seconds=0,6 durou %.3f s", medida.pedida))
        else
            log(string.format("OBJ-OK duracao (folha %.3f s, pedida 0,6 -> %.3f s)", medida.folha, medida.pedida))
        end
    elseif q == 85 and fase < 3 then
        falha(string.format("nao terminou em 85 quadros (fase %d, %.2f s)", fase, t))
        fase = 3
    end
end)
