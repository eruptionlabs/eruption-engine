-- Personagem como objeto: pai leva o personagem; destruído não volta sozinho.
local q = 0
-- O módulo vive entre um Play e outro do editor: cada jogo começa do zero.
on.start(function() q = 0 end)
local function falha(m) log("OBJ-FALHOU personagem: " .. m) end
on.update(function()
    q += 1
    if q == 50 then
        characters.add("teste_guarda", "assets/sprites/default.png", vector.create(0, 0, 0))
        characters.add("teste_alvo", "assets/sprites/default.png", vector.create(40, 0, 0))
    elseif q == 52 then
        local guarda = objects.find("teste_guarda")
        if not guarda or guarda.character ~= "teste_guarda" then falha("personagem virou objeto") return end
        local carro = objects.create("carro_teste", vector.create(0, 0, 0))
        guarda.parent = carro
        carro.position = vector.create(70, 0, 0)
        objects.find("teste_alvo"):destroy()
    elseif q == 55 then
        if vector.magnitude(characters.position("teste_guarda") - vector.create(70, 0, 0)) > 0.05 then
            falha("pai leva o personagem " .. tostring(characters.position("teste_guarda"))) return
        end
        if objects.find("teste_alvo") then falha("objeto destruído voltou sozinho") return end
        log("OBJ-OK personagem")
    end
end)
