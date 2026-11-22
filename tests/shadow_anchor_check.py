"""Confere o registro ERUPTION_TEST_SHADOW_ANCHOR_LOG: a parte fracionaria da
posicao (em texels) de pontos fixos do mundo nas matrizes que a luz usa nao
pode mudar quando a camera anda, e o texel nao pode mudar. Uso:
shadow_anchor_check.py <log> <atlas> [tolerancia_texels]. Sai 1 se falhar."""
import re,sys
log,atlas=sys.argv[1],int(sys.argv[2])
tol=float(sys.argv[3]) if len(sys.argv)>3 else 0.02
rows=[];pat=re.compile(r'\[SHADOWANCHOR\] c(\d) p(\d) u=(\S+) v=(\S+) texel=([0-9.eE+-]+)')
for l in open(log):
    m=pat.search(l)
    if m: rows.append((int(m[1]),int(m[2]),float(m[3]),float(m[4]),float(m[5])))
# per key (cascade, probe): sequence over frames, skip first 40 quads of frames
from collections import defaultdict
bad=False
d=defaultdict(list)
for c,k,u,v,t in rows: d[(c,k)].append((u*atlas,v*atlas,t))
def fd(a,b):
    x=(a-b)%1.0; return min(x,1-x)
for key,seq in sorted(d.items()):
    seq=seq[40:]
    u0,v0,_=seq[0]
    fr=max(max(fd(u,u0),fd(v,v0)) for u,v,_ in seq)
    ts=sorted(set(round(t,4) for _,_,t in seq))
    ok = fr < tol and len(ts) == 1
    bad |= not ok
    print(f"c{key[0]} p{key[1]} n={len(seq)} frac_max={fr:.4f} texels={ts[:6]} {'ok' if ok else 'FALHOU'}")
if not d: print("sem linhas SHADOWANCHOR (binario sem o registro?)"); bad = True
sys.exit(1 if bad else 0)
