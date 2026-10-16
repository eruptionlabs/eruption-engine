# Aula 11 — Objetos: hierarquia, criar e destruir, scripts por objeto

Tudo que está na cena é um **objeto**: cada modelo do mapa, cada modelo acrescentado,
cada personagem, cada luz, área de gatilho e emissor de som. Um objeto tem nome, posição, giro e escala, pode ter **pai** (o pai
leva os filhos quando se mexe), **etiquetas** (palavras livres) e **scripts próprios**.
Os scripts também criam e destroem objetos com o jogo rodando.

As funções antigas (`models.*`, `characters.*`) continuam valendo: por baixo elas
mexem no mesmo objeto.

## Achar um objeto

```lua
local porta = objects.find("porta")          -- pelo nome
local trinco = objects.find("casa/porta/trinco") -- descendo pela hierarquia
local inimigos = objects.with_tag("perigoso")   -- todos com a etiqueta
local outro = objects.find("tocha#1")           -- nome repetido: a segunda "tocha"
```

Um modelo acrescentado com vários nós (um `.glb` com várias peças) vira um objeto com
filhos: `casa` com `casa/porta`, `casa/janela`...

## Mexer

```lua
porta.position = porta.position + vector.create(0, 5, 0) -- posição no mundo
porta.rotation = vector.create(0, 90, 0)                 -- graus, relativo ao pai
porta.scale = 2
porta.active = false   -- some, e tudo que está dentro dela some junto
```

`local_position`, `rotation` e `scale` são relativos ao pai; `position` é sempre no
mundo.

Luzes viram objetos chamados `light_1`, `light_2`... (o número é o mesmo de
`lights.*`); áreas e sons têm o nome que você deu no editor.

## Pai e filhos

```lua
local carroca = objects.find("carroca")
local caixa = objects.find("caixa")
caixa.parent = carroca   -- a caixa fica onde está e passa a andar com a carroça
carroca:move_by(vector.create(100, 0, 0))
```

Uma lanterna que anda com o personagem:

```lua
objects.find("light_3").parent = objects.find("guarda")
objects.find("guarda").active = false   -- some o guarda e apaga a lanterna
```

`obj:set_parent(pai, false)` encaixa usando a posição local que o objeto já tinha.
Um objeto não pode ficar dentro de um filho dele (dá erro).

## Criar e destruir

```lua
-- Um .glb inteiro, um nó só dele ("arquivo.glb#Nó") ou um prefab.
local pedra = objects.spawn("assets/props/pedra.glb", vector.create(10, 0, 20))
local poste = objects.spawn("assets/prefabs/poste.prefab.luau", lugar, nil, "poste_1")

local grupo = objects.create("comboio", lugar)  -- objeto vazio: um grupo
pedra.parent = grupo

pedra:destroy()
print(pedra:alive())  -- false; ler um campo de um objeto destruído dá erro
```

Destruir e criar de novo o mesmo arquivo reaproveita o modelo: criar cem tiros por
segundo não faz a cena crescer. O que os scripts criaram some no **Stop** do editor.

## Prefab composto

Um prefab pode montar um objeto inteiro: a raiz (com ou sem modelo), etiquetas,
scripts e filhos, que podem ser outros prefabs.

```lua
-- assets/prefabs/carroca.prefab.luau
return {
    file = "assets/props/carroca.glb",   -- sem 'file' a raiz é só um grupo
    tags = {"veiculo"},
    scripts = {"andar"},
    children = {
        { prefab = "assets/prefabs/roda.prefab.luau", name = "roda_e", position = {-8, 3, 0} },
        { prefab = "assets/prefabs/roda.prefab.luau", name = "roda_d", position = {8, 3, 0} },
        { file = "assets/props/carga.glb", node = "Caixa", position = {0, 6, 0}, rotation = {0, 15, 0} },
    },
}
```

`objects.spawn("assets/prefabs/carroca.prefab.luau", lugar)` cria tudo de uma vez; a
carroça leva as rodas e a carga. Arrastado do painel Projeto para a cena, o prefab
também vem inteiro (precisa de um `file` na raiz); os filhos saem do prefab a cada
vez que a cena abre, então mudar o prefab muda todas as carroças.

## Dados e etiquetas

```lua
pedra.data.vida = 3          -- tabela do jogo presa ao objeto
pedra:add_tag("quebravel")
if pedra:has_tag("quebravel") then pedra.data.vida -= 1 end
```

`data` aceita números, textos, vetores, tabelas e até outros objetos.

## Script por objeto

Um arquivo em `assets/scripts` que devolve funções:

```lua
-- assets/scripts/girar.luau
return {
    start = function(self)
        self.data.velocidade = 45
    end,
    update = function(self, dt)
        local r = self.rotation
        self.rotation = vector.create(r.x, r.y + self.data.velocidade * dt, r.z)
    end,
    stop = function(self) end,
}
```

```lua
objects.find("moinho"):attach("girar")
```

`self` é o objeto. Guarde o estado em `self.data`: ao salvar o `girar.luau` com o
jogo rodando, o código novo entra e cada objeto continua de onde estava.

## No editor

Selecione um modelo ou personagem: o **Inspetor > Objeto** mostra o caminho e deixa
escrever o **Pai**, as **Etiquetas** e os **Scripts** (separados por vírgula, Enter
confirma). Isso fica no arquivo da cena:

```lua
objects = {
    ["porta"] = { parent = "casa", tags = {"interativo"}, scripts = {"abrir"} },
},
```

Na **Hierarquia**, a seção **Hierarquia de objetos** mostra as famílias (objetos com
filhos). Arraste um modelo ou personagem para cima de outro para pô-lo dentro; solte
no título da seção para tirá-lo. Ctrl+Z desfaz.

O que um script muda durante o jogo (pai, etiqueta, objeto destruído) volta ao que a
cena diz no Stop e nunca vai para o arquivo.

## Jogo salvo

`game.save` guarda os objetos que os scripts criaram (com pai, posição, etiquetas,
dados e scripts) e as etiquetas e dados dos objetos do mapa. Depois de `game.load`
os objetos são criados de novo: ache-os pelo nome (`objects.find`). Objetos guardados
em `vars` e em `data` voltam sozinhos.
