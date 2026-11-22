-- Script de JOGO: no Play a previa do editor nao pode existir (nem rodar).
local function conta()
    local n = 0
    for _, nome in ipairs({"prev_a", "prev_b", "prev_c"}) do
        if characters.exists(nome) then n += 1 end
    end
    return n
end
on.start(function()
    log(string.format("PREV-JOGO personagens=%d pedra=%s", conta(), tostring(objects.find("prev_pedra") ~= nil)))
    if io.read_text("../extra/lista.txt") ~= nil then log("PREV-FALHOU io.read_text funcionou no jogo") else log("PREV-JOGO-SEM-IO") end
end)
-- Fora do Play (edicao) o script de jogo nao roda: quem relata a presenca e' on_map_loaded.
