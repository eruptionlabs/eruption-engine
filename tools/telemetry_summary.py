#!/usr/bin/env python3
"""Resumo do registro de eventos do jogo (telemetry.emit, JSONL).

    tools/telemetry_summary.py partida.jsonl [partida.jsonl.1 ...]
    tools/telemetry_summary.py partida.jsonl --channel dano --json

Por canal: quantos eventos e quantos por minuto de jogo. Por campo:
numero -> n, soma, media, min, p50, p95, max; booleano -> quantos true;
texto -> os valores mais comuns; vetor -> media de cada componente.
Sem dependencias alem do Python 3. Linha quebrada (arquivo cortado por
um processo morto) e' contada e pulada, nunca derruba a leitura.
"""
import argparse
import json
import math
import sys
from collections import Counter, defaultdict


def percentile(sorted_values, q):
    if not sorted_values:
        return None
    k = (len(sorted_values) - 1) * q
    lo, hi = math.floor(k), math.ceil(k)
    return sorted_values[lo] + (sorted_values[hi] - sorted_values[lo]) * (k - lo)


class Field:
    def __init__(self):
        self.numbers = []
        self.trues = 0
        self.bools = 0
        self.texts = Counter()
        self.vectors = []

    def add(self, v):
        if isinstance(v, bool):
            self.bools += 1
            self.trues += v
        elif isinstance(v, (int, float)):
            self.numbers.append(float(v))
        elif isinstance(v, str):
            self.texts[v] += 1
        elif isinstance(v, list) and all(isinstance(x, (int, float)) for x in v):
            self.vectors.append(v)

    def summary(self):
        out = {}
        if self.numbers:
            s = sorted(self.numbers)
            total = sum(s)
            out.update(n=len(s), sum=total, mean=total / len(s), min=s[0],
                       p50=percentile(s, 0.5), p95=percentile(s, 0.95), max=s[-1])
        if self.bools:
            out.update(n_bool=self.bools, true=self.trues)
        if self.texts:
            out.update(n_text=sum(self.texts.values()), top=self.texts.most_common(5))
        if self.vectors:
            dim = min(len(v) for v in self.vectors)
            out.update(n_vec=len(self.vectors),
                       mean_vec=[sum(v[i] for v in self.vectors) / len(self.vectors) for i in range(dim)])
        return out


def read(paths, only):
    heads, ends = [], []
    channels = defaultdict(lambda: {"count": 0, "fields": defaultdict(Field)})
    bad = 0
    t_max = 0
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    ev = json.loads(line)
                except ValueError:
                    bad += 1
                    continue
                if not isinstance(ev, dict):
                    bad += 1
                    continue
                ch = ev.get("ch", "?")
                if ch == "_head":
                    heads.append(ev)
                    continue
                if ch == "_end":
                    ends.append(ev)
                    continue
                if isinstance(ev.get("t"), (int, float)):
                    t_max = max(t_max, ev["t"])
                if only and ch not in only:
                    continue
                c = channels[ch]
                c["count"] += 1
                for k, v in ev.items():
                    if k not in ("t", "f", "ch"):
                        c["fields"][k].add(v)
    return heads, ends, channels, bad, t_max


def fmt(v):
    if v is None:
        return "-"
    if isinstance(v, float) and not v.is_integer():
        return f"{v:.4g}"
    return str(int(v)) if isinstance(v, float) else str(v)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+")
    ap.add_argument("--channel", action="append", help="so' este canal (repetivel)")
    ap.add_argument("--json", action="store_true", help="saida em JSON")
    args = ap.parse_args()

    heads, ends, channels, bad, t_max = read(args.files, set(args.channel or []))
    minutes = t_max / 60000.0
    result = {
        "header": heads[0] if heads else None,
        "game_minutes": minutes,
        "dropped": sum(e.get("dropped", 0) for e in ends),
        "bad_lines": bad,
        "channels": {
            ch: {"count": c["count"], "per_min": c["count"] / minutes if minutes > 0 else None,
                 "fields": {k: f.summary() for k, f in sorted(c["fields"].items())}}
            for ch, c in sorted(channels.items())
        },
    }
    if args.json:
        json.dump(result, sys.stdout, ensure_ascii=False, indent=1)
        print()
        return
    h = result["header"] or {}
    print(f"{h.get('project', '?')} / {h.get('map', '?')}  {h.get('date', '')}  build {h.get('build', '?')}")
    print(f"{minutes:.2f} min de jogo; descartados {result['dropped']}; linhas ruins {bad}")
    for ch, c in result["channels"].items():
        rate = f"{c['per_min']:.1f}/min" if c["per_min"] is not None else ""
        print(f"\n[{ch}] {c['count']} eventos {rate}")
        for k, s in c["fields"].items():
            parts = []
            if "n" in s:
                parts.append("n {} soma {} media {} min {} p50 {} p95 {} max {}".format(
                    s["n"], fmt(s["sum"]), fmt(s["mean"]), fmt(s["min"]), fmt(s["p50"]), fmt(s["p95"]), fmt(s["max"])))
            if "n_bool" in s:
                parts.append(f"true {s['true']}/{s['n_bool']}")
            if "top" in s:
                parts.append("comuns " + ", ".join(f"{v} x{n}" for v, n in s["top"]))
            if "mean_vec" in s:
                parts.append("media " + " ".join(fmt(x) for x in s["mean_vec"]))
            print(f"  {k:<16} " + " | ".join(parts))


if __name__ == "__main__":
    main()
