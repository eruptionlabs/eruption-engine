-- Prefab composto: filhos, prefab dentro de prefab, giro, etiqueta, script, estoque.
local q, monte = 0
-- O módulo vive entre um Play e outro do editor: cada jogo começa do zero.
on.start(function() q, monte = 0, nil end)
local function falha(m) log("OBJ-FALHOU prefab: " .. m) end
on.update(function()
    q += 1
    if q == 30 then
        monte = objects.spawn("assets/prefabs/monte.prefab.luau", vector.create(0, 0, -150))
        if not monte then falha("spawn") return end
        if #monte:children() ~= 3 then falha("filhos " .. #monte:children()) end
        if not monte:has_tag("monte") then falha("etiqueta") end
        local topo = monte:find("topo")
        if not topo or not topo.model then falha("prefab dentro de prefab") end
        local b = monte:find("b")
        if not b or math.abs(b.rotation.y - 90) > 0.1 then falha("giro do filho") end
    elseif q == 35 and monte then
        if (monte.data.n or 0) < 3 then falha("script do prefab") return end
        local antes = models.count()
        monte:destroy()
        monte = objects.spawn("assets/prefabs/monte.prefab.luau", vector.create(0, 0, -150))
        if models.count() ~= antes then falha("reaproveitar " .. antes .. " -> " .. models.count()) return end
        log("OBJ-OK prefab")
    end
end)
