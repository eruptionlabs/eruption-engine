#!/usr/bin/env python3
"""Validate splash/crown placement against rain-cloud footprints.

Inputs: a screenshot (with debug splash viz: magenta crowns + red 3D quads)
and /tmp/splash_dump.txt (ERUPTION_TEST_DUMP_SPLASH_CLOUDS=1) containing the inverse
view-proj, the character anchor and the follower footprint boxes (world XZ).

Reports, for the classified splash-mark pixels:
  - % inside the union of projected footprint quads  (want ~100%)
  - list of clusters of marks OUTSIDE any footprint  (violations)
"""
import sys
from PIL import Image

def parse_dump(path):
    toks = open(path).read().split()
    ivp = [float(t) for t in toks[1:17]]
    i = toks.index("INFO")
    info = [float(t) for t in toks[i+1:i+6]]
    clouds = []
    for j, t in enumerate(toks):
        if t == "CLOUD":
            clouds.append([float(v) for v in toks[j+1:j+6]])
    return ivp, info, clouds

def invert4(m):
    # m: 16 floats column-major (glm mat4[col][row]). Gauss-Jordan on rows.
    A = [[m[c*4+r] for c in range(4)] for r in range(4)]  # A[row][col]
    I = [[float(r == c) for c in range(4)] for r in range(4)]
    for col in range(4):
        piv = max(range(col, 4), key=lambda r: abs(A[r][col]))
        A[col], A[piv] = A[piv], A[col]
        I[col], I[piv] = I[piv], I[col]
        d = A[col][col]
        A[col] = [v/d for v in A[col]]
        I[col] = [v/d for v in I[col]]
        for r in range(4):
            if r != col and A[r][col] != 0.0:
                f = A[r][col]
                A[r] = [a - f*b for a, b in zip(A[r], A[col])]
                I[r] = [a - f*b for a, b in zip(I[r], I[col])]
    return [I[r][c] for c in range(4) for r in range(4)]  # back to column-major

def project(vp, p, w, h):
    x, y, z = p
    cx = vp[0]*x + vp[4]*y + vp[8]*z + vp[12]
    cy = vp[1]*x + vp[5]*y + vp[9]*z + vp[13]
    cw = vp[3]*x + vp[7]*y + vp[11]*z + vp[15]
    if cw <= 1e-6:
        return None
    return ((cx/cw*0.5 + 0.5)*w, (cy/cw*0.5 + 0.5)*h)

def point_in_quad(px, py, quad):
    # quad: 4 (x,y) screen points; sign-of-cross test (convex)
    sign = 0
    for i in range(4):
        x1, y1 = quad[i]
        x2, y2 = quad[(i+1) % 4]
        cross = (x2-x1)*(py-y1) - (y2-y1)*(px-x1)
        if abs(cross) < 1e-9:
            continue
        s = 1 if cross > 0 else -1
        if sign == 0:
            sign = s
        elif s != sign:
            return False
    return True

def main(img_path, dump_path):
    ivp, info, clouds = parse_dump(dump_path)
    vp = invert4(ivp)
    im = Image.open(img_path).convert("RGB")
    w, h = im.size
    px = im.load()
    ground_y = info[2]
    quads = []
    for c in clouds:
        cx, cz, hx, hz = c[0], c[1], c[2], c[3]
        corners = [(cx-hx, ground_y, cz-hz), (cx+hx, ground_y, cz-hz),
                   (cx+hx, ground_y, cz+hz), (cx-hx, ground_y, cz+hz)]
        q = [project(vp, p, w, h) for p in corners]
        if all(q):
            quads.append(q)
    marks_in = marks_out = 0
    out_pts = []
    for y in range(0, h, 2):
        for x in range(0, w, 2):
            r, g, b = px[x, y]
            is_mag = r > 140 and r > g+60 and b > g+40
            is_red = r > 200 and g < 60 and b < 60
            if not (is_mag or is_red):
                continue
            # expand the test slightly (roofs shift the projection)
            inside = any(point_in_quad(x, y, q) for q in quads)
            if inside:
                marks_in += 1
            else:
                marks_out += 1
                out_pts.append((x, y))
    total = marks_in + marks_out
    print("clouds=%d quads=%d  marks=%d  inside=%d (%.1f%%)  outside=%d" %
          (len(clouds), len(quads), total, marks_in,
           100.0*marks_in/max(total, 1), marks_out))
    if out_pts:
        # cluster outside marks into a coarse 8x4 grid for location
        grid = {}
        for x, y in out_pts:
            key = (x*8//w, y*4//h)
            grid[key] = grid.get(key, 0) + 1
        top = sorted(grid.items(), key=lambda kv: -kv[1])[:6]
        for (gx, gy), n in top:
            print("  outside cluster cell(%d,%d) x~%d y~%d n=%d" %
                  (gx, gy, (gx+0.5)*w/8, (gy+0.5)*h/4, n))

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else "/tmp/splash_dump.txt")
