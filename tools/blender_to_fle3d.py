# -*- coding: utf-8 -*-
"""
Конвертер модели -> FleXSeeR (.fle3d) средствами Blender.

Blender умеет открыть практически всё (FBX, OBJ, GLB/GLTF, VRM, Blend),
поэтому этот путь подходит, если исходный файл не .glb или если нужно
подготовить модель вручную (упростить, удалить лишнее, поправить позу).

Запуск (Blender 3.6 / 4.x):
    blender --background --python blender_to_fle3d.py -- model.glb out.fle3d
или из самого Blender: Scripting -> Text Editor -> открыть файл -> Run Script
(тогда параметры задаются в переменной ARGS ниже).

Что делает скрипт:
  1) импортирует модель;
  2) находит меш (и арматуру, если она есть);
  3) прогоняет анимацию и запекает положения вершин в кадры;
  4) нормализует модель по габаритам (с учётом всего хода анимации);
  5) пишет .fle3d.

Анимация запекается, поэтому прошивке не нужен скелет: только готовые
кадры. На матрице 48x16 этого достаточно, а кода в прошивке заметно меньше.
"""

import os
import sys

import bpy

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fle3d

# Параметры запуска из Blender (если запускаем не из командной строки)
ARGS = ["model.glb", "out.fle3d"]


def script_args():
    if "--" in sys.argv:
        return sys.argv[sys.argv.index("--") + 1:]
    return ARGS


def import_model(path):
    ext = os.path.splitext(path)[1].lower()
    if ext in (".glb", ".gltf"):
        bpy.ops.import_scene.gltf(filepath=path)
    elif ext == ".fbx":
        bpy.ops.import_scene.fbx(filepath=path)
    elif ext == ".obj":
        bpy.ops.wm.obj_import(filepath=path)
    elif ext in (".usd", ".usda", ".usdc", ".usdz"):
        bpy.ops.wm.usd_import(filepath=path)     # USD-импортёр встроен в Blender
    elif ext == ".blend":
        bpy.ops.wm.open_mainfile(filepath=path)
    else:
        raise SystemExit("unsupported source: %s" % ext)


def find_mesh_object():
    """Объект с мешем (и, если есть, арматурой)."""
    best = None
    for obj in bpy.context.scene.objects:
        if obj.type != "MESH":
            continue
        if best is None or len(obj.data.vertices) > len(best.data.vertices):
            best = obj
    if best is None:
        raise SystemExit("no mesh in scene")
    return best


def object_color(obj):
    """Цвет объекта: вершинный цвет, иначе цвет материала."""
    me = obj.data
    if len(me.vertex_colors) > 0:
        layer = me.vertex_colors.active
        c = layer.data[0].color
        return (int(c[0] * 255), int(c[1] * 255), int(c[2] * 255), 255)
    if len(me.materials) > 0 and me.materials[0] is not None:
        c = me.materials[0].diffuse_color
        return (int(c[0] * 255), int(c[1] * 255), int(c[2] * 255), 255)
    return (220, 220, 220, 255)


def colors_from_texture(obj):
    """
    Запекает текстуру материала в цвета вершин: в матрице 48x16 текстура
    не помещается, а одноцветная модель выглядит бедно. Если у модели есть
    текстура — берём цвет пикселя в UV-координате каждой вершины.
    Возвращает список цветов или None, если текстуры нет.
    """
    me = obj.data
    uv_layer = me.uv_layers.active if len(me.uv_layers) else None
    if uv_layer is None:
        return None
    if not len(me.materials) or me.materials[0] is None:
        return None

    mat = me.materials[0]
    if not mat.use_nodes:
        return None

    image = None
    for node in mat.node_tree.nodes:
        if node.type == "TEX_IMAGE" and node.image is not None:
            image = node.image
            break
    if image is None:
        return None

    try:
        import numpy as np
    except ImportError:
        return None

    w, h = image.size
    if w == 0 or h == 0:
        return None

    buf = np.empty(w * h * 4, dtype=np.float32)
    image.pixels.foreach_get(buf)
    buf = buf.reshape(h, w, 4)

    def sample(u, v):
        x = int(u * w) % w
        y = int(v * h) % h
        px = buf[y, x]
        rgb = [px[0], px[1], px[2]]
        if px[3] < 0.5:
            return None
        # Blender отдаёт линейные значения — возвращаем в sRGB
        out = []
        for c in rgb:
            c = max(0.0, min(1.0, c))
            s = 12.92 * c if c <= 0.0031308 else 1.055 * (c ** (1 / 2.4)) - 0.055
            out.append(int(max(0.0, min(1.0, s)) * 255))
        return (out[0], out[1], out[2], 255)

    cols = [None] * len(me.vertices)
    fallback = object_color(obj)
    for poly in me.polygons:
        for vi in poly.vertices:
            if cols[vi] is not None:
                continue
            uv = uv_layer.data[vi].uv
            cols[vi] = sample(uv[0], uv[1]) or fallback
    return cols


def triangles_of(obj):
    tris = []
    for poly in obj.data.polygons:
        v = list(poly.vertices)
        for i in range(1, len(v) - 1):
            tris.append((v[0], v[i], v[i + 1]))
    return tris


def evaluated_positions(obj, frame):
    """Положения вершин на кадре (с учётом арматуры)."""
    bpy.context.scene.frame_set(frame)
    dg = bpy.context.evaluated_depsgraph_get()
    obj_eval = obj.evaluated_get(dg)
    me = obj_eval.to_mesh()
    try:
        return [tuple(v.co) for v in me.vertices]
    finally:
        obj_eval.to_mesh_clear()


def main():
    args = script_args()
    if len(args) < 2:
        raise SystemExit("usage: blender --background --python blender_to_fle3d.py -- src dst")
    src, dst = args[0], args[1]
    fps = int(args[2]) if len(args) > 2 else 24
    max_frames = int(args[3]) if len(args) > 3 else 0

    import_model(src)
    obj = find_mesh_object()

    scene = bpy.context.scene
    start = scene.frame_start
    end = scene.frame_end
    if max_frames > 0:
        end = min(end, start + max_frames - 1)

    mesh = fle3d.Mesh()
    mesh.triangles = triangles_of(obj)

    # Цвета: сначала пробуем запечь текстуру, иначе — цвет материала
    tex_cols = colors_from_texture(obj)
    if tex_cols:
        mesh.colors = tex_cols
        print("  colors    : baked from texture")
    else:
        mesh.colors = [object_color(obj)] * len(obj.data.vertices)

    frames = []
    if end > start:
        for f in range(start, end + 1):
            frames.append(evaluated_positions(obj, f))
        mesh.positions = frames[0]
    else:
        mesh.positions = evaluated_positions(obj, start)

    # нормализация по габаритам с учётом всей анимации
    allpos = list(mesh.positions) + [p for fr in frames for p in fr]
    tmp = fle3d.Mesh()
    tmp.positions = allpos
    k = fle3d.normalize_mesh(tmp)
    mesh.positions = [(p[0] * k, p[1] * k, p[2] * k) for p in mesh.positions]
    frames = [[(p[0] * k, p[1] * k, p[2] * k) for p in fr] for fr in frames]

    size = fle3d.write_fle3d(dst, mesh, frames=frames, fps=fps, loop=True)

    print("done: %s" % dst)
    print("  vertices  : %d" % mesh.vertex_count)
    print("  triangles : %d" % mesh.tri_count)
    print("  frames    : %d @ %d fps" % (len(frames), fps))
    print("  size      : %d bytes" % size)


if __name__ == "__main__":
    main()
