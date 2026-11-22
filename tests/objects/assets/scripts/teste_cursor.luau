-- input.set_cursor: PNG simples, folha animada, volta ao sistema e a mesma
-- chamada 1000 vezes (o cache tem que evitar recarregar e recriar: so' 3 GLFWcursor
-- (1 + 2 quadros) sao criados, conferido pelo test_objects.sh).
local q = 0
on.start(function() q = 0 end)
on.update(function()
    q += 1
    if q == 90 then
        input.set_cursor("assets/sprites/default.png", 2, 2)
        for _ = 1, 1000 do input.set_cursor("assets/sprites/default.png", 2, 2) end
        input.set_cursor("assets/sprites/default.png", 0, 0, { frame_w = 16, frame_h = 16, frames = 2, fps = 4, scale = 2 })
        for _ = 1, 1000 do
            input.set_cursor("assets/sprites/default.png", 0, 0, { frame_w = 16, frame_h = 16, frames = 2, fps = 4, scale = 2 })
        end
        -- Arquivo que nao existe: aviso no log, cursor do sistema, sem erro.
        input.set_cursor("assets/sprites/nao_existe.png", 0, 0)
        input.set_cursor(nil)
        input.set_cursor(nil)
        log("OBJ-OK cursor")
    end
end)
