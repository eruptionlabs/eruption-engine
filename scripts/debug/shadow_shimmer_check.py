#!/usr/bin/env python3
"""shadow_shimmer_check.py — valida o shimmer da sombra do sol (tint magenta).

Uso:
    ../tools/venv/bin/python3 shadow_shimmer_check.py img0.png img1.png [img2.png ...]

As imagens devem ser capturas com ERUPTION_TEST_SHADOW_TINT=0.9 da MESMA cena com o
cam target deslocado algumas unidades (simulando movimento WASD da free cam).

Saida:
  - % de pixels magenta por imagem (cobertura da sombra do sol)
  - % de pixels que MUDAM de estado magenta entre capturas consecutivas
    (isso e o shimmer: sombra estavel nao muda quase nada)
  - mapa de shimmer salvo em /tmp/shimmer_diff_<i>.png (branco = mudou)
  - cobertura por faixa vertical (tercos da tela) pra localizar o shimmer
"""
import sys
from PIL import Image


def magenta_mask(im):
    """Pixels pintados de magenta pelo ERUPTION_TEST_SHADOW_TINT."""
    r, g, b = im.convert('RGB').split()
    w, h = im.size
    pr, pg, pb = r.load(), g.load(), b.load()
    mask = []
    for y in range(h):
        row = []
        for x in range(w):
            # magenta tolerante: R e B altos, G bem menor
            row.append(1 if (pr[x, y] > 150 and pb[x, y] > 150 and pg[x, y] < pr[x, y] * 0.6) else 0)
        mask.append(row)
    return mask, w, h


def stats(masks):
    n = len(masks[0][0]) * len(masks[0])
    for i, m in enumerate(masks):
        cov = sum(sum(row) for row in m) / n * 100
        print(f'img{i}: cobertura magenta = {cov:.1f}%')

    h = len(masks[0])
    for i in range(len(masks) - 1):
        a, b = masks[i], masks[i + 1]
        diff = Image.new('L', (len(a[0]), h), 0)
        dp = diff.load()
        flips = 0
        tercos = [[0, 0] for _ in range(3)]  # [flips, total] por terco vertical
        for y in range(h):
            t = min(2, y * 3 // h)
            for x in range(len(a[0])):
                tercos[t][1] += 1
                if a[y][x] != b[y][x]:
                    flips += 1
                    tercos[t][0] += 1
                    dp[x, y] = 255
        out = f'/tmp/shimmer_diff_{i}.png'
        diff.save(out)
        print(f'img{i} -> img{i+1}: shimmer = {flips / n * 100:.2f}% dos pixels  (mapa: {out})')
        for t, (f, tot) in enumerate(tercos):
            nome = ['topo', 'meio', 'base'][t]
            print(f'    {nome:5s}: {f / tot * 100:.2f}%')


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    masks = []
    for path in sys.argv[1:]:
        m, w, h = magenta_mask(Image.open(path))
        masks.append(m)
    stats(masks)


if __name__ == '__main__':
    main()
