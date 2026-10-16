-- Jogo salvo à prova de: tabela que contém a si mesma, arquivo corrompido
-- (o executor põe "quebrado.json" na pasta de saves) e game.load chamado por
-- um script de objeto.
local q = 0
on.start(function() q = 0 end)
local function falha(m) log("OBJ-FALHOU robusto: " .. m) end
on.update(function()
    q += 1
    if q == 60 then
        local t = { nome = "ciclo" }
        t.a, t.b, t.c = t, t, t
        vars.set("ciclo", t)
        if not game.save("robusto") then falha("salvar com ciclo") return end
        vars.set("ciclo", nil)
    elseif q == 62 then
        game.load("quebrado") -- torto: avisa e segue
    elseif q == 64 then
        local o = objects.create("carregador")
        o:attach("comportamentos/carregar")
    elseif q == 70 then
        if objects.find("carregador") then falha("game.load do script de objeto não aconteceu") return end
        log("OBJ-OK robusto")
    end
end)
