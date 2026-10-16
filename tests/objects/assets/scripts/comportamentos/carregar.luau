-- Script de objeto que carrega o jogo de dentro do próprio update.
return {
    update = function(self)
        if self.data.feito then return end
        self.data.feito = true
        game.load("robusto")
    end,
}
