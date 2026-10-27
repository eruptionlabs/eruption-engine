-- Anel de chao: liga, troca, tira com nil e recusa raio invalido ou nome
-- que nao existe.
local q = 0
on.start(function() q = 0 end)
local function falha(m) log("OBJ-FALHOU anel: " .. m) end
on.update(function()
    q += 1
    if q == 70 then
        characters.add("teste_anel", "assets/sprites/default.png", camera.target())
        local ok, e = pcall(characters.ring, "teste_anel", { radius = 12, color = vector.create(1, 0.8, 0.2), alpha = 0.7 })
        if not ok then falha("anel recusado: " .. tostring(e)) return end
        ok = pcall(characters.ring, "teste_anel", { radius = 20, width = 3 })
        if not ok then falha("trocar o anel") return end
        if pcall(characters.ring, "teste_anel", { radius = 0 }) then falha("raio 0 aceito") return end
        if pcall(characters.ring, "ninguem", { radius = 5 }) then falha("nome inexistente aceito") return end
    elseif q == 74 then
        characters.hide("teste_anel")
    elseif q == 76 then
        characters.show("teste_anel")
        if not pcall(characters.ring, "teste_anel", nil) then falha("nil nao tira") return end
        objects.find("teste_anel"):destroy()
        log("OBJ-OK anel")
    end
end)
