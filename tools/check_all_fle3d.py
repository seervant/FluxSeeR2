# -*- coding: utf-8 -*-
"""
Проверка всех готовых .fle3d в папке: читает файл обратно, рендерит
кадр на матрице 48x16 и считает, сколько пикселей зажглось.

Пустая или почти пустая картинка — признак проблемы с моделью.
"""

import glob
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fle3d
import check_fle3d_render as R

folder = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "models", "fle3d")

files = sorted(glob.glob(os.path.join(folder, "*.fle3d")))
print("checking %d files in %s" % (len(files), folder))
print("")
print("%-42s %6s %6s %6s %6s  %s" % ("file", "verts", "tris", "frames", "pixels", "note"))
print("-" * 92)

bad = []
for path in files:
    name = os.path.basename(path)
    try:
        m = fle3d.read_fle3d(path)
        if m["vertCount"] == 0 or m["triCount"] == 0:
            bad.append((name, "empty mesh"))
            print("%-42s %6d %6d %6d %6s  EMPTY MESH" % (
                name, m["vertCount"], m["triCount"], m["frameCount"], "-"))
            continue

        # кадр из середины цикла
        frame = m["frameCount"] // 2
        grid, colors = R.render(m, frame, (0.0, 100.0, -600.0), 100.0)
        lit = sum(1 for row in grid for ch in row if ch != " ")

        note = ""
        if m["frameCount"] > 1 and len(m["frames"]) != m["frameCount"]:
            note = "BAD FRAMES"
            bad.append((name, "bad frames"))
        if lit < 3:
            note = "almost empty"
            bad.append((name, "almost empty"))
        if m["vertCount"] > 600:
            note = (note + " ").strip() + "too many verts"
        if m["triCount"] > 800:
            note = (note + " ").strip() + "too many tris"

        print("%-42s %6d %6d %6d %6d  %s" % (
            name, m["vertCount"], m["triCount"], m["frameCount"], lit, note))
    except Exception as e:
        bad.append((name, str(e)))
        print("%-42s  ERROR: %s" % (name, e))

print("")
if bad:
    print("problems: %d" % len(bad))
    for n, why in bad:
        print("  %s - %s" % (n, why))
else:
    print("all files OK")
