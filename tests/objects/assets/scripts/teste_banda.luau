-- perf.bandwidth (medidor de banda, spec 50): depois de alguns quadros a
-- tabela do ultimo quadro tem passes com nome e bytes, a soma bate com o
-- total, a auditoria ja' viu alvos e o contador real diz de onde vem (ou
-- por que nao ha').
local q = 0
on.start(function() q = 0 end)
local function falha(m) log("OBJ-FALHOU banda: " .. m) end
on.update(function()
    q += 1
    if q ~= 30 then return end
    local b = perf.bandwidth()
    if type(b) ~= "table" then falha("perf.bandwidth nao devolveu tabela") return end
    if not (b.write_mb > 0) then falha("write_mb = " .. tostring(b.write_mb)) return end
    if #b.passes == 0 then falha("nenhum passe") return end
    local r, w = 0, 0
    for _, p in b.passes do
        if type(p.name) ~= "string" or p.name == "" then falha("passe sem nome") return end
        r += p.read_mb
        w += p.write_mb
    end
    if math.abs(r - b.read_mb) > 0.01 or math.abs(w - b.write_mb) > 0.01 then
        falha(string.format("soma dos passes %.2f/%.2f != total %.2f/%.2f", r, w, b.read_mb, b.write_mb))
        return
    end
    if not (b.audit.targets > 0) then falha("auditoria sem alvos") return end
    if type(b.dram) ~= "string" or b.dram == "" then falha("dram sem estado") return end
    if b.textures.compressed_mb < 0 or b.textures.uncompressed_mb < 0 then falha("texturas negativas") return end
    log(string.format("OBJ-OK banda (%d passes, %.1f MB lidos + %.1f MB gravados, %d alvos, dram: %s)",
        #b.passes, b.read_mb, b.write_mb, b.audit.targets, b.dram))
end)
