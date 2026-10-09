# Aula 8 — Das regras para o código

Cada folha de regras é um script Luau. Na aba Regras, **Ver código** mostra o código
que as frases geram:

```lua
on.enter("porta", function()
    ui.message("Bem-vindo!", 3)
    models.move_by("porta", vector.create(0, 30, 0), 1)
end)
```

## Scripts

- **Jogo > Novo script** cria um arquivo em `assets/scripts` e abre no editor de
  código. Salvar (**Ctrl+S**) recarrega o script no jogo na hora.
- **Jogo > Abrir a pasta de scripts no VS Code**: com a extensão *luau-lsp*, o VS Code
  completa a API do motor (o arquivo `eruption.d.luau` diz o que existe).

## Depurador

1. **Jogo > Instalar o depurador no VS Code** e reinicie o VS Code.
2. Abra a pasta `assets/scripts` no VS Code.
3. No editor do Eruption, aperte **F5** (Play). No VS Code, **F5** em *Debug game scripts*.
4. Clique na margem de uma linha para marcar um ponto de parada. Quando o jogo passa
   por ela, ele congela e o VS Code mostra a pilha, as variáveis locais e as do jogo.
   **F10** próxima linha, **F11** entra na função, **F5** continua.
