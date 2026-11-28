# -*- coding: utf-8 -*-
"""
Генератор тестовой модели .glb — куб со скелетом и анимацией.

Нужен только для проверки конвейера конвертации:
    python make_test_glb.py test.glb
    python glb_to_fle3d.py test.glb test.fle3d
    python fle3d.py test.fle3d

Модель: куб 2x2x2, привязанный к одной кости, которая медленно
поворачивается вокруг вертикальной оси. Именно такой «скелет с анимацией»
приезжает из типовых анимированных моделей.
"""

import json
import math
import struct
import sys


def main(path):
    # --- вершины куба ---
    h = 1.0
    v = [
        (-h, -h, -h), ( h, -h, -h), ( h,  h, -h), (-h,  h, -h),
        (-h, -h,  h), ( h, -h,  h), ( h,  h,  h), (-h,  h,  h),
    ]
    faces = [
        (0, 1, 2), (0, 2, 3),      # зад
        (5, 4, 7), (5, 7, 6),      # перед
        (4, 0, 3), (4, 3, 7),      # лево
        (1, 5, 6), (1, 6, 2),      # право
        (3, 2, 6), (3, 6, 7),      # верх
        (4, 5, 1), (4, 1, 0),      # низ
    ]
    triangles = []
    for f in faces:
        triangles.extend(f)

    joints = [(0, 0, 0, 0)] * 8
    weights = [(1.0, 0.0, 0.0, 0.0)] * 8

    # --- сборка буфера ---
    buf = bytearray()

    def add(data, align=4):
        while len(buf) % align:
            buf.append(0)
        off = len(buf)
        buf.extend(data)
        return off

    views = []

    # позиции
    off = add(b"".join(struct.pack("<3f", *p) for p in v))
    views.append((off, len(v) * 12, 34962))
    pos_view = len(views) - 1

    # индексы
    off = add(b"".join(struct.pack("<H", i) for i in triangles))
    views.append((off, len(triangles) * 2, 34963))
    idx_view = len(views) - 1

    # кости
    off = add(b"".join(struct.pack("<4H", *j) for j in joints))
    views.append((off, len(joints) * 8, 34962))
    joint_view = len(views) - 1

    # веса
    off = add(b"".join(struct.pack("<4f", *w) for w in weights))
    views.append((off, len(weights) * 16, 34962))
    weight_view = len(views) - 1

    # обратные матрицы привязки (единичная)
    off = add(b"".join(struct.pack("<16f", 1, 0, 0, 0, 0, 1, 0, 0,
                                   0, 0, 1, 0, 0, 0, 0, 1) for _ in range(1)))
    views.append((off, 64, 34962))
    ibm_view = len(views) - 1

    # анимация: поворот кости вокруг Y на 90° за 2 секунды
    times = [0.0, 1.0, 2.0]
    quats = []
    for t in times:
        a = t * (math.pi / 4.0)          # 90° за 2 с
        quats.append((0.0, math.sin(a / 2), 0.0, math.cos(a / 2)))
    off = add(b"".join(struct.pack("<f", t) for t in times))
    views.append((off, len(times) * 4, 34962))
    time_view = len(views) - 1

    off = add(b"".join(struct.pack("<4f", *q) for q in quats))
    views.append((off, len(quats) * 16, 34962))
    quat_view = len(views) - 1

    js = {
        "asset": {"version": "2.0", "generator": "make_test_glb"},
        "scene": 0,
        "scenes": [{"nodes": [0, 1]}],
        "nodes": [
            {"name": "Cube", "mesh": 0, "skin": 0},
            {"name": "Armature", "children": [2]},
            {"name": "Joint0", "rotation": [0.0, 0.0, 0.0, 1.0]},
        ],
        "meshes": [{
            "name": "CubeMesh",
            "primitives": [{
                "attributes": {
                    "POSITION": 0,
                    "JOINTS_0": 2,
                    "WEIGHTS_0": 3,
                },
                "indices": 1,
                "material": 0,
                "mode": 4,
            }],
        }],
        "materials": [{
            "name": "Body",
            "pbrMetallicRoughness": {"baseColorFactor": [0.25, 0.65, 1.0, 1.0]},
        }],
        "skins": [{
            "name": "Rig",
            "joints": [2],
            "inverseBindMatrices": 4,
        }],
        "animations": [{
            "name": "Spin",
            "channels": [{"sampler": 0, "target": {"node": 2, "path": "rotation"}}],
            "samplers": [{"input": 5, "output": 6, "interpolation": "LINEAR"}],
        }],
        "accessors": [
            {"bufferView": pos_view, "componentType": 5126, "count": len(v),
             "type": "VEC3", "min": [-h, -h, -h], "max": [h, h, h]},
            {"bufferView": idx_view, "componentType": 5123, "count": len(triangles),
             "type": "SCALAR"},
            {"bufferView": joint_view, "componentType": 5123, "count": len(joints),
             "type": "VEC4"},
            {"bufferView": weight_view, "componentType": 5126, "count": len(weights),
             "type": "VEC4"},
            {"bufferView": ibm_view, "componentType": 5126, "count": 1,
             "type": "MAT4"},
            {"bufferView": time_view, "componentType": 5126, "count": len(times),
             "type": "SCALAR", "min": [min(times)], "max": [max(times)]},
            {"bufferView": quat_view, "componentType": 5126, "count": len(quats),
             "type": "VEC4"},
        ],
        "bufferViews": [
            {"buffer": 0, "byteOffset": o, "byteLength": l, "target": t}
            for (o, l, t) in views
        ],
        "buffers": [{"byteLength": len(buf)}],
    }

    json_bytes = json.dumps(js, separators=(",", ":")).encode("utf-8")
    while len(json_bytes) % 4:
        json_bytes += b" "
    bin_bytes = bytes(buf)
    while len(bin_bytes) % 4:
        bin_bytes += b"\x00"

    total = 12 + 8 + len(json_bytes) + 8 + len(bin_bytes)
    with open(path, "wb") as f:
        f.write(struct.pack("<4sII", b"glTF", 2, total))
        f.write(struct.pack("<II", len(json_bytes), 0x4E4F534A))
        f.write(json_bytes)
        f.write(struct.pack("<II", len(bin_bytes), 0x004E4942))
        f.write(bin_bytes)

    print("done: %s (%d bytes)" % (path, total))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "test.glb")
