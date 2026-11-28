# -*- coding: utf-8 -*-
"""
Отрисовка модели .fle3d на матрице 48x16 — проверка формата «глазами»
прошивки до того, как она прочитает файл с SD-карты.

Использует ту же геометрию и те же формулы, что и firmware/src/Scene3D.cpp:
корпус R = 80 мм, H = 160 мм, луч из точки наблюдателя через поверхность
цилиндра, масштаб модели по радиусу из заголовка .fle3d, затенение по
нормали грани, подсветка рёбер.

Запуск:
    python check_fle3d_render.py model.fle3d [--frame 0] [--size 55]
                                       [--eye-h 100] [--dist 600] [--edge 8]
"""

import argparse
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fle3d

# геометрия корпуса — как в firmware/include/Config.h
R = 80.0
H = 160.0
W = 48
HT = 16
COL_DIR = 1

# столбец с «прицелом» — как в firmware/include/Config.h.
# Наблюдатель стоит напротив этого столбца, поэтому шов ленты
# (между 47 и 0) оказывается за спиной, а объект в центре сцены
# не разрезается пополам.
CROSSHAIR_COL = 24

# значения по умолчанию — как в Config.h
DEF_SIZE_PCT = 55.0
DEF_EYE_H_MM = 100.0
DEF_DIST_MM = 600.0
DEF_EDGE_MM = 8.0

EDGE_FADE = 1.5


def vsub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def vdot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def vcross(a, b):
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def vnorm(a):
    l = math.sqrt(vdot(a, a))
    return (0.0, 0.0, 0.0) if l < 1e-9 else (a[0] / l, a[1] / l, a[2] / l)


def ray_tri(o, d, a, b, c):
    """Пересечение луча с треугольником (Мёллер–Трумборн)."""
    e1 = vsub(b, a)
    e2 = vsub(c, a)
    p = vcross(d, e2)
    det = vdot(e1, p)
    if abs(det) < 1e-9:
        return None
    inv = 1.0 / det
    tv = vsub(o, a)
    u = vdot(tv, p) * inv
    if u < 0.0 or u > 1.0:
        return None
    q = vcross(tv, e1)
    v = vdot(d, q) * inv
    if v < 0.0 or u + v > 1.0:
        return None
    t = vdot(e2, q) * inv
    return (t, u, v)


def render(model, frame, eye, size_pct, edge_mm=DEF_EDGE_MM):
    """
    Отрисовка модели на матрице. Формулы повторяют renderModel()
    из firmware/src/Scene3D.cpp.

    Возвращает (grid, colors) — символы и цвета пикселей.
    """
    if model["frameCount"] > 1:
        pos = model["frames"][min(frame, model["frameCount"] - 1)]
    else:
        pos = [(v["pos"][0], v["pos"][1], v["pos"][2]) for v in model["vertices"]]

    # Масштаб: радиус модели приводится к заданной доле радиуса корпуса.
    # Радиус модели — полудиагональ габаритов (из заголовка .fle3d).
    radius_units = model["modelRadius"]
    if radius_units <= 0.0:
        return ([[" "] * W for _ in range(HT)],
                [[(0, 0, 0)] * W for _ in range(HT)])
    radius_mm = R * max(5.0, min(100.0, size_pct)) / 100.0
    scale = radius_mm / radius_units

    pos_mm = [(p[0] * scale, p[1] * scale, p[2] * scale) for p in pos]
    tris = []
    for ti, t in enumerate(model["triangles"]):
        tris.append((pos_mm[t[0]], pos_mm[t[1]], pos_mm[t[2]], ti))

    row_h = H / HT
    grid = [[" "] * W for _ in range(HT)]
    colors = [[(0, 0, 0)] * W for _ in range(HT)]

    for x in range(W):
        ang = COL_DIR * x * (2 * math.pi / W)
        sn, cs = math.sin(ang), math.cos(ang)
        n_body = (sn, 0.0, cs)
        for y in range(HT):
            hh = (y + 0.5) * row_h - H * 0.5
            p_body = (sn * R, hh, cs * R)
            to_eye = vsub(eye, p_body)
            dist = math.sqrt(vdot(to_eye, to_eye))
            if dist < 1e-3:
                continue
            if vdot(n_body, to_eye) <= 0:
                continue
            d = (-to_eye[0] / dist, -to_eye[1] / dist, -to_eye[2] / dist)

            best = None
            for (a, b, c, idx) in tris:
                hit = ray_tri(eye, d, a, b, c)
                if hit and (best is None or hit[0] < best[0]):
                    best = (hit[0], hit[1], hit[2], a, b, c, idx)
            if best is None:
                continue

            t, u, v, a, b, c, idx = best
            e1 = vsub(b, a)
            e2 = vsub(c, a)
            cr = vcross(e1, e2)
            length = math.sqrt(vdot(cr, cr))
            if length < 1e-9:
                continue
            nrm = (cr[0] / length, cr[1] / length, cr[2] / length)

            lambert = -vdot(nrm, d)
            if lambert < 0.0:
                lambert = 0.0
            shade = 0.16 + 0.84 * lambert

            # воздушная перспектива — как в прошивке
            depth = max(0.0, min(1.0, (t - 380.0) / 420.0))
            shade *= (1.0 - 0.20 * depth)

            # расстояние до ребра треугольника
            wgt = 1.0 - u - v
            min_bary = min(u, v, wgt)
            if min_bary < 0.0:
                min_bary = 0.0
            area = 0.5 * length
            max_edge = max(math.sqrt(vdot(e1, e1)),
                            max(math.sqrt(vdot(vsub(c, b), vsub(c, b))),
                                math.sqrt(vdot(vsub(a, c), vsub(a, c)))))
            alt = (2.0 * area / max_edge) if max_edge > 1e-6 else 0.0
            dist_edge = min_bary * alt
            edge_k = max(0.0, min(1.0, (edge_mm + EDGE_FADE - dist_edge) / EDGE_FADE))

            ch = shadeColor(model, idx, u, v, shade, edge_k)
            grid[HT - 1 - y][x] = ch[0]
            colors[HT - 1 - y][x] = ch[1]

    return grid, colors


