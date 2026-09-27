#!/usr/bin/env python3
"""Bake offline de texturas -> .etex (BC1/BC3 + mips prontos).

O segredo do loading: tudo que a engine fazia por textura em runtime
(decode PNG/BMP, chroma key magenta, dilate, mip blit chain na GPU) é feito
UMA vez aqui e gravado pronto. A engine só lê e sobe (zero decode).

Formato .etex (little-endian):
    u32 magic 'ETX1' (0x31585445)
    u32 vkFormat        (131=BC1_RGBA_UNORM_BLOCK, 137=BC3_UNORM_BLOCK)
    u32 width, u32 height, u32 mipCount
    mipCount x { u32 byteSize, payload }

Uso:
    python3 tools/bake_assets.py               # assets/data/texture inteiro
    python3 tools/bake_assets.py --root <dir> --jobs 16 --force

Incremental por mtime (refaz só o que mudou). BC1 para RGB opaco, BC3
quando há alfa OU quando o alfa carrega dado (_mrahw: altura/wetness).
Compressão ~4-8x menor que RGBA8 em VRAM e upload.
"""

import argparse
import os
import struct
import sys
import time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
MAGIC = 0x31585445  # 'ETX1'
VK_FORMAT_BC1_RGBA_UNORM_BLOCK = 131
VK_FORMAT_BC3_UNORM_BLOCK = 137
VK_FORMAT_R8G8B8A8_UNORM = 37
VK_FORMAT_BC5_UNORM_BLOCK = 141
VK_FORMAT_BC7_UNORM_BLOCK = 145
DILATE_PASSES = 32  # cobre bleed até o mip mais fundo que importa

# Encoder BC7 externo (third_party/bc7enc_rdo). Mapas de DADOS empacotados
# (_mrahw: M/R/AO/H; _normal) NÃO podem usar a paleta conjunta do BC1/BC3 —
# canais independentes fora da reta RGB degradam feio (medido: RMS 9-14 na
# regressão visual). BC7 escolhe modos por bloco e resolve.
BC7ENC = ROOT / "third_party/bc7enc_rdo/build/bc7enc"


def magenta_key_and_dilate(rgba: np.ndarray) -> np.ndarray:
    """Chroma key magenta -> alpha 0 + dilatação da cor dos vizinhos opacos.
    Mesmo comportamento do ImageUtils::dilate da engine (BFS), vetorizado."""
    r, g, b, a = rgba[..., 0], rgba[..., 1], rgba[..., 2], rgba[..., 3]
    transparent = (a == 0) | ((r > 230) & (g < 25) & (b > 230))
    if not transparent.any():
        return rgba
    out = rgba.copy()
    out[..., 3] = np.where(transparent, 0, a)
    filled = ~transparent
    color = out[..., :3].astype(np.float32)
    for _ in range(DILATE_PASSES):
        if filled.all():
            break
        grew = False
        for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0),
                       (1, 1), (1, -1), (-1, 1), (-1, -1)):
            src_ok = np.roll(filled, (dy, dx), axis=(0, 1))
            if dx > 0: src_ok[:, :dx] = False
            if dx < 0: src_ok[:, dx:] = False
            if dy > 0: src_ok[:dy, :] = False
            if dy < 0: src_ok[dy:, :] = False
            fill_here = (~filled) & src_ok
            if fill_here.any():
                src_color = np.roll(color, (dy, dx), axis=(0, 1))
                color[fill_here] = src_color[fill_here]
                filled |= fill_here
                grew = True
        if not grew:
            break
    out[..., :3] = np.clip(color, 0, 255).astype(np.uint8)
    out[..., :3][~filled] = 0  # sem vizinho opaco: preto transparente
    return out


def to_565(rgb: np.ndarray) -> np.ndarray:
    r = (rgb[..., 0].astype(np.uint16) >> 3) << 11
    g = (rgb[..., 1].astype(np.uint16) >> 2) << 5
    b = rgb[..., 2].astype(np.uint16) >> 3
    return r | g | b


