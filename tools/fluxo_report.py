#!/usr/bin/env python3
"""Relatorio do medidor de banda (spec 50, "Fluxo Frio") a partir da telemetria.

  fluxo_report.py <telemetria.jsonl | pasta da bancada> [--de N] [--json]
  fluxo_report.py compara <A> <B> [--de N]

Le o bloco "bw" que o motor grava por quadro com ERUPTION_TELEMETRY_OUT (a
bancada liga sozinha) e resume, fora dos quadros de carga:
  - estimativa dos anexos (formato x area x load/store) por passe, em MB/quadro;
  - DRAM medida pelo controlador de memoria (Intel, Linux; sistema inteiro),
    em MB/quadro e GB/s - na iGPU e' o trafego real do motor + CPU + tela;
  - ocupacao da VRAM pelo NVML (NVIDIA; % do tempo, nao GB/s);
  - tempo de GPU por passe (o mesmo nome do passe do medidor).
"compara" mostra B - A e a variacao percentual de cada numero.
So' usa a biblioteca padrao.
"""
import json
import os
import statistics
import sys


def _arquivo(p):
    return os.path.join(p, "telemetria.jsonl") if os.path.isdir(p) else p


def carrega(p, de=0):
    quadros = []
    with open(_arquivo(p)) as fh:
        for linha in fh:
            try:
                q = json.loads(linha)
            except ValueError:
                continue
            if "bw" not in q or q.get("load", {}).get("loading"):
                continue
            if q.get("frame", 0) < de:
                continue
            quadros.append(q)
    return quadros


def media(v):
    return statistics.fmean(v) if v else float("nan")


def resume(quadros):
    r = {"quadros": len(quadros)}
    if not quadros:
        return r
    bw = [q["bw"] for q in quadros]
    r["gpu_total_ms"] = media([q.get("gpu_total_ms", 0.0) for q in quadros])
    r["frame_dt_ms"] = media([q.get("frame_dt_ms", 0.0) for q in quadros])
    r["est_read_mb"] = media([b["read_mb"] for b in bw])
    r["est_write_mb"] = media([b["write_mb"] for b in bw])
    dr = [b for b in bw if "dram_read_mb" in b and b.get("dram_s", 0) > 0]
    if dr:
        s = sum(b["dram_s"] for b in dr)
        rd = sum(b["dram_read_mb"] for b in dr)
        wr = sum(b["dram_write_mb"] for b in dr)
        r["dram_read_mb_q"] = rd / len(dr)
        r["dram_write_mb_q"] = wr / len(dr)
        r["dram_total_mb_q"] = (rd + wr) / len(dr)
        r["dram_gbs"] = (rd + wr) * 1024 * 1024 / s / 1e9
    busy = [b["vram_busy_pct"] for b in bw if "vram_busy_pct" in b]
    if busy:
        r["vram_busy_pct"] = media(busy)
    passes = {}
    for q in quadros:
        for n, (rd, wr) in q["bw"].get("passes", {}).items():
            passes.setdefault(n, {"r": [], "w": []})
            passes[n]["r"].append(rd)
            passes[n]["w"].append(wr)
    gpu = {}
    for q in quadros:
        for n, ms in q.get("gpu", {}).items():
            gpu.setdefault(n, []).append(ms)
    r["passes"] = {n: {"read_mb": sum(v["r"]) / len(quadros), "write_mb": sum(v["w"]) / len(quadros),
                       "gpu_ms": media(gpu.get(n, []))} for n, v in passes.items()}
    r["gpu"] = {n: media(v) for n, v in gpu.items()}
    r["audit"] = bw[-1].get("audit", {})
    return r


def imprime(nome, r):
    print(f"== {nome}: {r['quadros']} quadros")
    if not r["quadros"]:
        return
    print(f"gpu_total {r['gpu_total_ms']:.3f} ms  quadro {r['frame_dt_ms']:.2f} ms")
    print(f"estimativa anexos: le {r['est_read_mb']:.1f} MB + grava {r['est_write_mb']:.1f} MB por quadro")
    if "dram_gbs" in r:
        print(f"DRAM medida: {r['dram_read_mb_q']:.1f} MB lidos + {r['dram_write_mb_q']:.1f} MB gravados por quadro"
              f" = {r['dram_total_mb_q']:.1f} MB/quadro, {r['dram_gbs']:.2f} GB/s")
    if "vram_busy_pct" in r:
        print(f"VRAM ocupada (NVML): {r['vram_busy_pct']:.1f}% do tempo")
    print(f"{'passe':28} {'le MB':>8} {'grava MB':>9} {'gpu ms':>8}")
    for n, p in sorted(r["passes"].items(), key=lambda kv: -(kv[1]["read_mb"] + kv[1]["write_mb"])):
        ms = p["gpu_ms"]
        print(f"{n:28} {p['read_mb']:8.2f} {p['write_mb']:9.2f} {ms if ms == ms else 0.0:8.3f}")
    a = r.get("audit", {})
    if a:
        print(f"auditoria: {a.get('targets')} alvos, {a.get('flagged')} com flag, {a.get('warnings')} avisos")


def _pct(a, b):
    return 100.0 * (b - a) / a if a else float("nan")


def compara(a, b):
    print(f"{'numero':28} {'A':>10} {'B':>10} {'B-A':>10} {'%':>7}")
    for k in ("gpu_total_ms", "frame_dt_ms", "est_read_mb", "est_write_mb", "dram_read_mb_q", "dram_write_mb_q",
              "dram_total_mb_q", "dram_gbs", "vram_busy_pct"):
        if k in a and k in b:
            print(f"{k:28} {a[k]:10.3f} {b[k]:10.3f} {b[k] - a[k]:+10.3f} {_pct(a[k], b[k]):+7.1f}")
    nomes = sorted(set(a.get("gpu", {})) | set(b.get("gpu", {})))
    for n in nomes:
        x, y = a["gpu"].get(n, 0.0), b["gpu"].get(n, 0.0)
        if max(x, y) >= 0.05:
            print(f"gpu {n[:24]:24} {x:10.3f} {y:10.3f} {y - x:+10.3f} {_pct(x, y):+7.1f}")
    for n in sorted(set(a.get("passes", {})) | set(b.get("passes", {}))):
        pa = a["passes"].get(n, {"read_mb": 0.0, "write_mb": 0.0})
        pb = b["passes"].get(n, {"read_mb": 0.0, "write_mb": 0.0})
        x, y = pa["read_mb"] + pa["write_mb"], pb["read_mb"] + pb["write_mb"]
        if x or y:
            print(f"MB {n[:25]:25} {x:10.2f} {y:10.2f} {y - x:+10.2f} {_pct(x, y):+7.1f}")


def main():
    args = sys.argv[1:]
    de = 0
    if "--de" in args:
        i = args.index("--de")
        de = int(args[i + 1])
        del args[i:i + 2]
    js = "--json" in args
    args = [x for x in args if x != "--json"]
    if not args:
        print(__doc__)
        return 2
    if args[0] == "compara" and len(args) == 3:
        compara(resume(carrega(args[1], de)), resume(carrega(args[2], de)))
        return 0
    r = resume(carrega(args[0], de))
    if js:
        print(json.dumps(r, indent=1))
    else:
        imprime(args[0], r)
    return 0


if __name__ == "__main__":
    sys.exit(main())