def shadeColor(model, tri_idx, u, v, shade, edge_k=0.0):
    """Символ + цвет (интерполяция цвета по грани, подсветка ребра)."""
    t = model["triangles"][tri_idx]
    w = 1.0 - u - v
    cols = []
    for idx in t:
        c = model["vertices"][idx]["col"]
        cols.append((c[0], c[1], c[2]))
    r = int(cols[0][0] * w + cols[1][0] * u + cols[2][0] * v)
    g = int(cols[0][1] * w + cols[1][1] * u + cols[2][1] * v)
    b = int(cols[0][2] * w + cols[1][2] * u + cols[2][2] * v)

    # ребро: осветление к белому
    fr = int(r * shade)
    fg = int(g * shade)
    fb = int(b * shade)
    er = min(255, int(fr * 0.45 + 255 * 0.55) + 30)
    eg = min(255, int(fg * 0.45 + 255 * 0.55) + 30)
    eb = min(255, int(fb * 0.45 + 255 * 0.55) + 30)
    r = int(fr + (er - fr) * edge_k)
    g = int(fg + (eg - fg) * edge_k)
    b = int(fb + (eb - fb) * edge_k)

    level = " .:-=+*#%@"[max(0, min(9, int(shade * 9.99)))]
    return level, (r, g, b)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("model")
    ap.add_argument("--frame", type=int, default=0)
    ap.add_argument("--eye-h", type=float, default=DEF_EYE_H_MM)
    ap.add_argument("--dist", type=float, default=DEF_DIST_MM)
    ap.add_argument("--size", type=float, default=DEF_SIZE_PCT,
                    help="размер модели, %% от радиуса корпуса (5..100)")
    ap.add_argument("--edge", type=float, default=DEF_EDGE_MM,
                    help="ширина подсветки рёбер, мм")
    args = ap.parse_args()

    model = fle3d.read_fle3d(args.model)
    # Наблюдатель — напротив столбца с прицелом (как eyeInCalFrame
    # в firmware/src/EffectScene3D.cpp)
    ang = COL_DIR * CROSSHAIR_COL * (2 * math.pi / W)
    eye = (math.sin(ang) * args.dist, args.eye_h, math.cos(ang) * args.dist)

    grid, colors = render(model, args.frame, eye, args.size, args.edge)
    print("=== %s, frame %d, size %d%% ===" % (os.path.basename(args.model),
                                               args.frame, args.size))
    for row in grid:
        print("|" + "".join(row) + "|")
    print()

    # фрагмент в цвете (ASCII-раскраска по оттенку)
    print("color (hue):")
    for row in colors:
        line = ""
        for (r, g, b) in row:
            if r + g + b < 12:
                line += " "
            elif r > g and r > b:
                line += "R"
            elif g > r and g > b:
                line += "G"
            else:
                line += "B"
        print("|" + line + "|")

    print()
    print("model radius: %.1f units, %d verts, %d tris, %d frames"
          % (model["modelRadius"], model["vertCount"], model["triCount"],
             model["frameCount"]))


if __name__ == "__main__":
    main()
