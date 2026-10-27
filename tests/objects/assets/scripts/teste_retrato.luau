-- Retrato: liga, troca o recorte, recusa recorte vazio e solta com nil,
-- com o personagem existindo e depois de ele sair.
local q = 0
on.start(function() q = 0 end)
local function falha(m) log("OBJ-FALHOU retrato: " .. m) end
on.update(function()
    q += 1
    if q == 80 then
        characters.add("teste_retrato", "assets/sprites/default.png", camera.target())
        if not pcall(ui.portrait, "#teste-retrato", "teste_retrato") then falha("retrato recusado") return end
        if not pcall(ui.portrait, "#teste-retrato", "teste_retrato", { 0, 0, 1, 0.4 }) then falha("recorte recusado") return end
        if pcall(ui.portrait, "#teste-retrato", "teste_retrato", { 0, 0, 0, 1 }) then falha("recorte vazio aceito") return end
    elseif q == 83 then
        objects.find("teste_retrato"):destroy()
    elseif q == 86 then
        if not pcall(ui.portrait, "#teste-retrato", nil) then falha("nil nao solta") return end
        log("OBJ-OK retrato")
    end
end)
