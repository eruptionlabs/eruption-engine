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

from PIL import Image, ImageChops


def cutout_palette_png(rgba):
    """PNG de paleta para recorte binario (alfa so' 0 ou 255), ou None.

    Os pixels transparentes guardam a cor da vizinhanca (dilatacao feita na
    conversao do mapa): o filtro linear e os mips da borda leem essa cor. Uma
    paleta comum com alfa junta todos os transparentes numa entrada (quase
    sempre preta) e a borda do recorte ganha contorno escuro. Aqui a cor vai
    em 128 entradas e a paleta e' duplicada: as 128 de cima sao as mesmas
    cores com alfa 0. Alfa com meio-termo (vidro) fica como veio (None).
    """
    alpha = rgba.getchannel('A')
    if alpha.point(lambda v: 0 if v in (0, 255) else 255).getextrema()[1] > 0:
        return None
    rgb = rgba.convert("RGB").quantize(128, method=0, dither=0)  # 0 = MEDIANCUT, sem dither
    palette = rgb.getpalette()[:128 * 3]
    palette += [0] * (128 * 3 - len(palette))
    index = Image.frombytes('L', rgb.size, rgb.tobytes())  # indices crus 0..127
    transparent = alpha.point(lambda v: 0 if v == 255 else 128)
    out_img = Image.frombytes('P', rgb.size, ImageChops.add(index, transparent).tobytes())
    out_img.putpalette(palette + palette)
    out = io.BytesIO()
    out_img.save(out, 'PNG', optimize=True, transparency=bytes([255] * 128 + [0] * 128))
    return out.getvalue()


def has_color_key(im):
    """Algum pixel exatamente FF00FF (a chave de recorte dos mapas)."""
    r, g, b = im.convert('RGB').split()
    mask = ImageChops.multiply(ImageChops.multiply(r.point(lambda v: 255 if v == 255 else 0),
                                                   g.point(lambda v: 255 if v == 0 else 0)),
                               b.point(lambda v: 255 if v == 255 else 0))
    return mask.getextrema()[1] > 0


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
        if has_color_key(im):
            # Rosa puro (FF00FF) e' chave de recorte no shader: JPEG e paleta
            # mexem nessa cor e o recorte vira rosa visivel com contorno escuro.
            continue
        if im.mode in ('RGBA', 'LA', 'P'):
            rgba = im.convert('RGBA')
            if rgba.getchannel('A').getextrema()[0] < 255:
                out = cutout_palette_png(rgba)
                if out is not None and 0 < len(out) < len(data):
                    views[img['bufferView']] = out
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
