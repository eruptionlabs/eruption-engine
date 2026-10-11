#!/usr/bin/env python3
"""Janelas e vidros que acendem de noite: gera emissiveTexture no .glb.

    emissive_windows.py entrada.glb saida.glb marcacoes.json [previa.png]

Precisa de numpy e Pillow.

marcacoes.json diz, para cada textura (trecho do nome da imagem), onde estao
as janelas e quais texturas sao vidro claro de lampiao:

    {
      "janelas": {"casa_frente": [[u, v, meia_largura, meia_altura], ...]},
      "vidros":  ["lampiao"],
      "cor_janela": [1.0, 0.72, 0.36],
      "cor_vidro":  [1.0, 0.82, 0.5]
    }

u, v na imagem (0..1, v de cima para baixo); as meias medidas tambem em 0..1.

O formato do brilho sai da propria textura:
  - janela: dentro da caixa, quanto mais ESCURO o pixel, mais brilha
    (1,1 - luminancia, piso 0,15). O vidro preto vira cor quente forte, a
    grade e a moldura - mais claras - ficam fracas, e a borda da caixa
    esmaece em 15% da medida. A caixa so' precisa cobrir a janela; o
    desenho fino quem da' e' a textura.
  - vidro de lampiao: o contrario, os paineis CLAROS acendem
    (luminancia 0,5 -> 0,7 vira 0 -> 1).
A mascara passa por um desfoque leve, vira PNG RGB (cor x mascara) e entra
como emissiveTexture (emissiveFactor 1) em todo material que usa a imagem.
O motor acende o emissivo com a mesma curva das luzes artificiais
(model_frag.glsl), entao some de dia.
"""
import io
import json
import struct
import sys

import numpy as np
from PIL import Image, ImageFilter


def luminance(rgba):
    return (rgba[..., :3].astype(np.float32) @ np.array([0.299, 0.587, 0.114], np.float32)) / 255.0


def window_mask(rgba, boxes):
    h, w = rgba.shape[:2]
    lum = luminance(rgba)
    yy, xx = np.mgrid[0:h, 0:w]
    m = np.zeros((h, w), np.float32)
    for u, v, hw, hh in boxes:
        dx = np.abs((xx + 0.5) / w - u) / hw
        dy = np.abs((yy + 0.5) / h - v) / hh
        edge = np.clip((1.0 - np.maximum(dx, dy)) / 0.15, 0.0, 1.0)
        m = np.maximum(m, edge * np.clip(1.1 - lum, 0.15, 1.0))
    return m


def glass_mask(rgba):
    return np.clip((luminance(rgba) - 0.5) / 0.2, 0.0, 1.0)


def encode(mask, color):
    blurred = Image.fromarray((mask * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(0.6))
    m = np.asarray(blurred, np.float32) / 255.0
    rgb = (m[..., None] * np.array(color, np.float32)[None, None, :] * 255).clip(0, 255).astype(np.uint8)
    out = io.BytesIO()
    Image.fromarray(rgb, 'RGB').save(out, 'PNG', optimize=True)
    return out.getvalue(), m


def main():
    src, dst, marks_path = sys.argv[1:4]
    preview_path = sys.argv[4] if len(sys.argv) > 4 else None
    marks = json.load(open(marks_path))
    windows = marks.get('janelas', {})
    glasses = marks.get('vidros', [])
    window_color = marks.get('cor_janela', [1.0, 0.72, 0.36])
    glass_color = marks.get('cor_vidro', [1.0, 0.82, 0.5])

    raw = open(src, 'rb').read()
    jlen = struct.unpack_from('<I', raw, 12)[0]
    doc = json.loads(raw[20:20 + jlen])
    blen = struct.unpack_from('<I', raw, 20 + jlen)[0]
    blob = bytearray(raw[28 + jlen:28 + jlen + blen])

    def image_rgba(i):
        view = doc['bufferViews'][doc['images'][i]['bufferView']]
        off = view.get('byteOffset', 0)
        return np.asarray(Image.open(io.BytesIO(bytes(blob[off:off + view['byteLength']]))).convert('RGBA'))

    names = [im.get('name', '') for im in doc['images']]
    made, previews = {}, []
    for i, name in enumerate(names):
        if 'bufferView' not in doc['images'][i]:
            continue
        key = next((k for k in windows if k in name), None)
        if key is not None:
            rgba = image_rgba(i)
            mask, color = window_mask(rgba, windows[key]), window_color
        elif any(g in name for g in glasses):
            rgba = image_rgba(i)
            mask, color = glass_mask(rgba), glass_color
        else:
            continue
        if mask.max() <= 0.0:
            continue
        made[i], final = encode(mask, color)
        previews.append((rgba, final))

    texture_of = {}
    for i, png in made.items():
        blob.extend(b'\0' * (-len(blob) % 4))
        doc['bufferViews'].append({'buffer': 0, 'byteOffset': len(blob), 'byteLength': len(png)})
        blob.extend(png)
        doc['images'].append({'name': 'emissiva_' + names[i].strip('_#'), 'mimeType': 'image/png',
                              'bufferView': len(doc['bufferViews']) - 1})
        doc['textures'].append({'source': len(doc['images']) - 1})
        texture_of[i] = len(doc['textures']) - 1
    blob.extend(b'\0' * (-len(blob) % 4))
    doc['buffers'][0]['byteLength'] = len(blob)

    materials = 0
    for mat in doc.get('materials', []):
        t = mat.get('pbrMetallicRoughness', {}).get('baseColorTexture', {}).get('index')
        if t is not None and doc['textures'][t]['source'] in texture_of:
            mat['emissiveTexture'] = {'index': texture_of[doc['textures'][t]['source']]}
            mat['emissiveFactor'] = [1.0, 1.0, 1.0]
            materials += 1

    js = json.dumps(doc, separators=(',', ':')).encode()
    js += b' ' * (-len(js) % 4)
    with open(dst, 'wb') as f:
        f.write(struct.pack('<4sII', b'glTF', 2, 12 + 8 + len(js) + 8 + len(blob)))
        f.write(struct.pack('<I4s', len(js), b'JSON'))
        f.write(js)
        f.write(struct.pack('<I4s', len(blob), b'BIN\0'))
        f.write(blob)
    print(f'{len(made)} texturas emissivas em {materials} materiais -> {dst}')

    if preview_path and previews:
        cell = 192
        sheet = Image.new('RGB', (cell * 8, cell * ((len(previews) + 3) // 4)))
        for k, (rgba, m) in enumerate(previews):
            x, y = (k % 4) * cell * 2, (k // 4) * cell
            sheet.paste(Image.fromarray(rgba).convert('RGB').resize((cell, cell)), (x, y))
            sheet.paste(Image.fromarray((m * 255).astype(np.uint8)).convert('RGB').resize((cell, cell)), (x + cell, y))
        sheet.save(preview_path)


if __name__ == '__main__':
    main()
