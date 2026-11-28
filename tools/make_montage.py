# -*- coding: utf-8 -*-
"""
Монтаж рендеров моделей .fle3d в один PNG — «как это будет выглядеть
на матрице 48x16».

PIL в окружении нет, поэтому PNG пишется вручную (zlib из стандартной
библиотеки). Номера в углу каждой панели — шрифт 3x5, без файлов шрифтов.

Запуск:
    python make_montage.py [папка] [--out models/montage.png]
"""

import argparse
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fle3d
import check_fle3d_render as R

# Шрифт 3x5: каждая цифра - 5 строк по 3 пикселя
DIGITS = {
    "0": ["111", "101", "101", "101", "111"],
    "1": ["010", "110", "010", "010", "111"],
    "2": ["111", "001", "111", "100", "111"],
    "3": ["111", "001", "111", "001", "111"],
    "4": ["101", "101", "111", "001", "001"],
    "5": ["111", "100", "111", "001", "111"],
    "6": ["111", "100", "111", "101", "111"],
    "7": ["111", "001", "001", "001", "001"],
    "8": ["111", "101", "111", "101", "111"],
    "9": ["111", "101", "111", "001", "111"],
}

# панели монтажа: (имя файла, подпись)
PANELS = [
    ("mario", "mario"),
    ("yoshi__super_mario_64_rigged_animated", "yoshi"),
    ("halloween_pumpkin_low_poly", "pumpkin"),
    ("cartoon_bird", "bird"),
    ("low_poly_rocket_ship", "rocket"),
    ("truck", "truck"),
    ("minecraft_steve_low_poly", "steve"),
    ("minecraft_chicken", "chicken"),
    ("colossal_aquid", "aquid"),
    ("orca_whale_low_poly", "orca"),
    ("heart_-_animated_anatomical_3d_model", "heart"),
    ("tennis_ball_low-poly_pbr", "tennis"),
    ("cluckoraptor", "raptor"),
    ("minecraft_globe", "globe"),
    ("low-poly_apple", "apple"),
    ("theia_prime_f_-_the_black_hole", "theia"),
]

SCALE = 4          # во сколько раз увеличиваем пиксель матрицы
GAP = 4            # зазор между пан��лями
HEADER = 18        # полоса сверху под номер
COLS = 4


def write_png(path, w, h, rows):
    """rows — список строк, каждая строка — список (r, g, b)."""
    raw = bytearray()
    for row in rows:
        raw.append(0)
        for px in row:
            raw += bytes(px)

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff))

    hdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", hdr) +
           chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


def draw_number(pixels, x0, y0, text, color):
    """Рисует число шрифтом 3x5."""
    for ci, ch in enumerate(text):
        glyph = DIGITS.get(ch)
        if not glyph:
            continue
        for ry, line in enumerate(glyph):
            for rx, bit in enumerate(line):
                if bit == "1":
                    px = x0 + (ci * 4 + rx), y0 + ry
                    pixels[px[1]][px[0]] = color


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("folder", nargs="?", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "models", "fle3d"))
    ap.add_argument("--out", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "models", "montage.png"))
    ap.add_argument("--size", type=float, default=100.0)
    args = ap.parse_args()

    cell_w = 48 * SCALE
    cell_h = 16 * SCALE
    panel_w = cell_w
    panel_h = cell_h + HEADER

    rows_needed = (len(PANELS) + COLS - 1) // COLS
    img_w = COLS * panel_w + (COLS + 1) * GAP
    img_h = rows_needed * panel_h + (rows_needed + 1) * GAP

    pixels = [[(18, 18, 22) for _ in range(img_w)] for _ in range(img_h)]

    used = []
    for i, (fname, label) in enumerate(PANELS):
        path = os.path.join(args.folder, fname + ".fle3d")
        if not os.path.exists(path):
            print("skip (not found): %s" % fname)
            continue

        m = fle3d.read_fle3d(path)
        frame = m["frameCount"] // 2
        grid, colors = R.render(m, frame, (0.0, 100.0, -600.0), args.size)

        px = (i % COLS) * (panel_w + GAP) + GAP
        py = (i // COLS) * (panel_h + GAP) + GAP

        # номер панели
        draw_number(pixels, px + 3, py + 4, "%d" % (i + 1), (255, 210, 90))

        # сама матрица
        for y in range(16):
            for x in range(48):
                r, g, b = colors[y][x]
                if r + g + b < 10:
                    continue
                for dy in range(SCALE):
                    for dx in range(SCALE):
                        pixels[py + HEADER + y * SCALE + dy][px + x * SCALE + dx] = (r, g, b)

        used.append((i + 1, label, fname))

    write_png(args.out, img_w, img_h, pixels)
    print("written: %s (%d x %d)" % (os.path.abspath(args.out), img_w, img_h))
    print("")
    print("panels:")
    for num, label, fname in used:
        print("  %2d  %-16s %s" % (num, label, fname))


if __name__ == "__main__":
    main()
