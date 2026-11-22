-- Script de editor (manifesto "editor_scripts"): le uma lista de texto de uma
-- pasta fora de assets (listada em "editor_read_paths") e cria um personagem de
-- previa por linha. Escreve "PREV-..." no log; tests/test_objects.sh confere.
local lista, motivo = io.read_text("../extra/lista.txt")
if not lista then log("PREV-FALHOU leitura permitida: " .. tostring(motivo)) end

local function negado(caminho, rotulo)
    local texto = io.read_text(caminho)
    if texto ~= nil then log("PREV-FALHOU leitura fora da lista foi aceita: " .. rotulo)
    else log("PREV-NEGADO " .. rotulo) end
end

return {
    on_map_loaded = function(mapa)
        local n = 0
        for nome, x, z in string.gmatch(lista or "", "(%S+) (%S+) (%S+)") do
            if characters.add(nome, "assets/sprites/default.png", vector.create(tonumber(x), 0, tonumber(z))) then n += 1 end
        end
        local pedra = objects.spawn("assets/prefabs/pedrinha.prefab.luau", vector.create(420, 0, 300), nil, "prev_pedra")
        local nomes = io.list_dir("../extra")
        log(string.format("PREV-MAPA %s personagens=%d pedra=%s arquivos=%d", mapa, n, tostring(pedra ~= nil), nomes and #nomes or -1))
        negado("../secret/x.txt", "irmao")
        negado("../extra/../secret/x.txt", "ponto-ponto")
        negado("/etc/hostname", "absoluto")
        local lista2 = io.list_dir("../secret")
        if lista2 ~= nil then log("PREV-FALHOU list_dir fora da lista foi aceito") else log("PREV-NEGADO list_dir") end
    end,
    on_preview_clear = function() log("PREV-LIMPA") end,
}
