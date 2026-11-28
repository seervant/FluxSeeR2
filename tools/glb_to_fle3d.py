# -*- coding: utf-8 -*-
"""
Конвертер glTF/GLB -> FleXSeeR (.fle3d). Без внешних зависимостей.

Работает с бинарными .glb и с .gltf + внешним .bin. Анимация запекается
в кадры (скиннинг считается на стороне конвертера), поэтому прошивке
не нужны ни скелет, ни матрицы — только готовые положения вершин.

Запуск:
    python glb_to_fle3d.py model.glb out.fle3d [--fps 24] [--frames 72]
                                  [--name "Имя модели"] [--no-scale]

Ограничения (сознательные, под размер матрицы 48x16):
    - один меш, одна анимация;
    - без текстур (цвет берётся из COLOR_0 или из материала);
    - нормали приводятся к граням (плоское затенение) — на матрице
      768 пикселей гладкие нормали не читаются.

Проверено на тестовой модели tools/make_test_glb.py
"""

import argparse
import json
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fle3d

# --- матрицы 4x4 в порядке glTF (столбцы) ---

def mat_identity():
    return [1.0, 0, 0, 0,  0, 1.0, 0, 0,  0, 0, 1.0, 0,  0, 0, 0, 1.0]


def mat_mul(a, b):
    """a * b (оба — списки из 16 чисел, столбцы)."""
    r = [0.0] * 16
    for c in range(4):
        for rw in range(4):
            s = 0.0
            for k in range(4):
                s += a[k * 4 + rw] * b[c * 4 + k]
            r[c * 4 + rw] = s
    return r


def mat_trs(t, q, s):
    """Матрица из перемещения, поворота (кватернион xyzw) и масштаба."""
    x, y, z, w = q
    xx, yy, zz = x * x, y * y, z * z
    xy, xz, yz = x * y, x * z, y * z
    wx, wy, wz = w * x, w * y, w * z
    m = [
        (1 - 2 * (yy + zz)) * s[0], (2 * (xy + wz)) * s[0], (2 * (xz - wy)) * s[0], 0,
        (2 * (xy - wz)) * s[1], (1 - 2 * (xx + zz)) * s[1], (2 * (yz + wx)) * s[1], 0,
        (2 * (xz + wy)) * s[2], (2 * (yz - wx)) * s[2], (1 - 2 * (xx + yy)) * s[2], 0,
        t[0], t[1], t[2], 1,
    ]
    return m


def mat_transform_point(m, p):
    return (m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12],
            m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13],
            m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14])


def mat_transform_dir(m, p):
    return (m[0] * p[0] + m[4] * p[1] + m[8] * p[2],
            m[1] * p[0] + m[5] * p[1] + m[9] * p[2],
            m[2] * p[0] + m[6] * p[1] + m[10] * p[2])


def quat_slerp(a, b, t):
    """Кватернионы glTF — в порядке (x, y, z, w)."""
    w = fle3d._qslerp((a[3], a[0], a[1], a[2]), (b[3], b[0], b[1], b[2]), t)
    return (w[1], w[2], w[3], w[0])


# ---------------------------------------------------------------------------
# Чтение GLB
# ---------------------------------------------------------------------------
def load_gltf(path):
    """Возвращает (json, binary_chunk)."""
    with open(path, "rb") as f:
        data = f.read()

    if data[0:4] == b"glTF":
        length = struct.unpack_from("<I", data, 8)[0]
        offset = 12
        js = None
        binary = b""
        while offset < length:
            clen, ctype = struct.unpack_from("<II", data, offset)
            offset += 8
            chunk = data[offset:offset + clen]
            offset += clen
            if ctype == 0x4E4F534A:      # JSON
                js = json.loads(chunk.decode("utf-8"))
            elif ctype == 0x004E4942:    # BIN
                binary = chunk
        if js is None:
            raise ValueError("в файле нет JSON-чанка")
        return js, binary

    # .gltf + внешний .bin
    with open(path, "r", encoding="utf-8") as f:
        js = json.load(f)
    binary = b""
    for buf in js.get("buffers", []):
        uri = buf.get("uri", "")
        if not uri:
            continue
        base = os.path.dirname(os.path.abspath(path))
        with open(os.path.join(base, uri), "rb") as f:
            binary = f.read()
        break
    return js, binary


