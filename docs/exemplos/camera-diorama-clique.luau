-- Exemplo: camera de diorama (inclinada, com foco de plano inclinado), clique
-- no chao para andar e clique num personagem para selecionar.
-- Cole num script do projeto (assets/scripts/). "heroi" e "vizinho" sao
-- personagens da cena (characters.add ou arrastados no editor).

local HEROI = "heroi"
local selecionado = nil

on.start(function()
    -- Visual de diorama (presets de data/diorama.json) e foco: um plano
    -- inclinado, nitido na faixa do jogo e desfocado acima e abaixo dela.
    world.set_look("diorama")
    world.set_focus(characters.position(HEROI), {
        model = "tilt_plane",
        tilt_angle = 60,   -- 0 = plano em pe de frente para a camera, 90 = deitado
        band = 120,        -- unidades de mundo nitidas em volta do ponto
        max_blur = 3,
    })
    -- Camera: yaw e pitch em graus, distancia em unidades de mundo.
    camera.free(true)                       -- o script manda, nao o jogador
    camera.set_orbit(45, 50, 260)
    camera.set_target(characters.position(HEROI))
end)

on.update(function(dt)
    -- Clique esquerdo: personagem sob o cursor? Senao, o chao sob o cursor.
    if input.pressed("mouse_left") then
        local x, y = input.mouse_position()
        local nome = characters.at_screen(x, y)
        if nome and nome ~= HEROI then
            selecionado = nome
            log("selecionado: " .. nome)
        else
            local alvo = world.screen_to_ground(x, y)
            if alvo then
                -- walk_to desvia de solidos e do que nao se anda; a animacao
                -- de andar toca sozinha. seconds opcional fixa o ritmo.
                characters.walk_to(HEROI, alvo)
            end
        end
    end

    -- Animacao extra da folha (v3) no clique direito, se o personagem a tem.
    if input.pressed("mouse_right") then
        characters.play(HEROI, "golpe", false)
    end

    -- A camera segue o personagem com suavidade e o foco vai junto.
    local p = characters.position(HEROI)
    local c = camera.target()
    local k = 1 - math.exp(-6 * dt)
    camera.set_target(c + (p - c) * k)
    world.set_focus(p, { model = "tilt_plane", tilt_angle = 60, band = 120, max_blur = 3 })
end)
