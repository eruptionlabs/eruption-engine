#!/usr/bin/env python3
"""Encolhe um .glb para o APK: texturas opacas viram JPEG, as com alfa PNG de paleta.

    shrink_glb.py entrada.glb saida.glb [qualidade]

Imagem com qualquer pixel de alfa < 255 continua PNG, com paleta de 256
cores (recorte e vidro). As demais viram JPEG (padrao 88). O resto do arquivo (malha, animacao,
materiais) e' copiado como esta'; so' os bufferViews sao reempacotados.
"""
import io
import json
import struct
import sys

from PIL import Image


def main():
    src, dst = sys.argv[1], sys.argv[2]
    quality = int(sys.argv[3]) if len(sys.argv) > 3 else 88
    raw = open(src, 'rb').read()
    magic, version, _ = struct.unpack_from('<4sII', raw, 0)
    if magic != b'glTF':
        sys.exit(f'{src}: nao e glb')
    jlen, _ = struct.unpack_from('<II', raw, 12)
    doc = json.loads(raw[20:20 + jlen])
    blen, _ = struct.unpack_from('<II', raw, 20 + jlen)
    binc = raw[28 + jlen:28 + jlen + blen]

    views = [binc[v.get('byteOffset', 0):v.get('byteOffset', 0) + v['byteLength']] for v in doc['bufferViews']]
    before = sum(len(v) for v in views)
    converted = 0
    for img in doc.get('images', []):
        if 'bufferView' not in img or img.get('mimeType') == 'image/jpeg':
            continue
        data = views[img['bufferView']]
        im = Image.open(io.BytesIO(data))
        if im.mode in ('RGBA', 'LA', 'P'):
            rgba = im.convert('RGBA')
            if rgba.getchannel('A').getextrema()[0] < 255:
                # Com recorte: PNG de paleta (256 cores com alfa), como as
                # texturas de 8 bits que esses mapas usam de origem.
                out = io.BytesIO()
                # method=2: FASTOCTREE, o unico quantizador do PIL que aceita alfa.
                # Sem dither: pontilhado no alfa vira recorte granulado (folhas).
                rgba.quantize(256, method=2, dither=0).save(out, 'PNG', optimize=True)
                if 0 < out.tell() < len(data):
                    views[img['bufferView']] = out.getvalue()
                    converted += 1
                continue
            im = rgba
        out = io.BytesIO()
        im.convert('RGB').save(out, 'JPEG', quality=quality, optimize=True)
        if 0 < out.tell() < len(data):
            views[img['bufferView']] = out.getvalue()
            img['mimeType'] = 'image/jpeg'
            converted += 1

    blob = bytearray()
    for i, v in enumerate(views):
        while len(blob) % 4:
            blob.append(0)
        doc['bufferViews'][i]['byteOffset'] = len(blob)
        doc['bufferViews'][i]['byteLength'] = len(v)
        blob += v
    while len(blob) % 4:
        blob.append(0)
    doc['buffers'][0]['byteLength'] = len(blob)

    js = json.dumps(doc, separators=(',', ':')).encode()
    js += b' ' * (-len(js) % 4)
    total = 12 + 8 + len(js) + 8 + len(blob)
    with open(dst, 'wb') as f:
        f.write(struct.pack('<4sII', b'glTF', version, total))
        f.write(struct.pack('<I4s', len(js), b'JSON'))
        f.write(js)
        f.write(struct.pack('<I4s', len(blob), b'BIN\0'))
        f.write(blob)
    print(f'{src}: {converted} texturas recodificadas, binario {before / 1e6:.1f} -> {len(blob) / 1e6:.1f} MB')


if __name__ == '__main__':
    main()