CTYPES = {5120: "b", 5121: "B", 5122: "h", 5123: "H", 5125: "I", 5126: "f"}
NCOMP = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT2": 4,
         "MAT3": 9, "MAT4": 16}


def read_accessor(js, binary, index):
    """Читает accessor в виде списка кортежей."""
    acc = js["accessors"][index]
    view = js["bufferViews"][acc["bufferView"]]
    base = view.get("byteOffset", 0) + acc.get("byteOffset", 0)

    fmt = CTYPES[acc["componentType"]]
    ncomp = NCOMP[acc["type"]]
    elem = struct.calcsize("<" + fmt)
    stride = view.get("byteStride") or (ncomp * elem)

    out = []
    for i in range(acc["count"]):
        off = base + i * stride
        if ncomp == 1:
            out.append(struct.unpack_from("<" + fmt, binary, off)[0])
        else:
            out.append(struct.unpack_from("<" + fmt * ncomp, binary, off))
    return out


def node_local_matrix(js, node):
    if "matrix" in node:
        return list(node["matrix"])
    t = node.get("translation", [0.0, 0.0, 0.0])
    r = node.get("rotation", [0.0, 0.0, 0.0, 1.0])
    s = node.get("scale", [1.0, 1.0, 1.0])
    return mat_trs(t, r, s)


def build_node_tree(js):
    """Список узлов с родителями и глобальными матрицами."""
    nodes = js.get("nodes", [])
    parent = {}
    for i, n in enumerate(nodes):
        for ch in n.get("children", []):
            parent[ch] = i

    globals_ = [None] * len(nodes)

    def global_of(i):
        if globals_[i] is not None:
            return globals_[i]
        m = node_local_matrix(js, nodes[i])
        p = parent.get(i)
        if p is not None:
            m = mat_mul(global_of(p), m)
        globals_[i] = m
        return m

    for i in range(len(nodes)):
        global_of(i)
    return parent, globals_


# ---------------------------------------------------------------------------
# Выборка анимации
# ---------------------------------------------------------------------------
def sample_sampler(js, binary, samplers, sampler_index, time, kind):
    """
    Значение дорожки в момент времени. kind = 'VEC3' или 'QUAT'.
    Возвращает None, если времени не покрывают.
    """
    smp = samplers[sampler_index]
    times = read_accessor(js, binary, smp["input"])
    values = read_accessor(js, binary, smp["output"])
    interp = smp.get("interpolation", "LINEAR")

    if not times:
        return None
    if time <= times[0]:
        return values[0]
    if time >= times[-1]:
        return values[-1]

    i = 0
    while i + 1 < len(times) and times[i + 1] < time:
        i += 1
    t0, t1 = times[i], times[i + 1]
    span = (t1 - t0) or 1.0
    f = (time - t0) / span

    v0, v1 = values[i], values[i + 1]
    if interp == "STEP":
        return v0
    if kind == "QUAT":
        return quat_slerp(v0, v1, f)
    if kind == "VEC3":
        return tuple(v0[k] + (v1[k] - v0[k]) * f for k in range(3))
    return v0


def animated_node_trs(js, binary, anim_index, node_index, time):
    """Локальные TRS узла в момент времени (или None, если узел не анимирован)."""
    anim = js["animations"][anim_index]
    samplers = anim.get("samplers", [])
    for ch in anim.get("channels", []):
        if ch["target"].get("node") != node_index:
            continue
        path = ch["target"]["path"]
        val = sample_sampler(js, binary, samplers, ch["sampler"], time,
                             "QUAT" if path == "rotation" else "VEC3")
        if val is None:
            return None
        return path, val
    return None