def encode_bc1_color(blocks: np.ndarray) -> np.ndarray:
    """blocks: (N,16,3) uint8 -> (N,8) bytes (modo 4 cores, endpoints min/max)."""
    n = blocks.shape[0]
    f = blocks.astype(np.float32)
    cmax = f.max(axis=1)  # (N,3)
    cmin = f.min(axis=1)
    e0 = to_565(np.clip(cmax, 0, 255).astype(np.uint8))
    e1 = to_565(np.clip(cmin, 0, 255).astype(np.uint8))
    # Garante e0 > e1 (modo 4 cores); bloco uniforme -> índices 0.
    swap = e0 < e1
    e0s, e1s = np.where(swap, e1, e0), np.where(swap, e0, e1)
    hi = np.where(swap[:, None], cmin, cmax)
    lo = np.where(swap[:, None], cmax, cmin)
    pal = np.stack([hi, lo, (2 * hi + lo) / 3, (hi + 2 * lo) / 3], axis=1)  # (N,4,3)
    d = ((f[:, :, None, :] - pal[:, None, :, :]) ** 2).sum(axis=3)  # (N,16,4)
    idx = d.argmin(axis=2).astype(np.uint32)  # (N,16)
    idx[e0s == e1s] = 0
    bits = np.zeros(n, dtype=np.uint32)
    for i in range(16):
        bits |= idx[:, i] << (2 * i)
    out = np.zeros((n, 8), dtype=np.uint8)
    out[:, 0] = e0s & 0xFF; out[:, 1] = e0s >> 8
    out[:, 2] = e1s & 0xFF; out[:, 3] = e1s >> 8
    for byte in range(4):
        out[:, 4 + byte] = (bits >> (8 * byte)) & 0xFF
    return out


def encode_bc4(vals: np.ndarray) -> np.ndarray:
    """vals: (N,16) uint8 (canal único) -> (N,8) bytes (BC4/alpha do BC3)."""
    n = vals.shape[0]
    f = vals.astype(np.float32)
    a0 = f.max(axis=1)
    a1 = f.min(axis=1)
    # Rampa de 8 valores (modo a0 > a1).
    w = np.array([0, 7, 6, 5, 4, 3, 2, 1], dtype=np.float32)
    pal = (a0[:, None] * (7 - w) + a1[:, None] * w) / 7.0  # ordem dos índices BC4
    pal[:, 0] = a0; pal[:, 1] = a1
    d = np.abs(f[:, :, None] - pal[:, None, :])
    idx = d.argmin(axis=2).astype(np.uint64)
    same = a0 == a1
    idx[same] = 0
    bits = np.zeros(n, dtype=np.uint64)
    for i in range(16):
        bits |= idx[:, i] << np.uint64(3 * i)
    out = np.zeros((n, 8), dtype=np.uint8)
    out[:, 0] = np.clip(a0, 0, 255).astype(np.uint8)
    out[:, 1] = np.clip(a1, 0, 255).astype(np.uint8)
    for byte in range(6):
        out[:, 2 + byte] = ((bits >> np.uint64(8 * byte)) & np.uint64(0xFF)).astype(np.uint8)
    return out


