-- ui.state e ui.hovered: a API existe, aceita seletor de id, classe e
-- composto, e nao da erro com elemento que nao existe. As cores (var(),
-- .row.selected, :hover) foram conferidas por imagem na mao.
local q = 0
on.start(function() q = 0 end)
on.update(function()
    q += 1
    if q == 20 then
        ui.load("assets/ui/estado.css")
    elseif q == 40 then
        ui.state("#r2", "selected", true)
        ui.state(".row", "selected", false)
        ui.state("rect.row.selected", "x", true)
        ui.state("#nao_existe", "selected", true)
        local h = ui.hovered("#r1") or ui.hovered(".row") or ui.hovered("#nao_existe")
        if type(h) == "boolean" then log("OBJ-OK ui_estado") else log("OBJ-FALHOU ui_estado: hovered nao devolveu boolean") end
        ui.reset()
    end
end)
