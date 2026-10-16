-- Script de objeto: conta os quadros em self.data.n.
return { update = function(self) self.data.n = (self.data.n or 0) + 1 end }