def blockify(img: np.ndarray) -> np.ndarray:
    """(H,W,C) -> (N,16,C) em blocos 4x4 (H,W múltiplos de 4)."""
    h, w = img.shape[:2]
    c = img.shape[2] if img.ndim == 3 else 1
    v = img.reshape(h // 4, 4, w // 4, 4, c)
    return v.transpose(0, 2, 1, 3, 4).reshape(-1, 16, c)


def encode_level(rgba: np.ndarray, use_bc3: bool) -> bytes:
    h, w = rgba.shape[:2]
    ph, pw = (h + 3) & ~3, (w + 3) & ~3
    if (ph, pw) != (h, w):  # pad borda replicada
        padded = np.empty((ph, pw, 4), dtype=np.uint8)
        padded[:h, :w] = rgba
        padded[h:, :w] = rgba[h - 1:h, :]
        padded[:h, w:] = rgba[:, w - 1:w]
        padded[h:, w:] = rgba[h - 1, w - 1]
        rgba = padded
    blocks = blockify(rgba)
    color = encode_bc1_color(blocks[..., :3])
    if not use_bc3:
        return color.tobytes()
    alpha = encode_bc4(blocks[..., 3].reshape(-1, 16))
    inter = np.concatenate([alpha, color], axis=1)  # BC3 = 8B alfa + 8B cor
    return inter.tobytes()


def encode_level_ext(rgba: np.ndarray, tmpdir: Path, tag: str, args: list) -> bytes:
    """Codifica um nível via bc7enc externo (DDS -> blocos crus).
    args: ["-u4"] = BC7; ["-5", "-f"] = BC5 (normals; -f força header DX10)."""
    import subprocess
    h, w = rgba.shape[:2]
    ph, pw = (h + 3) & ~3, (w + 3) & ~3
    if (ph, pw) != (h, w):
        padded = np.empty((ph, pw, 4), dtype=np.uint8)
        padded[:h, :w] = rgba
        padded[h:, :w] = rgba[h - 1:h, :]
        padded[:h, w:] = rgba[:, w - 1:w]
        padded[h:, w:] = rgba[h - 1, w - 1]
        rgba = padded
    png = tmpdir / f"{tag}.png"
    dds = tmpdir / f"{tag}.dds"
    Image.fromarray(rgba).save(png)
    r = subprocess.run([str(BC7ENC), "-q", "-g"] + args + [str(png), str(dds)],
                       capture_output=True, timeout=300)
    if r.returncode != 0 or not dds.exists():
        raise RuntimeError(f"bc7enc falhou: {r.stderr.decode()[:200]}")
    data = dds.read_bytes()
    # DDS: 4 magic + 124 header (+20 se fourCC == 'DX10').
    off = 148 if data[84:88] == b"DX10" else 128
    blocks = data[off:]
    expected = (rgba.shape[0] // 4) * (rgba.shape[1] // 4) * 16
    if len(blocks) < expected:
        raise RuntimeError(f"DDS truncado ({len(blocks)} < {expected})")
    png.unlink(missing_ok=True)
    dds.unlink(missing_ok=True)
    return blocks[:expected]


def poisson_height_from_normal(normal_rgba: np.ndarray) -> np.ndarray:
    """Frankot-Chellappa: integra o campo de gradientes do normal map via FFT
    (resolve o Poisson 'de que altura vieram esses normais?'). Retorna altura
    normalizada [0,1] uint8 — vai no alpha do MRAH-W para o displacement
    híbrido (docs/displacement_design.md)."""
    n = normal_rgba[..., :3].astype(np.float32) / 255.0 * 2.0 - 1.0
    nz = np.clip(np.abs(n[..., 2]), 0.1, 1.0)
    gx = -n[..., 0] / nz
    gy = -n[..., 1] / nz
    h, w = gx.shape
    fy = np.fft.fftfreq(h)[:, None]
    fx = np.fft.fftfreq(w)[None, :]
    denom = (2j * np.pi * fx) ** 2 + (2j * np.pi * fy) ** 2
    denom[0, 0] = 1.0
    F = (2j * np.pi * fx) * np.fft.fft2(gx) + (2j * np.pi * fy) * np.fft.fft2(gy)
    Z = np.real(np.fft.ifft2(F / denom))

    # HIGH-PASS obrigatório: a integração devolve também a RAMPA de baixa
    # frequência do tile (gradiente médio != 0). Rampa no campo de altura faz
    # o POM deslocar a TEXTURA INTEIRA em bloco quando a câmera gira — o
    # "tudo desliza" reportado no audit. Só o detalhe de alta frequência
    # (pedra, junta, grão) é relevo de verdade; a forma grande é geometria.
    h, w = Z.shape
    sigma = max(h, w) / 8.0
    fy2 = (np.fft.fftfreq(h)[:, None] * h / sigma) ** 2
    fx2 = (np.fft.fftfreq(w)[None, :] * w / sigma) ** 2
    lowpass = np.exp(-0.5 * (fx2 + fy2))          # gaussiana no domínio de freq
    Z = Z - np.real(np.fft.ifft2(np.fft.fft2(Z) * lowpass))

    # Normaliza pela dispersão (não pelo min/max, que um outlier domina) e
    # centra em 0.5: relevo simétrico em torno da superfície.
    s = Z.std()
    Z = (Z / (4.0 * s) + 0.5) if s > 1e-6 else np.full_like(Z, 0.5)
    # 254 é o teto: 255 = flag "sem altura" no shader.
    return np.clip(Z * 254.0, 0.0, 254.0).astype(np.uint8)


def half_box(x: np.ndarray) -> np.ndarray:
    """Média 2x2 STRAIGHT (não-premultiplicada), idêntica ao vkCmdBlitImage
    LINEAR da engine. NUNCA usar PIL.resize aqui: o PIL premultiplica RGB por
    alpha no resize, e os normal maps do acervo carregam materialProps no
    alpha (a=1) — os mips viravam lixo magenta (root cause do RMS 9+ que
    custou 5 rodadas de suspeita nos codecs)."""
    h, w = x.shape[:2]
    nh, nw = max(1, h // 2), max(1, w // 2)
    x = x[:nh * 2 if h > 1 else 1, :nw * 2 if w > 1 else 1]
    acc = x.astype(np.float32)
    if h > 1:
        acc = (acc[0::2] + acc[1::2]) * 0.5
    if w > 1:
        acc = (acc[:, 0::2] + acc[:, 1::2]) * 0.5
    return np.clip(acc + 0.5, 0, 255).astype(np.uint8)


def bake_texture(src: Path, dst: Path, is_data_alpha: bool) -> str:
    try:
        img = Image.open(src).convert("RGBA")
    except Exception as e:  # noqa: BLE001
        return f"SKIP {src.name}: {e}"
    rgba = np.asarray(img, dtype=np.uint8).copy()
    is_normal = src.stem.endswith("_normal")
    is_mrahw = src.stem.endswith("_mrahw")

    # Displacement híbrido (docs/displacement_design.md): mrahw sem altura
    # cozida (alpha todo 255 = flag "sem dado") ganha altura reconstruída do
    # normal map irmão por integração de Poisson (Frankot-Chellappa). Teto em
    # 254: 255 continua sendo a flag de "sem altura" no shader.
    if is_mrahw and int(rgba[..., 3].min()) == 255:
        sib = src.with_name(src.name.replace("_mrahw", "_normal"))
        if sib.exists():
            try:
                nimg = Image.open(sib).convert("RGBA")
                if nimg.size != (rgba.shape[1], rgba.shape[0]):
                    nimg = nimg.resize((rgba.shape[1], rgba.shape[0]), Image.BILINEAR)
                narr = np.asarray(nimg, dtype=np.uint8)
                rgba[..., 3] = poisson_height_from_normal(narr)
            except Exception:
                pass  # mantém 255 (sem altura); POM só não liga nesse material
    use_ext = is_data_alpha and BC7ENC.exists()
    if not is_data_alpha:
        # Albedo: chroma key + dilate (o alfa de _mrahw/_normal é DADO, não cobertura).
        rgba = magenta_key_and_dilate(rgba)
    has_alpha = bool((rgba[..., 3] != 255).any())
    use_bc3 = has_alpha or is_data_alpha
    ext_args = []
    if is_normal:
        # Normal map: RGBA8 RAW (só mips prontos, zero decode no load).
        # BC5+reconstrução-z e BC7 foram testados e REPROVADOS: o acervo tem
        # normals não-unitários (blue ~239 flat) e o look do jogo depende da
        # convenção; codec de bloco nesses maps deu RMS de tela 9-17.
        fmt = VK_FORMAT_R8G8B8A8_UNORM
        use_ext = False
    elif use_ext:
        # MRAH-W (dados empacotados): BC7.
        ext_args = ["-u6"]
        fmt = VK_FORMAT_BC7_UNORM_BLOCK
    else:
        fmt = VK_FORMAT_BC3_UNORM_BLOCK if use_bc3 else VK_FORMAT_BC1_RGBA_UNORM_BLOCK

    tmpdir = None
    if use_ext:
        import tempfile
        tmpdir = Path(tempfile.mkdtemp(prefix="bcx_"))
    levels = []
    cur = rgba
    try:
        lvl = 0
        while True:
            if is_normal:
                levels.append(cur.tobytes())  # RGBA8 raw
            elif use_ext:
                levels.append(encode_level_ext(cur, tmpdir, f"m{lvl}", ext_args))
            else:
                levels.append(encode_level(cur, use_bc3))
            h, w = cur.shape[:2]
            # Até 1x1: a engine amostra com mip bias (u_normalSmoothing) e uma
            # cadeia curta deixa os mips fundos mais nítidos que o runtime
            # (relevo mais forte no zoom-out — medido RMS 9+ na regressão).
            if max(h, w) <= 1:
                break
            cur = half_box(cur)
            lvl += 1
    finally:
        if tmpdir is not None:
            import shutil
            shutil.rmtree(tmpdir, ignore_errors=True)

    dst.parent.mkdir(parents=True, exist_ok=True)
    with open(dst, "wb") as f:
        f.write(struct.pack("<5I", MAGIC, fmt, rgba.shape[1], rgba.shape[0],
                            len(levels)))
        for lv in levels:
            f.write(struct.pack("<I", len(lv)))
            f.write(lv)
    fmt_name = ("RGBA8" if is_normal
                else ("BC7" if use_ext else ("BC3" if use_bc3 else "BC1")))
    return f"OK {src.name} ({fmt_name}, {len(levels)} mips)"


def job(args):
    src, dst, is_data = args
    return bake_texture(Path(src), Path(dst), is_data)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=str(ROOT / "assets/data/texture"))
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--limit", type=int, default=0, help="bake só N arquivos (teste)")
    a = ap.parse_args()

    root = Path(a.root)
    todo = []
    for src in sorted(root.rglob("*")):
        if src.suffix.lower() not in (".png", ".bmp", ".jpg", ".jpeg"):
            continue
        dst = src.with_suffix(src.suffix + ".etex")
        if not a.force and dst.exists() and dst.stat().st_mtime >= src.stat().st_mtime:
            continue
        is_data = src.stem.endswith(("_mrahw", "_normal"))
        todo.append((str(src), str(dst), is_data))
        if a.limit and len(todo) >= a.limit:
            break

    print(f"{len(todo)} texturas para bakear ({a.jobs} jobs)")
    t0 = time.time()
    fail = 0
    with ProcessPoolExecutor(max_workers=a.jobs) as ex:
        for i, res in enumerate(ex.map(job, todo, chunksize=8)):
            if res.startswith("SKIP"):
                fail += 1
                print(res)
            if (i + 1) % 500 == 0:
                print(f"  {i + 1}/{len(todo)} ({time.time() - t0:.0f}s)")
    print(f"pronto: {len(todo) - fail} ok, {fail} pulados, {time.time() - t0:.0f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
