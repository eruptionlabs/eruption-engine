-- Script de objeto: começa, conta e se destrói no 4º quadro.
return {
    start = function(self)
        self.data.quadros = 0
        self.data.comecou = true
    end,
    update = function(self, dt)
        self.data.quadros += 1
        local r = self.rotation
        self.rotation = vector.create(r.x, r.y + 90 * dt, r.z)
        if self.data.quadros == 4 then self:destroy() end
    end,
}
