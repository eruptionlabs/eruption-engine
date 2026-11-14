#!/usr/bin/env python3
"""Tira arquivos do cache de paginas do sistema, sem root, para medir carga
com o disco "frio" (o que acontece na primeira abertura depois de ligar a
maquina).

Usa posix_fadvise(POSIX_FADV_DONTNEED) em cada arquivo (pastas sao
percorridas). Com --device tambem esvazia o cache do dispositivo de bloco por
tras de cada caminho: num NTFS via FUSE (ntfs-3g) o servidor le' o disco pelo
dispositivo, entao os dados e a tabela de arquivos (MFT) ficam tambem no cache
dele - so' tirar o arquivo deixaria a segunda leitura quente. Abrir o
dispositivo so' para leitura pede o grupo "disk"; sem ele, avisa e segue.

Uso:
  tools/evict_page_cache.py [--device] <arquivo-ou-pasta> [...]
  tools/evict_page_cache.py --device --emap <mapa.emap>   (o pacote e as fontes dele)
"""
import os
import struct
import sys


def evict_file(path):
    try:
        fd = os.open(path, os.O_RDONLY)
    except OSError:
        return 0
    try:
        os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
        return 1
    finally:
        os.close(fd)


def walk(path):
    if os.path.isdir(path):
        for root, _dirs, files in os.walk(path):
            for f in files:
                yield os.path.join(root, f)
    elif os.path.exists(path):
        yield path


def device_of(path):
    """Dispositivo de bloco montado sob o caminho (maior ponto de montagem que o contem)."""
    real = os.path.realpath(path)
    best, dev = "", None
    with open("/proc/mounts") as m:
        for line in m:
            parts = line.split()
            if len(parts) < 2 or not parts[0].startswith("/dev/"):
                continue
            mnt = parts[1].replace("\\040", " ")
            if (real == mnt or real.startswith(mnt.rstrip("/") + "/")) and len(mnt) > len(best):
                best, dev = mnt, parts[0]
    return dev


def emap_sources(emap):
    """Arquivos de origem guardados no indice de um .emap (ver src/utils/AssetVfs.cpp)."""
    out = [emap]
    with open(emap, "rb") as f:
        head = f.read(64)
        if head[:8] != b"ERUPEMAP":
            return out
        count, = struct.unpack_from("<I", head, 12)
        toc_off, toc_size = struct.unpack_from("<QQ", head, 16)
        f.seek(toc_off)
        toc = f.read(toc_size)
    base = os.path.dirname(os.path.abspath(emap))
    roots = {}
    c = 0
    nroots, = struct.unpack_from("<I", toc, c)
    c += 4
    for i in range(nroots):
        n, = struct.unpack_from("<H", toc, c)
        c += 2
        rel = toc[c:c + n].decode("utf-8", "replace")
        c += n
        roots[i] = os.path.normpath(os.path.join(base, rel))
    for _ in range(count):
        n, = struct.unpack_from("<H", toc, c)
        c += 2
        key = toc[c:c + n].decode("utf-8", "replace")
        c += n
        root, kind = struct.unpack_from("<BB", toc, c)
        c += 2 + 1 + 8 * 6  # codec + offset, stored, raw, hash, srcSize, srcMtime
        if kind != 2:
            out.append(os.path.join(roots.get(root, base), key))
    return out


def main(argv):
    device = "--device" in argv
    args = [a for a in argv if a != "--device"]
    paths = []
    i = 0
    while i < len(args):
        if args[i] == "--emap" and i + 1 < len(args):
            paths += emap_sources(args[i + 1])
            i += 2
            continue
        paths.append(args[i])
        i += 1
    if not paths:
        print(__doc__)
        return 1
    n = 0
    devs = set()
    for p in paths:
        for f in walk(p):
            n += evict_file(f)
        if device:
            d = device_of(p)
            if d:
                devs.add(d)
    msg = "%d arquivos fora do cache" % n
    for d in sorted(devs):
        try:
            fd = os.open(d, os.O_RDONLY)
            os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
            os.close(fd)
            msg += ", dispositivo %s esvaziado" % d
        except OSError as e:
            msg += ", dispositivo %s: %s" % (d, e.strerror)
    print(msg)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
