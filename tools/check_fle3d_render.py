# -*- coding: utf-8 -*-
"""
Отрисовка модели .fle3d на матрице 48x16 — проверка формата «глазами»
прошивки до того, как она прочитает файл с SD-карты.

Использует ту же геометрию, что и firmware/src/Scene3D.cpp:
корпус R = 80 мм, H = 160 мм, луч из точки наблюдателя через поверхность
цилиндра, затенение по нормали грани.

Запуск:
    python check_fle3d_render.py model.fle3d [--frame 0] [--eye-h 100] [--dist 600]
"""

import argparse
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fle3d

R = 80.0
H = 160.0
W = 48
HT = 16
COL_DIR = 1


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


def render(model, frame, eye, scale_mm, show_colors=True):
    """scale_mm — половина габарита модели в мм (масштаб объекта в лампе)."""
    if model["frameCount"] > 1:
        pos = model["frames"][min(frame, model["frameCount"] - 1)]
    else:
        pos = [(v["pos"][0], v["pos"][1], v["pos"][2]) for v in model["vertices"]]

    # модель в мм: единицы файла -> мм
    k = scale_mm / 512.0
    pos = [(p[0] * k, p[1] * k, p[2] * k) for p in pos]

    tris = []
    for ti, t in enumerate(model["triangles"]):
        tris.append((pos[t[0]], pos[t[1]], pos[t[2]], ti))

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
            nrm = vnorm(vcross(vsub(b, a), vsub(c, a)))
            lam = max(0.0, -vdot(nrm, d))
            shade = 0.16 + 0.84 * lam
            ch = shadeColor(model, idx, u, v, shade)
            grid[HT - 1 - y][x] = ch[0]
            colors[HT - 1 - y][x] = ch[1]

    return grid, colors


def shadeColor(model, tri_idx, u, v, shade):
    """Символ + цвет (интерполяция цвета по грани)."""
    t = model["triangles"][tri_idx]
    w = 1.0 - u - v
    cols = []
    for idx in t:
        c = model["vertices"][idx]["col"]
        cols.append((c[0], c[1], c[2]))
    r = int(cols[0][0] * w + cols[1][0] * u + cols[2][0] * v)
    g = int(cols[0][1] * w + cols[1][1] * u + cols[2][1] * v)
    b = int(cols[0][2] * w + cols[1][2] * u + cols[2][2] * v)
    r = int(r * shade)
    g = int(g * shade)
    b = int(b * shade)
    level = " .:-=+*#%@"[max(0, min(9, int(shade * 9.99)))]
    return level, (r, g, b)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("model")
    ap.add_argument("--frame", type=int, default=0)
    ap.add_argument("--eye-h", type=float, default=100.0)
    ap.add_argument("--dist", type=float, default=600.0)
    ap.add_argument("--size", type=float, default=44.0, help="полуразмер модели, мм")
    args = ap.parse_args()

    model = fle3d.read_fle3d(args.model)
    eye = (0.0, args.eye_h, args.dist)

    grid, colors = render(model, args.frame, eye, args.size)
    print("=== %s, кадр %d ===" % (os.path.basename(args.model), args.frame))
    for row in grid:
        print("|" + "".join(row) + "|")
    print()

    # фрагмент в цвете (ASCII-раскраска по оттенку)
    print("цвет (средний по яркости):")
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


if __name__ == "__main__":
    main()