def evaluate_skin(js, binary, skin_index, node_index, time):
    """
    Возвращает матрицы скиннинга для всех костей скина в момент времени:
    список [(joint_node_index, matrix_16)].
    """
    skin = js["skins"][skin_index]
    joints = skin["joints"]
    parent, globals_ = build_node_tree(js)

    ibm = None
    if "inverseBindMatrices" in skin:
        raw = read_accessor(js, binary, skin["inverseBindMatrices"])
        ibm = [list(r) for r in raw]

    # локальные трансформации узлов в этот момент
    nodes = js.get("nodes", [])
    local = {}
    for n in nodes:
        local[id(n)] = node_local_matrix(js, n)
    # применяем анимацию
    for i, n in enumerate(nodes):
        got = animated_node_trs(js, binary, 0, i, time)
        if got:
            path, val = got
            t = n.get("translation", [0.0, 0.0, 0.0])
            r = n.get("rotation", [0.0, 0.0, 0.0, 1.0])
            s = n.get("scale", [1.0, 1.0, 1.0])
            if path == "translation":
                t = val
            elif path == "rotation":
                r = val
            else:
                s = val
            local[id(n)] = mat_trs(t, r, s)

    # глобальные матрицы с учётом анимации
    gcache = {}

    def global_of(i):
        if i in gcache:
            return gcache[i]
        m = local[id(nodes[i])]
        p = parent.get(i)
        if p is not None:
            m = mat_mul(global_of(p), m)
        gcache[i] = m
        return m

    out = []
    for k, j in enumerate(joints):
        m = global_of(j)
        if ibm and k < len(ibm):
            m = mat_mul(m, ibm[k])
        out.append((j, m))
    return out


# ---------------------------------------------------------------------------
# Сборка модели
# ---------------------------------------------------------------------------
def build_mesh(js, binary, node_index, time, skin_index=None):
    """
    Собирает меш узла в позициях на момент времени.
    Возвращает (positions, normals, colors, uvs, triangles).
    """
    node = js["nodes"][node_index]
    prim = js["meshes"][node["mesh"]]["primitives"][0]

    pos_acc = prim["attributes"]["POSITION"]
    positions = read_accessor(js, binary, pos_acc)

    normals = []
    if "NORMAL" in prim["attributes"]:
        normals = read_accessor(js, binary, prim["attributes"]["NORMAL"])

    colors = []
    if "COLOR_0" in prim["attributes"]:
        raw = read_accessor(js, binary, prim["attributes"]["COLOR_0"])
        for c in raw:
            if len(c) == 3:
                colors.append((c[0], c[1], c[2], 255))
            else:
                colors.append(tuple(c))
    else:
        # цвет из материала
        col = (200, 200, 200, 255)
        mat = js.get("materials", [])
        if "material" in prim and prim["material"] < len(mat):
            bc = mat[prim["material"]].get("pbrMetallicRoughness", {}).get("baseColorFactor")
            if bc:
                col = tuple(int(max(0.0, min(1.0, x)) * 255) for x in bc)
        colors = [col] * len(positions)

    uvs = []
    if "TEXCOORD_0" in prim["attributes"]:
        uvs = read_accessor(js, binary, prim["attributes"]["TEXCOORD_0"])

    indices = []
    if "indices" in prim:
        indices = read_accessor(js, binary, prim["indices"])
    else:
        indices = list(range(len(positions)))
    triangles = [(indices[i], indices[i + 1], indices[i + 2])
                 for i in range(0, len(indices) - 2, 3)]

    # --- скиннинг ---
    if skin_index is not None and "JOINTS_0" in prim["attributes"]:
        joints = read_accessor(js, binary, prim["attributes"]["JOINTS_0"])
        weights = read_accessor(js, binary, prim["attributes"]["WEIGHTS_0"])
        skin_mats = evaluate_skin(js, binary, skin_index, node_index, time)

        new_pos = []
        for i, p in enumerate(positions):
            acc = [0.0, 0.0, 0.0]
            js_ = joints[i]
            ws = weights[i]
            for k in range(4):
                w = ws[k] if k < len(ws) else 0.0
                if w <= 0.0:
                    continue
                jn = js_[k] if k < len(js_) else 0
                # индекс в JOINTS_0 — это позиция в списке joints скина
                if jn >= len(skin_mats):
                    continue
                m = skin_mats[jn][1]
                tp = mat_transform_point(m, p)
                acc[0] += tp[0] * w
                acc[1] += tp[1] * w
                acc[2] += tp[2] * w
            new_pos.append(tuple(acc))
        positions = new_pos
        normals = []      # пересчитаем ниже

    return positions, normals, colors, uvs, triangles


