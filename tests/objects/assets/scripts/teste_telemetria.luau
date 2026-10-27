-- Telemetria: abre, grava eventos rasos, recusa tabela aninhada, chave do
-- motor e canal "_", conta e fecha. tools/test_objects.sh le' o arquivo depois.
local q = 0
on.start(function() q = 0 end)
local function falha(m) log("OBJ-FALHOU telemetria: " .. m) end
on.update(function()
    q += 1
    if q ~= 90 then return end
    if telemetry.enabled() then falha("ligada sem open") return end
    telemetry.emit("nada", { x = 1 }) -- fechada: nao faz nada
    local caminho = telemetry.open("teste_objetos.jsonl")
    if not caminho or not telemetry.enabled() then falha("open") return end
    for i = 1, 50 do
        telemetry.emit("teste", { valor = i, crit = i % 5 == 0, alvo = "lobo", pos = vector.create(i, 2, 3) })
    end
    telemetry.emit("vazio")
    if pcall(telemetry.emit, "teste", { dentro = { 1 } }) then falha("tabela aninhada aceita") return end
    if pcall(telemetry.emit, "teste", { t = 1 }) then falha("chave t aceita") return end
    if pcall(telemetry.emit, "_motor") then falha("canal _ aceito") return end
    local n, caidos = telemetry.stats()
    if n ~= 51 or caidos ~= 0 then falha(string.format("stats %d %d", n, caidos)) return end
    telemetry.close()
    if telemetry.enabled() then falha("close") return end
    log("OBJ-OK telemetria " .. caminho)
end)
