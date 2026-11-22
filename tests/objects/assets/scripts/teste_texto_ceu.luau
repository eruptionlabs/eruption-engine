-- world.set_sky/sky e a entrada de texto (input.text_begin/text/text_end).
-- O motor injeta caracteres pelo ERUPTION_TEST_INPUT (acao "char,codigo"):
-- 97 ('a') e 233 (e agudo), ver tests/test_objects.sh.
local q = 0
local viuTexto = ""
local function falha(m) log("OBJ-FALHOU texto_ceu: " .. m) end
on.start(function() q = 0 viuTexto = "" end)
on.update(function()
    q += 1
    if q == 2 then
        -- Ceu: desligar vale ate' nova chamada (ou troca de mapa).
        world.set_sky(false)
        if world.sky() ~= false then falha("set_sky(false) nao pegou") return end
        world.set_sky(true)
        if world.sky() ~= true then falha("set_sky(true) nao pegou") return end
        world.set_sky(false)
        -- Texto fora da entrada: nada acumula.
        if input.text_active() then falha("text_active antes de text_begin") return end
        if input.text() ~= "" then falha("text() nao vazio sem entrada") return end
        input.text_begin()
        if not input.text_active() then falha("text_begin nao ligou") return end
    end
    if q >= 3 then viuTexto ..= input.text() end
    if q >= 3 and #viuTexto >= 3 then
        -- O motor injeta 'a' e e-agudo em quadros vizinhos, de 10 em 10 quadros.
        if viuTexto ~= "a\xC3\xA9" and viuTexto ~= "\xC3\xA9a" then
            falha(string.format("texto = %q (esperado 'a' e e agudo em UTF-8)", viuTexto)) return
        end
        input.text_end()
        if input.text_active() then falha("text_end nao desligou") return end
        if input.text() ~= "" then falha("text() nao limpou no text_end") return end
        if world.sky() ~= false then falha("ceu mudou sozinho") return end
        world.set_sky(true) -- devolve o ceu: o editor roda este teste duas vezes
        log("OBJ-OK texto_ceu")
        q = -100000 -- uma vez so'
    elseif q == 80 then
        falha("nenhum texto chegou")
    end
end)