def recalc_normals(positions, triangles):
    """Плоские нормали (по граням) — на матрице 48x16 они читаются лучше."""
    nrm = [[0.0, 0.0, 0.0] for _ in positions]
    for t in triangles:
        a, b, c = positions[t[0]], positions[t[1]], positions[t[2]]
        u = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
        v = (c[0] - a[0], c[1] - a[1], c[2] - a[2])
        n = (u[1] * v[2] - u[2] * v[1],
             u[2] * v[0] - u[0] * v[2],
             u[0] * v[1] - u[1] * v[0])
        for idx in t:
            nrm[idx][0] += n[0]
            nrm[idx][1] += n[1]
            nrm[idx][2] += n[2]
    out = []
    for n in nrm:
        l = math.sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2])
        out.append((n[0] / l, n[1] / l, n[2] / l) if l > 1e-9 else (0.0, 0.0, 1.0))
    return out


def find_skinned_node(js):
    """Ищет узел с мешем, к которому привязан скин."""
    for i, n in enumerate(js.get("nodes", [])):
        if "mesh" in n and "skin" in n:
            return i
    for i, n in enumerate(js.get("nodes", [])):
        if "mesh" in n:
            return i
    return None


def main():
    ap = argparse.ArgumentParser(description="GLB -> FLE3D")
    ap.add_argument("source")
    ap.add_argument("target")
    ap.add_argument("--fps", type=int, default=24, help="кадров в секунду")
    ap.add_argument("--frames", type=int, default=0, help="длительность цикла в кадрах (0 — вся анимация)")
    ap.add_argument("--loop", action="store_true", default=True)
    ap.add_argument("--no-scale", action="store_true", help="не нормализовать по габаритам")
    args = ap.parse_args()

    js, binary = load_gltf(args.source)

    node_index = find_skinned_node(js)
    if node_index is None:
        raise SystemExit("в модели нет узла с мешем")

    skin_index = js["nodes"][node_index].get("skin")
    if skin_index is None:
        for s in js.get("skins", []):
            skin_index = js.get("skins").index(s)
            break

    # --- длительность анимации ---
    duration = 0.0
    if js.get("animations"):
        anim = js["animations"][0]
        samplers = anim.get("samplers", [])
        for ch in anim["channels"]:
            smp = samplers[ch["sampler"]]
            times = read_accessor(js, binary, smp["input"])
            if times:
                duration = max(duration, times[-1])
    frame_count = args.frames if args.frames > 0 else max(1, int(round(duration * args.fps)))

    # --- базовый кадр (поза покоя) ---
    pos0, nrm0, col0, uv0, tri0 = build_mesh(js, binary, node_index, 0.0, skin_index)

    mesh = fle3d.Mesh()
    mesh.positions = pos0
    mesh.triangles = tri0
    mesh.colors = col0
    mesh.uvs = uv0
    mesh.normals = recalc_normals(pos0, tri0)

    # --- кадры анимации ---
    frames = []
    if js.get("animations") and frame_count > 1:
        for f in range(frame_count):
            t = f / float(args.fps)
            pf, _, _, _, _ = build_mesh(js, binary, node_index, t, skin_index)
            frames.append(pf)

    # --- нормализация по габаритам (по всем кадрам) ---
    if not args.no_scale:
        allpos = list(mesh.positions) + [p for fr in frames for p in fr]
        tmp = fle3d.Mesh()
        tmp.positions = allpos
        k = fle3d.normalize_mesh(tmp)
        mesh.positions = [(p[0] * k, p[1] * k, p[2] * k) for p in mesh.positions]
        frames = [[(p[0] * k, p[1] * k, p[2] * k) for p in fr] for fr in frames]

    size = fle3d.write_fle3d(args.target, mesh, frames=frames, fps=args.fps,
                             loop=args.loop)

    print("done: %s" % args.target)
    print("  vertices  : %d" % mesh.vertex_count)
    print("  triangles : %d" % mesh.tri_count)
    print("  frames    : %d @ %d fps" % (frame_count, args.fps))
    print("  size      : %d bytes" % size)


if __name__ == "__main__":
    main()
