-- Pai, etiquetas e script declarados no .scene.luau.
-- Confere no começo e de novo depois do game.load do teste "salvar" (quadro
-- 22): o que a cena declara e o prefab posto nela têm que continuar.
local q, vezes = 0, 0
-- O módulo vive entre um Play e outro do editor: cada jogo começa do zero.
on.start(function() q, vezes = 0, 0 end)
on.update(function()
    q += 1
    if q ~= 6 and q ~= 40 then return end
    local filha = objects.find("namaqualand_boulder_06_1129/namaqualand_boulder_04_1130")
    if not filha then log("OBJ-FALHOU cena: pai declarado") return end
    if not (filha:has_tag("cena") and filha:has_tag("pedra")) then log("OBJ-FALHOU cena: etiquetas") return end
    if (filha.data.n or 0) < 2 then log("OBJ-FALHOU cena: script declarado") return end
    -- Prefab composto posto na cena.
    local carroca = objects.find("carroca_cena")
    if not carroca or #carroca:children() ~= 2 then log("OBJ-FALHOU cena: filhos do prefab posto na cena") return end
    if not carroca:has_tag("carroca") or (carroca.data.n or 0) < 2 then log("OBJ-FALHOU cena: etiqueta/script do prefab") return end
    local roda = carroca:find("roda_d")
    if not roda or vector.magnitude(roda.position - vector.create(530, 40, 500)) > 0.5 then
        log("OBJ-FALHOU cena: lugar do filho " .. tostring(roda and roda.position)) return
    end
    vezes += 1
    if vezes == 2 then log("OBJ-OK cena") end
end)
