# Contribuindo com a Eruption Engine

Obrigado pelo interesse em contribuir com a Eruption Engine! Este documento define as regras para contribuições de código, documentação e outros materiais.

## 1. Termo de Contribuição (CLA)

Ao contribuir com este projeto — seja por pull request, issue, patch, sugestão de código ou qualquer outro material — você concorda com os termos abaixo:

> **Contributor License Agreement (CLA) — Eruption Engine**
>
> Eu, **{nome completo}**, declaro que:
>
> 1. Sou autor das contribuições que estou enviando, ou tenho permissão expressa para enviá-las.
> 2. Concedo ao titular do projeto uma licença perpétua, irrevogável, mundial, gratuita e não exclusiva para usar, reproduzir, modificar, adaptar, publicar, traduzir, sublicenciar e distribuir minhas contribuições, em qualquer formato e para qualquer fim, incluindo fins comerciais.
> 3. Concordo que minhas contribuições serão licenciadas sob os termos da **Apache License 2.0**, juntamente com o restante do projeto.
> 4. Entendo que o projeto pode ser usado por terceiros, inclusive para desenvolvimento de jogos comerciais, sem que eu tenha direito a qualquer remuneração por minhas contribuições.
> 5. Declaro que minhas contribuições não infringem direitos autorais, patentes, marcas ou outros direitos de terceiros.

## 2. Como contribuir

1. **Abra uma issue primeiro** para discutir mudanças grandes.
2. Faça um fork do repositório.
3. Crie uma branch com nome descritivo: `feature/nome-da-feature` ou `fix/nome-do-bug`.
4. Escreva código claro, comentado e que siga o estilo existente.
5. Adicione testes quando aplicável.
6. Certifique-se de que o build passa:
   ```bash
   mkdir build && cd build
   cmake -DERUPTION_BUILD_TESTS=ON ..
   make -j$(nproc)
   ctest
   ```
7. Abra um Pull Request descrevendo o que foi alterado e por quê.

## 3. O que não aceitamos

- Código copiado de outro projeto sem licença compatível.
- Assets, sprites, mapas, modelos ou texturas protegidos por direitos autorais de terceiros.
- Conteúdo ofensivo, discriminatório ou ilegal.
- Mudanças que quebram a API pública sem discussão prévia.

## 4. Direitos e responsabilidades

- O titular do projeto mantém a decisão final sobre o que entra no repositório.
- Contribuidores mantêm os créditos autorais individuais sobre suas criações, mas concedem as permissões acima.
- O projeto é fornecido "no estado em que se encontra", sem garantias.

## 5. Dúvidas?

Abra uma issue ou entre em contato com o mantenedor do projeto.

---

**Nota legal:** este não é um documento jurídico formal. Para projetos com muitos contribuidores ou uso comercial significativo, recomenda-se tornar este CLA um documento assinado digitalmente (por exemplo, via CLA Assistant ou Docusign).
