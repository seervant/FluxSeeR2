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
    - все меши модели собираются вместе (у персонажа их несколько);
    - одна анимация (первая в файле);
    - без текстур (цвет берётся из COLOR_0 или из материала);
    - нормали приводятся к граням (плоское затенение) — на матрице
      768 пикселей гладкие нормали не читаются.

Проверено на тестовой модели tools/make_test_glb.py
"""

import argparse
import base64
import json
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fle3d

# Предел числа кадров анимации в файле. Длинные анимации семплируются:
# прошивка интерполирует между кадрами, поэтому 48-72 кадров достаточно.
DEFAULT_MAX_FRAMES = 72

# Сколько вершин оставить до скиннинга. Модели тяжелее этого
# прореживаются пространственно: считать скиннинг для сотен тысяч
# вершин в каждом кадре слишком долго и памяти не хватит.
THIN_LIMIT = 8000

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
    """
    Возвращает (json, buffers), где buffers — список байтовых буферов.
    Поддерживаются .glb, .gltf с внешними .bin и .gltf с данными внутри
    (data: URI, base64).
    """
    with open(path, "rb") as f:
        data = f.read()

    if data[0:4] == b"glTF":
        length = struct.unpack_from("<I", data, 8)[0]
        offset = 12
        js = None
        buffers = []
        while offset < length:
            clen, ctype = struct.unpack_from("<II", data, offset)
            offset += 8
            chunk = data[offset:offset + clen]
            offset += clen
            if ctype == 0x4E4F534A:      # JSON
                js = json.loads(chunk.decode("utf-8"))
            elif ctype == 0x004E4942:    # BIN
                buffers.append(chunk)
        if js is None:
            raise ValueError("в файле нет JSON-чанка")
        return js, buffers

    # .gltf: буферы могут быть внешними файлами или быть встроены в URI
    with open(path, "r", encoding="utf-8") as f:
        js = json.load(f)

    base = os.path.dirname(os.path.abspath(path))
    buffers = []
    for buf in js.get("buffers", []):
        uri = buf.get("uri", "")
        if not uri:
            buffers.append(b"")          # GLB-вставка внутри .gltf — не поддержана
            continue
        if uri.startswith("data:"):
            head, _, payload = uri.partition(",")
            if ";base64" in head:
                buffers.append(base64.b64decode(payload))
            else:
                buffers.append(payload.encode("utf-8"))
        else:
            with open(os.path.join(base, uri), "rb") as f:
                buffers.append(f.read())
    return js, buffers


CTYPES = {5120: "b", 5121: "B", 5122: "h", 5123: "H", 5125: "I", 5126: "f"}
NCOMP = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT2": 4,
         "MAT3": 9, "MAT4": 16}


def read_accessor(js, binary, index, cache=None):
    """
    Читает accessor в виде списка кортежей. binary — список буферов.
    cache — словарь для повторного использования: данные accessor'а
    не меняются от кадра к кадру, а перечитывать их 72 раза дорого.
    """
    if cache is not None and index in cache:
        return cache[index]

    acc = js["accessors"][index]
    view = js["bufferViews"][acc["bufferView"]]
    bi = view.get("buffer", 0)
    buf = binary[bi] if bi < len(binary) else b""
    base = view.get("byteOffset", 0) + acc.get("byteOffset", 0)

    fmt = CTYPES[acc["componentType"]]
    ncomp = NCOMP[acc["type"]]
    elem = struct.calcsize("<" + fmt)
    stride = view.get("byteStride") or (ncomp * elem)

    out = []
    for i in range(acc["count"]):
        off = base + i * stride
        if ncomp == 1:
            out.append(struct.unpack_from("<" + fmt, buf, off)[0])
        else:
            out.append(struct.unpack_from("<" + fmt * ncomp, buf, off))
    if cache is not None:
        cache[index] = out
    return out


def node_local_matrix(js, node):
    if "matrix" in node:
        return list(node["matrix"])
    t = node.get("translation", [0.0, 0.0, 0.0])
    r = node.get("rotation", [0.0, 0.0, 0.0, 1.0])
    s = node.get("scale", [1.0, 1.0, 1.0])
    return mat_trs(t, r, s)


# ---------------------------------------------------------------------------
# Выборка анимации
# ---------------------------------------------------------------------------
def build_anim_tracks(js, anim_index):
    """
    Дорожки анимации по узлам: {node_index: [(path, sampler_index), ...]}.
    У узла может быть несколько дорожек (поворот + сдвиг + масштаб) —
    применяются все вместе, иначе поза «едет».

    Каналы морф-таргетов (path = "weights") пропускаются: формат .fle3d
    не хранит морфы, а их значения — скаляры, и в общий код они не ложатся.
    """
    anim = js["animations"][anim_index]
    tracks = {}
    for ch in anim.get("channels", []):
        node = ch["target"].get("node")
        path = ch["target"].get("path")
        if node is None or path not in ("translation", "rotation", "scale"):
            continue
        tracks.setdefault(node, []).append((path, ch["sampler"]))
    return tracks


def sample_sampler(js, binary, samplers, sampler_index, time, kind, cache=None):
    """
    Значение дорожки в момент времени. kind = 'VEC3' или 'QUAT'.
    Возвращает None, если времени не покрывают.
    """
    smp = samplers[sampler_index]
    times = read_accessor(js, binary, smp["input"], cache)
    values = read_accessor(js, binary, smp["output"], cache)
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


def node_parents(js):
    """Индекс родителя каждого узла (или None)."""
    parent = {}
    for i, n in enumerate(js.get("nodes", [])):
        for ch in n.get("children", []):
            parent[ch] = i
    return parent


def compute_local_matrices(js, binary, tracks, samplers, time, cache=None):
    """
    Локальные матрицы всех узлов в момент времени (с учётом анимации).
    """
    nodes = js.get("nodes", [])
    local = []
    for i, n in enumerate(nodes):
        if "matrix" in n:
            local.append(list(n["matrix"]))
            continue
        t = n.get("translation", [0.0, 0.0, 0.0])
        r = n.get("rotation", [0.0, 0.0, 0.0, 1.0])
        s = n.get("scale", [1.0, 1.0, 1.0])
        for path, smp_index in tracks.get(i, []):
            val = sample_sampler(js, binary, samplers, smp_index, time,
                                 "QUAT" if path == "rotation" else "VEC3", cache)
            if val is None:
                continue
            if path == "translation":
                t = val
            elif path == "rotation":
                r = val
            elif path == "scale":
                s = val
        local.append(mat_trs(t, r, s))
    return local


def global_matrices(js, parent, local):
    """Глобальные матрицы узлов (с учётом родителей)."""
    gcache = {}

    def global_of(i):
        if i in gcache:
            return gcache[i]
        m = local[i]
        p = parent.get(i)
        if p is not None:
            m = mat_mul(global_of(p), m)
        gcache[i] = m
        return m

    return [global_of(i) for i in range(len(local))]


def evaluate_skin(js, binary, skin_index, parent, local, cache=None):
    """
    Матрицы скиннинга для всех костей скина в момент времени:
    список [(joint_node_index, matrix_16)].
    """
    skin = js["skins"][skin_index]
    joints = skin["joints"]
    g = global_matrices(js, parent, local)

    ibm = None
    if "inverseBindMatrices" in skin:
        raw = read_accessor(js, binary, skin["inverseBindMatrices"], cache)
        ibm = [list(r) for r in raw]

    out = []
    for k, j in enumerate(joints):
        m = g[j]
        if ibm and k < len(ibm):
            m = mat_mul(m, ibm[k])
        out.append((j, m))
    return out


# ---------------------------------------------------------------------------
# Сборка модели
# ---------------------------------------------------------------------------
def thin_plan(js, binary, node_indices, parent, max_verts=THIN_LIMIT, cache=None):
    """
    Прореживает модель ДО скиннинга.

    Нужно для тяжёлых файлов: у модели с 363 тысячами вершин считать
    скиннинг покадрово — это минуты и гигабайты памяти. Прореживание
    пространственное: вершины, попавшие в одну ячейку общей сетки,
    сливаются в одну представительскую, треугольники переиспользуют её.

    Возвращает (план, всего_вершин_было). План — словарь
    (узел, примитив) -> (список оставленных индексов, таблица remap),
    либо None, если прореживать не нужно.
    """
    # --- собираем сырые позиции всех частей ---
    parts = []
    total = 0
    for ni in node_indices:
        for pi, prim in enumerate(js["meshes"][js["nodes"][ni]["mesh"]]["primitives"]):
            pos = read_accessor(js, binary, prim["attributes"]["POSITION"], cache)
            parts.append((ni, pi, pos))
            total += len(pos)
    if total <= max_verts:
        return None, total

    # --- габариты общей сетки ---
    xs = []
    ys = []
    zs = []
    for (_, _, pos) in parts:
        for p in pos:
            xs.append(p[0]); ys.append(p[1]); zs.append(p[2])
    lo = (min(xs), min(ys), min(zs))
    hi = (max(xs), max(ys), max(zs))
    longest = max(hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2])
    if longest <= 1e-9:
        return None, total

    # --- подбираем разрешение: берём максимально мелкую сетку,
    #     при которой вершин всё ещё не больше лимита ---
    n = 4
    plan = None
    while n <= 2048:
        cell = longest / float(n)
        cur = {}
        kept = 0
        for (ni, pi, pos) in parts:
            local_of_cell = {}
            remap = []
            keep = []
            for i, p in enumerate(pos):
                key = (int((p[0] - lo[0]) / cell),
                       int((p[1] - lo[1]) / cell),
                       int((p[2] - lo[2]) / cell))
                k = local_of_cell.get(key)
                if k is None:
                    k = len(keep)
                    local_of_cell[key] = k
                    keep.append(i)
                remap.append(k)
            cur[(ni, pi)] = (keep, remap)
            kept += len(keep)
        if kept > max_verts:
            break                      # следующая сетка ещё мельче — она лишняя
        plan = cur
        n *= 2

    if plan is None:
        plan = cur
    return plan, total


def build_mesh(js, binary, node_indices, time, tracks, samplers, parent,
               plan=None, cache=None):
    """
    Собирает меш(и) узлов в позициях на момент времени.
    node_indices — список узлов (у персонажа их несколько: части тела).
    plan — результат thin_plan(): какие вершины оставить.
    Возвращает (positions, normals, colors, uvs, triangles).
    """
    positions = []
    normals = []
    colors = []
    uvs = []
    triangles = []

    local = compute_local_matrices(js, binary, tracks, samplers, time, cache)
    g = global_matrices(js, parent, local)

    skin_cache = {}

    def skin_mats_for(idx):
        """Матрицы скина считаются один раз на кадр, а не на каждую часть тела."""
        if idx not in skin_cache:
            skin_cache[idx] = evaluate_skin(js, binary, idx, parent, local, cache)
        return skin_cache[idx]

    for node_index in node_indices:
        node = js["nodes"][node_index]
        node_mat = g[node_index]
        skin_index = node.get("skin")
        if skin_index is None:
            skin_index = js.get("skin")     # внешний скин (fallback конвертера)
        if skin_index is not None and skin_index >= len(js.get("skins", [])):
            skin_index = None

        prims = js["meshes"][node["mesh"]]["primitives"]
        for pi, prim in enumerate(prims):

            pos_acc = prim["attributes"]["POSITION"]
            p_pos = read_accessor(js, binary, pos_acc, cache)

            p_nrm = []
            if "NORMAL" in prim["attributes"]:
                p_nrm = read_accessor(js, binary, prim["attributes"]["NORMAL"], cache)

            p_col = []
            if "COLOR_0" in prim["attributes"]:
                raw = read_accessor(js, binary, prim["attributes"]["COLOR_0"], cache)
                for c in raw:
                    if len(c) == 3:
                        p_col.append((c[0], c[1], c[2], 255))
                    else:
                        p_col.append(tuple(c))
            else:
                # цвет из материала
                col = (200, 200, 200, 255)
                mat = js.get("materials", [])
                if "material" in prim and prim["material"] < len(mat):
                    bc = mat[prim["material"]].get("pbrMetallicRoughness", {}).get("baseColorFactor")
                    if bc:
                        col = tuple(int(max(0.0, min(1.0, x)) * 255) for x in bc)
                p_col = [col] * len(p_pos)

            p_uv = []
            if "TEXCOORD_0" in prim["attributes"]:
                p_uv = read_accessor(js, binary, prim["attributes"]["TEXCOORD_0"], cache)

            idx = []
            if "indices" in prim:
                idx = read_accessor(js, binary, prim["indices"], cache)
            else:
                idx = list(range(len(p_pos)))
            p_tris = [(idx[i], idx[i + 1], idx[i + 2])
                      for i in range(0, len(idx) - 2, 3)]

            joints = []
            weights = []
            if "JOINTS_0" in prim["attributes"]:
                joints = read_accessor(js, binary, prim["attributes"]["JOINTS_0"], cache)
                weights = read_accessor(js, binary, prim["attributes"]["WEIGHTS_0"], cache)

            # --- прореживание: оставляем только нужные вершины ---
            sel = plan.get((node_index, pi)) if plan else None
            if sel is not None:
                keep, remap = sel
                p_pos = [p_pos[i] for i in keep]
                if p_nrm:
                    p_nrm = [p_nrm[i] for i in keep]
                p_col = [p_col[i] for i in keep]
                if p_uv:
                    p_uv = [p_uv[i] for i in keep]
                if joints:
                    joints = [joints[i] for i in keep]
                    weights = [weights[i] for i in keep]
                new_tris = []
                for t in p_tris:
                    a, b, c = remap[t[0]], remap[t[1]], remap[t[2]]
                    if a == b or b == c or a == c:
                        continue        # вырожденный после прореживания
                    new_tris.append((a, b, c))
                p_tris = new_tris

            # --- скиннинг ---
            # По спецификации glTF transform самого узла скина игнорируется:
            # позицию дают матрицы костей.
            if skin_index is not None and joints:
                skin_mats = skin_mats_for(skin_index)

                new_pos = []
                for i, p in enumerate(p_pos):
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
                p_pos = new_pos
                p_nrm = []          # пересчитаем ниже

            else:
                # статичная часть: переносим в мировые координаты через узел
                # (если матрица единичная — результат не меняется)
                p_pos = [mat_transform_point(node_mat, p) for p in p_pos]
                if p_nrm:
                    p_nrm = [mat_transform_dir(node_mat, n) for n in p_nrm]

            offset = len(positions)
            positions.extend(p_pos)
            normals.extend(p_nrm)
            colors.extend(p_col)
            uvs.extend(p_uv)
            for t in p_tris:
                triangles.append((t[0] + offset, t[1] + offset, t[2] + offset))

    return positions, normals, colors, uvs, triangles


def find_model_nodes(js):
    """
    Все узлы с мешами — у персонажа их несколько (части тела на одном скине).
    Возвращает (список индексов узлов, индекс скина по умолчанию).
    """
    nodes = js.get("nodes", [])
    meshes = js.get("meshes", [])
    out = []
    for i, n in enumerate(nodes):
        if "mesh" in n and n["mesh"] < len(meshes):
            out.append(i)
    default_skin = 0 if js.get("skins") else None
    return out, default_skin


def pick_anim(js, spec):
    """
    Выбор клипа анимации: номер или часть имени.
    В некоторых моделях клипов сотни (у Mario — 193), поэтому нужно
    уметь указать нужный.
    """
    anims = js.get("animations", [])
    if not anims:
        return 0
    if not spec:
        return 0
    if spec.lstrip("-").isdigit():
        i = int(spec)
        if 0 <= i < len(anims):
            return i
        raise SystemExit("клипа с номером %d нет (всего %d)" % (i, len(anims)))
    low = spec.lower()
    hits = [i for i, a in enumerate(anims) if low in (a.get("name") or "").lower()]
    if not hits:
        raise SystemExit("клип '%s' не найден" % spec)
    return hits[0]


def build_scene(src, fps=24, frames_arg=0, anim_spec="", no_scale=False,
                max_frames=DEFAULT_MAX_FRAMES):
    """
    Читает модель и готовит сетку с кадрами анимации.

    Возвращает словарь: mesh, frames, frame_count, out_fps, duration,
    anim_index, anim_name, anim_count.
    """
    js, binary = load_gltf(src)
    cache = {}

    node_indices, default_skin = find_model_nodes(js)
    if not node_indices:
        raise SystemExit("в модели нет узла с мешем")

    # скин по умолчанию: если узлы не указывают свой, берём первый
    if default_skin is not None:
        js["skin"] = default_skin

    # --- анимация ---
    tracks = {}
    samplers = []
    duration = 0.0
    anim_index = 0
    anim_name = ""
    if js.get("animations"):
        anim_index = pick_anim(js, anim_spec)
        anim = js["animations"][anim_index]
        anim_name = anim.get("name") or ""
        samplers = anim.get("samplers", [])
        tracks = build_anim_tracks(js, anim_index)
        for ch in anim.get("channels", []):
            smp = samplers[ch["sampler"]]
            times = read_accessor(js, binary, smp["input"], cache)
            if times:
                duration = max(duration, times[-1])

    parent = node_parents(js)

    # --- прореживание тяжёлых моделей (до скиннинга) ---
    plan, raw_verts = thin_plan(js, binary, node_indices, parent, cache=cache)

    # Если длительность цикла задана кадрами — шаг времени фиксирован.
    # Иначе растягиваем анимацию на весь цикл и пересчитываем fps,
    # чтобы прошивка проигрывала ровно исходную длительность.
    if frames_arg > 0:
        frame_count = frames_arg
        out_fps = fps
        time_of = lambda f: f / float(fps)
    else:
        frame_count = max(1, int(round(duration * fps)))
        out_fps = fps
        # Длинная анимация семплируется: кадры берутся равномерно по всей
        # длительности, прошивка дорисовывает промежутки интерполяцией.
        if max_frames > 0 and frame_count > max_frames and duration > 0.0:
            frame_count = max_frames
            out_fps = max(1, int(round(frame_count / duration)))
            time_of = lambda f: (f / float(frame_count)) * duration
        elif duration > 0.0 and frame_count > 1:
            out_fps = max(1, int(round(frame_count / duration)))
            time_of = lambda f: (f / float(frame_count)) * duration
        else:
            time_of = lambda f: 0.0

    # --- базовый кадр (поза покоя) ---
    pos0, nrm0, col0, uv0, tri0 = build_mesh(js, binary, node_indices, 0.0,
                                              tracks, samplers, parent,
                                              plan=plan, cache=cache)

    mesh = fle3d.Mesh()
    mesh.positions = pos0
    mesh.triangles = tri0
    mesh.colors = col0
    mesh.uvs = uv0
    mesh.normals = fle3d.recalc_normals(pos0, tri0)

    # --- кадры анимации ---
    frames = []
    if js.get("animations") and frame_count > 1:
        for f in range(frame_count):
            pf, _, _, _, _ = build_mesh(js, binary, node_indices, time_of(f),
                                        tracks, samplers, parent,
                                        plan=plan, cache=cache)
            frames.append(pf)

    # --- центрирование (модель должна стоять в начале координат сцены) ---
    fle3d.center_model(mesh, frames)

    # --- нормализация по габаритам (по всем кадрам) ---
    if not no_scale:
        allpos = list(mesh.positions) + [p for fr in frames for p in fr]
        tmp = fle3d.Mesh()
        tmp.positions = allpos
        k = fle3d.normalize_mesh(tmp)
        mesh.positions = [(p[0] * k, p[1] * k, p[2] * k) for p in mesh.positions]
        frames = [[(p[0] * k, p[1] * k, p[2] * k) for p in fr] for fr in frames]

    return {
        "mesh": mesh,
        "frames": frames,
        "frame_count": frame_count,
        "out_fps": out_fps,
        "duration": duration,
        "anim_index": anim_index,
        "anim_name": anim_name,
        "anim_count": len(js.get("animations", [])),
        "raw_verts": raw_verts,
        "thinned": plan is not None,
    }


def simplify_scene(scene, grid, max_tris=0):
    """
    Возвращает (mesh, frames) с нужным уровнем упрощения.
    Исходная сцена не меняется — можно перебрать несколько уровней.
    max_tris — жёсткий предел числа треугольников (0 — без предела).
    """
    mesh = scene["mesh"]
    frames = scene["frames"]
    if grid > 1:
        mesh, frames = fle3d.decimate_cluster(mesh, grid, frames)
    if max_tris > 0 and mesh.tri_count > max_tris:
        # список треугольников копируем: сцена должна остаться целой,
        # её переиспользуют при подборе уровня упрощения
        mesh.triangles = list(mesh.triangles)
        fle3d.limit_triangles(mesh, max_tris)
    mesh.normals = fle3d.recalc_normals(mesh.positions, mesh.triangles)
    return mesh, frames


def save_scene(scene, dst, decimate=0, loop=True):
    """
    При необходимости упрощает сетку и записывает .fle3d.
    Возвращает (размер файла, mesh, frames).
    """
    mesh, frames = simplify_scene(scene, decimate)
    size = fle3d.write_fle3d(dst, mesh, frames=frames, fps=scene["out_fps"],
                             loop=loop)
    return size, mesh, frames


def main():
    ap = argparse.ArgumentParser(description="GLB -> FLE3D")
    ap.add_argument("source")
    ap.add_argument("target")
    ap.add_argument("--fps", type=int, default=24, help="кадров в секунду")
    ap.add_argument("--frames", type=int, default=0, help="длительность цикла в кадрах (0 — вся анимация)")
    ap.add_argument("--loop", action="store_true", default=True)
    ap.add_argument("--no-scale", action="store_true", help="не нормализовать по габаритам")
    ap.add_argument("--decimate", type=int, default=0,
                    help="упростить сетку: число ячеек по большей оси (12-20 -> ~800 граней)")
    ap.add_argument("--anim", default="",
                    help="клип анимации: номер или часть имени (например \"walk\")")
    args = ap.parse_args()

    scene = build_scene(args.source, fps=args.fps, frames_arg=args.frames,
                        anim_spec=args.anim, no_scale=args.no_scale)
    size, mesh, frames = save_scene(scene, args.target, decimate=args.decimate,
                                    loop=args.loop)

    print("done: %s" % args.target)
    print("  vertices  : %d" % mesh.vertex_count)
    print("  triangles : %d" % mesh.tri_count)
    print("  frames    : %d @ %d fps" % (scene["frame_count"], scene["out_fps"]))
    if scene["anim_count"]:
        print("  clip      : %d (%s) of %d" % (scene["anim_index"],
                                               scene["anim_name"] or "-",
                                               scene["anim_count"]))
    if scene["duration"] > 0.0:
        print("  cycle     : %.2f s" % scene["duration"])
    print("  size      : %d bytes (%.0f KB)" % (size, size / 1024.0))

    # --- budget warnings ---
    warn = False
    if mesh.vertex_count > 600:
        print("  WARNING: vertices > 600 - the lamp will draw it slowly.")
        warn = True
    if mesh.tri_count > 800:
        print("  WARNING: triangles > 800 - too dense for 48x16.")
        warn = True
    if size > 600 * 1024:
        print("  WARNING: file > 600 KB - loading from the card will take longer.")
        warn = True
    if not warn:
        print("  fits the lamp budget (see MODEL_FORMAT.md)")

    if mesh.tri_count < 20:
        print("  NOTE: very few triangles - the model may look flat on the matrix.")


if __name__ == "__main__":
    main()
