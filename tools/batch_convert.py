# -*- coding: utf-8 -*-
"""
Пакетная конвертация моделей в .fle3d.

Перебирает файлы в папке (по умолчанию models/), для каждого подбирает
уровень упрощения так, чтобы модель укладывалась в бюджет лампы
(<= 600 вершин, <= 800 треугольников), и пишет .fle3d рядом.

Запуск:
    python batch_convert.py [папка] [--out папка] [--anim клип]
                           [--fps 24] [--no-anim]

Сообщения — на английском, чтобы не ломалась кодировка консоли Windows.
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fle3d
import glb_to_fle3d as G

# бюджет лампы
MAX_VERTS = 600
MAX_TRIS = 800

# уровни упрощения: чем больше число, тем мельче ячейка и меньше полигонов
GRIDS = [0, 40, 32, 26, 22, 20, 18, 16, 14, 12, 10, 8, 6]

# длина имени модели на OLED (firmware/include/Model3D.h)
MODEL_NAME_LEN = 12

SOURCE_EXT = (".glb", ".gltf")


def convert_one(src, dst, fps, anim, no_anim):
    """Конвертирует один файл. Возвращает словарь с результатом."""
    t0 = time.time()
    try:
        scene = G.build_scene(src, fps=fps, anim_spec=anim,
                               no_scale=False)
    except Exception as e:
        return {"ok": False, "error": str(e), "name": os.path.basename(src)}

    if no_anim:
        scene["frames"] = []

    best = None
    for grid in GRIDS:
        try:
            mesh, frames = G.simplify_scene(scene, grid, max_tris=MAX_TRIS)
        except Exception as e:
            return {"ok": False, "error": str(e), "name": os.path.basename(src)}
        fits = (mesh.vertex_count <= MAX_VERTS and mesh.tri_count <= MAX_TRIS)
        best = (grid, mesh, frames)
        if fits:
            break
    if best is None:
        return {"ok": False, "error": "no mesh", "name": os.path.basename(src)}

    grid, mesh, frames = best
    try:
        size = fle3d.write_fle3d(dst, mesh, frames=frames,
                                 fps=scene["out_fps"], loop=True)
    except Exception as e:
        return {"ok": False, "error": str(e), "name": os.path.basename(src)}

    # пропорции габаритов: сильно вытянутая модель на матрице выглядит
    # тонкой полоской — стоит предупредить
    xs = [p[0] for p in mesh.positions]
    ys = [p[1] for p in mesh.positions]
    zs = [p[2] for p in mesh.positions]
    ext = [max(xs) - min(xs), max(ys) - min(ys), max(zs) - min(zs)]
    longest = max(ext) if ext else 0.0
    ratio = (longest / min(e for e in ext if e > 0.0)) if longest > 0 else 0.0

    return {
        "ok": True,
        "name": os.path.basename(src),
        "out": os.path.basename(dst),
        "grid": grid,
        "verts": mesh.vertex_count,
        "tris": mesh.tri_count,
        "frames": len(frames) if frames else 1,
        "fps": scene["out_fps"],
        "size": size,
        "clip": scene["anim_name"],
        "clip_count": scene["anim_count"],
        "ratio": ratio,
        "secs": time.time() - t0,
    }


def main():
    ap = argparse.ArgumentParser(description="batch GLB -> FLE3D")
    ap.add_argument("folder", nargs="?", default="models")
    ap.add_argument("--out", default="", help="папка для .fle3d (по умолчанию — рядом с моделью)")
    ap.add_argument("--anim", default="", help="клип анимации: номер или часть имени")
    ap.add_argument("--fps", type=int, default=24)
    ap.add_argument("--no-anim", action="store_true", help="без анимации, только поза")
    ap.add_argument("--only", default="", help="конвертировать только файлы с подстрокой в имени")
    args = ap.parse_args()

    folder = os.path.abspath(args.folder)
    out_dir = os.path.abspath(args.out) if args.out else folder
    if not os.path.isdir(out_dir):
        os.makedirs(out_dir)

    sources = []
    for name in sorted(os.listdir(folder)):
        low = name.lower()
        if not low.endswith(SOURCE_EXT):
            continue
        if args.only and args.only.lower() not in low:
            continue
        sources.append(os.path.join(folder, name))

    if not sources:
        print("no models found in %s" % folder)
        return

    print("source folder: %s" % folder)
    print("output folder: %s" % out_dir)
    print("budget: <= %d verts, <= %d tris" % (MAX_VERTS, MAX_TRIS))
    print("")

    rows = []
    failed = []
    for src in sources:
        base = os.path.splitext(os.path.basename(src))[0]
        dst = os.path.join(out_dir, base + ".fle3d")
        res = convert_one(src, dst, args.fps, args.anim, args.no_anim)
        if res["ok"]:
            rows.append(res)
            warn = ""
            if len(base) > MODEL_NAME_LEN - 1:
                warn = "  (name too long for OLED, will be cut to %d chars)" % (
                    MODEL_NAME_LEN - 1)
            if res["ratio"] > 3.0:
                warn += "  (very stretched 1:%.1f - will look thin on the matrix)" % (
                    res["ratio"])
            print("OK   %-42s grid=%-3s verts=%-4d tris=%-5d frames=%-3d %6.1f KB  %.1fs%s"
                  % (res["name"], res["grid"] or "-", res["verts"], res["tris"],
                     res["frames"], res["size"] / 1024.0, res["secs"], warn))
        else:
            failed.append(res)
            print("FAIL %-42s %s" % (res["name"], res["error"]))

    print("")
    print("converted: %d, failed: %d" % (len(rows), len(failed)))
    if rows:
        total = sum(r["size"] for r in rows)
        print("total size: %.1f KB" % (total / 1024.0))


if __name__ == "__main__":
    main()
