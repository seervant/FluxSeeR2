# -*- coding: utf-8 -*-
"""
Формат модели FleXSeeR (.fle3d) — запись и чтение.

Спецификация: ../MODEL_FORMAT.md

Формат рассчитан на то, чтобы ESP32-S3 прочитал файл с SD-карты за
доли секунды и разложил его в PSRAM без единого выделения памяти
загрузчиком: все смещения фиксированы в заголовке.

Все величины — little-endian.
"""

import struct
import math

MAGIC = b"FLE3"
VERSION = 1
HEADER_SIZE = 64
VERTEX_STRIDE = 24

# Флаги заголовка
FLAG_COLORS   = 1 << 0   # цвета вершин заполнены
FLAG_ANIMATED = 1 << 1   # есть кадры анимации
FLAG_LOOP     = 1 << 2   # анимация зациклена

# Полуразмер габаритов модели в единицах файла.
# Модель нормализуется так, чтобы по наибольшей оси полуразмер был 512.
MODEL_UNIT = 512.0

# Пределы формата (умещаются в u16 / u8)
MAX_VERTS = 65535
MAX_TRIS = 65535


# ---------------------------------------------------------------------------
# Модель в памяти конвертера
# ---------------------------------------------------------------------------
class Mesh:
    """Меш с вершинами, треугольниками и (опционально) цветами."""

    def __init__(self):
        self.positions = []   # [(x, y, z)]
        self.normals = []     # [(x, y, z)]
        self.colors = []      # [(r, g, b, a)]
        self.uvs = []         # [(u, v)]
        self.triangles = []   # [(i0, i1, i2)]

    @property
    def vertex_count(self):
        return len(self.positions)

    @property
    def tri_count(self):
        return len(self.triangles)


def mesh_bounds(mesh):
    """Габариты модели: (минимум, максимум)."""
    if not mesh.positions:
        return (0.0, 0.0, 0.0), (0.0, 0.0, 0.0)
    xs = [p[0] for p in mesh.positions]
    ys = [p[1] for p in mesh.positions]
    zs = [p[2] for p in mesh.positions]
    return (min(xs), min(ys), min(zs)), (max(xs), max(ys), max(zs))


def center_model(mesh, frames=None):
    """
    Сдвигает модель так, чтобы центр её габаритов оказался в начале координат.

    Прошивка ставит модель в начало координат сцены (ось цилиндра),
    поэтому центрировать должен конвертер: в исходном файле персонаж
    обычно стоит «ногами на нуле», и луч из наблюдателя его не находит.

    Габариты считаются по всем кадрам анимации, чтобы при цикле
    модель не «прыгала».
    """
    pts = list(mesh.positions)
    if frames:
        for fr in frames:
            pts.extend(fr)
    if not pts:
        return (0.0, 0.0, 0.0)

    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    zs = [p[2] for p in pts]
    cx = (min(xs) + max(xs)) * 0.5
    cy = (min(ys) + max(ys)) * 0.5
    cz = (min(zs) + max(zs)) * 0.5

    mesh.positions = [(p[0] - cx, p[1] - cy, p[2] - cz) for p in mesh.positions]
    if frames:
        for i, fr in enumerate(frames):
            frames[i] = [(p[0] - cx, p[1] - cy, p[2] - cz) for p in fr]
    return (cx, cy, cz)


def normalize_mesh(mesh, unit=MODEL_UNIT):
    """
    Масштабирует модель так, чтобы полуразмер габаритов по наибольшей оси
    стал равен unit. Возвращает коэффициент масштаба.
    """
    lo, hi = mesh_bounds(mesh)
    half = max((hi[0] - lo[0]) * 0.5, (hi[1] - lo[1]) * 0.5, (hi[2] - lo[2]) * 0.5)
    if half < 1e-9:
        return 1.0
    k = unit / half
    mesh.positions = [(p[0] * k, p[1] * k, p[2] * k) for p in mesh.positions]
    if mesh.normals:
        mesh.normals = [_norm(n) for n in mesh.normals]
    return k


def _norm(v):
    l = math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])
    if l < 1e-9:
        return (0.0, 0.0, 1.0)
    return (v[0] / l, v[1] / l, v[2] / l)


def decimate_cluster(mesh, grid, frames=None):
    """
    Упрощение сетки методом кластеризации вершин.

    Вершины сортируются по ячейкам равномерной сетки; вершины одной ячейки
    сливаются в одну. Вырожденные треугольники выбрасываются.

    grid — число ячеек по наибольшей оси (16 -> примерно 800 граней).
    frames — список кадров анимации; к ним применяется та же раскладка.

    Возвращает (mesh, frames).
    """
    if grid <= 1 or not mesh.positions:
        return mesh, frames

    lo, hi = mesh_bounds(mesh)
    size = [hi[i] - lo[i] for i in range(3)]
    longest = max(size)
    if longest <= 1e-9:
        return mesh, frames
    cell = longest / float(grid)

    mapping = {}
    new_pos = []
    color_sum = []
    color_n = []
    remap = []

    for i, p in enumerate(mesh.positions):
        key = (int(math.floor((p[0] - lo[0]) / cell)),
               int(math.floor((p[1] - lo[1]) / cell)),
               int(math.floor((p[2] - lo[2]) / cell)))
        idx = mapping.get(key)
        if idx is None:
            idx = len(new_pos)
            mapping[key] = idx
            new_pos.append([p[0], p[1], p[2]])
            color_sum.append([0.0, 0.0, 0.0, 0.0])
            color_n.append(0)
        remap.append(idx)

        c = mesh.colors[i] if i < len(mesh.colors) else (255, 255, 255, 255)
        cs = color_sum[idx]
        cs[0] += c[0]; cs[1] += c[1]; cs[2] += c[2]; cs[3] += c[3]
        color_n[idx] += 1

    # средние позиции и цвета по кластерам
    acc = {}
    for i, p in enumerate(mesh.positions):
        idx = remap[i]
        a = acc.get(idx)
        if a is None:
            acc[idx] = [p[0], p[1], p[2]]
        else:
            a[0] += p[0]; a[1] += p[1]; a[2] += p[2]
    for idx, a in acc.items():
        n = float(color_n[idx])
        new_pos[idx] = [a[0] / n, a[1] / n, a[2] / n]

    new_colors = []
    for idx in range(len(new_pos)):
        cs = color_sum[idx]
        n = float(color_n[idx])
        new_colors.append((int(cs[0] / n), int(cs[1] / n),
                           int(cs[2] / n), int(cs[3] / n)))

    new_tris = []
    seen = set()
    for t in mesh.triangles:
        a, b, c = remap[t[0]], remap[t[1]], remap[t[2]]
        if a == b or b == c or a == c:
            continue
        # после прореживания разные треугольники могут совпасть —
        # дубликаты убираем, иначе файл распухает
        key = (min(a, b, c), a + b + c - min(a, b, c) - max(a, b, c), max(a, b, c))
        if key in seen:
            continue
        seen.add(key)
        new_tris.append((a, b, c))

    out = Mesh()
    out.positions = [tuple(p) for p in new_pos]
    out.colors = new_colors
    out.triangles = new_tris
    out.normals = recalc_normals(out.positions, out.triangles)

    # --- кадры анимации ---
    # Позиции кадра усредняются по тем же кластерам, что и вершины покоя.
    # Иначе в кадре осталось бы больше точек, чем вершин в модели
    # (запись файла проверяет равенство длин и падает).
    new_frames = None
    if frames:
        members = {}
        for i, idx in enumerate(remap):
            members.setdefault(idx, []).append(i)
        new_frames = []
        for fr in frames:
            nf = []
            for idx in range(len(new_pos)):
                ids = members[idx]
                ax = ay = az = 0.0
                for i in ids:
                    p = fr[i]
                    ax += p[0]; ay += p[1]; az += p[2]
                n = float(len(ids))
                nf.append((ax / n, ay / n, az / n))
            new_frames.append(nf)

    return out, new_frames


def limit_triangles(mesh, max_tris):
    """
    Оставляет не больше max_tris треугольников, выбрасывая самые мелкие.

    После прореживания тяжёлой модели треугольников может остаться больше,
    чем нужно лампе. Мелкие грани на матрице всё равно не видны, поэтому
    их выгоднее убрать, чем рисовать.
    """
    if mesh.tri_count <= max_tris:
        return mesh

    tris = mesh.triangles
    areas = []
    for t in tris:
        a, b, c = mesh.positions[t[0]], mesh.positions[t[1]], mesh.positions[t[2]]
        u = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
        v = (c[0] - a[0], c[1] - a[1], c[2] - a[2])
        n = (u[1] * v[2] - u[2] * v[1],
             u[2] * v[0] - u[0] * v[2],
             u[0] * v[1] - u[1] * v[0])
        areas.append((n[0] * n[0] + n[1] * n[1] + n[2] * n[2], t))

    areas.sort(key=lambda x: -x[0])
    keep = [t for (_, t) in areas[:max_tris]]
    mesh.triangles = keep
    return mesh


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


def _qnorm(q):
    l = math.sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3])
    if l < 1e-9:
        return (1.0, 0.0, 0.0, 0.0)
    return (q[0] / l, q[1] / l, q[2] / l, q[3] / l)


def _qslerp(a, b, t):
    """Интерполяция кватернионов. Кватернионы в порядке (w, x, y, z)."""
    a = _qnorm(a)
    b = _qnorm(b)
    d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]
    if d < 0.0:
        b = (-b[0], -b[1], -b[2], -b[3])
        d = -d
    if d > 0.9995:
        r = tuple(a[i] + (b[i] - a[i]) * t for i in range(4))
        return _qnorm(r)
    th = math.acos(max(-1.0, min(1.0, d)))
    s = math.sin(th)
    f1 = math.sin((1.0 - t) * th) / s
    f2 = math.sin(t * th) / s
    return _qnorm(tuple(a[i] * f1 + b[i] * f2 for i in range(4)))


# ---------------------------------------------------------------------------
# Запись файла
# ---------------------------------------------------------------------------
def write_fle3d(path, mesh, frames=None, fps=24, loop=True,
                model_height_mm=0.0, model_radius=None):
    """
    Записывает модель в файл .fle3d.

    mesh    — объект Mesh
    frames  — список кадров анимации; каждый кадр — список позиций вершин
              (len == mesh.vertex_count). None — статичная модель.
    fps     — кадров в секунду
    """
    if mesh.vertex_count == 0 or mesh.tri_count == 0:
        raise ValueError("модель пустая")
    if mesh.vertex_count > MAX_VERTS:
        raise ValueError("слишком много вершин: %d" % mesh.vertex_count)
    if mesh.tri_count > MAX_TRIS:
        raise ValueError("слишком много треугольников: %d" % mesh.tri_count)

    flags = 0
    if mesh.colors:
        flags |= FLAG_COLORS
    animated = bool(frames)
    if animated:
        flags |= FLAG_ANIMATED
    if loop:
        flags |= FLAG_LOOP

    # --- блок вершин ---
    vblock = bytearray()
    for i in range(mesh.vertex_count):
        p = mesh.positions[i]
        vblock += struct.pack("<3h", _clip_i16(p[0]), _clip_i16(p[1]), _clip_i16(p[2]))

        n = mesh.normals[i] if i < len(mesh.normals) else (0.0, 0.0, 1.0)
        vblock += struct.pack("<3b", _clip_i8(n[0] * 127.0), _clip_i8(n[1] * 127.0),
                              _clip_i8(n[2] * 127.0))

        c = mesh.colors[i] if i < len(mesh.colors) else (255, 255, 255, 255)
        vblock += struct.pack("<4B", _clip_u8(c[0]), _clip_u8(c[1]),
                              _clip_u8(c[2]), _clip_u8(c[3] if len(c) > 3 else 255))

        uv = mesh.uvs[i] if i < len(mesh.uvs) else (0.0, 0.0)
        vblock += struct.pack("<2H", _clip_u16(uv[0] * 65535.0), _clip_u16(uv[1] * 65535.0))
        vblock += b"\x00" * 7                      # запас под будущие поля
    assert len(vblock) == mesh.vertex_count * VERTEX_STRIDE

    # --- блок индексов ---
    iblock = bytearray()
    for t in mesh.triangles:
        iblock += struct.pack("<3H", t[0], t[1], t[2])

    # --- блок анимации ---
    ablock = bytearray()
    frame_count = 1
    if animated:
        frame_count = len(frames)
        for fr in frames:
            if len(fr) != mesh.vertex_count:
                raise ValueError("кадр анимации: %d вершин вместо %d"
                                 % (len(fr), mesh.vertex_count))
            for p in fr:
                ablock += struct.pack("<3h", _clip_i16(p[0]), _clip_i16(p[1]), _clip_i16(p[2]))

    vertex_off = HEADER_SIZE
    index_off = vertex_off + len(vblock)
    anim_off = index_off + len(iblock)

    # --- радиус модели (половина диагонали габаритов) ---
    if model_radius is None:
        lo, hi = mesh_bounds(mesh)
        dx = (hi[0] - lo[0]) * 0.5
        dy = (hi[1] - lo[1]) * 0.5
        dz = (hi[2] - lo[2]) * 0.5
        model_radius = math.sqrt(dx * dx + dy * dy + dz * dz)

    total = anim_off + len(ablock)

    header = bytearray(HEADER_SIZE)
    struct.pack_into("<4s", header, 0, MAGIC)
    struct.pack_into("<H", header, 4, VERSION)
    struct.pack_into("<H", header, 6, flags)
    struct.pack_into("<H", header, 8, mesh.vertex_count)
    struct.pack_into("<H", header, 10, mesh.tri_count)
    struct.pack_into("<H", header, 12, frame_count)
    struct.pack_into("<H", header, 14, fps)
    struct.pack_into("<H", header, 16, VERTEX_STRIDE)
    struct.pack_into("<H", header, 18, 0)
    struct.pack_into("<I", header, 20, vertex_off)
    struct.pack_into("<I", header, 24, index_off)
    struct.pack_into("<I", header, 28, anim_off)
    struct.pack_into("<I", header, 32, total)
    struct.pack_into("<f", header, 36, model_radius)
    struct.pack_into("<f", header, 40, model_height_mm)

    with open(path, "wb") as f:
        f.write(header)
        f.write(vblock)
        f.write(iblock)
        f.write(ablock)
    return total


def _clip_i16(v):
    return max(-32768, min(32767, int(round(v))))


def _clip_i8(v):
    return max(-128, min(127, int(round(v))))


def _clip_u8(v):
    return max(0, min(255, int(round(v))))


def _clip_u16(v):
    return max(0, min(65535, int(round(v))))


# ---------------------------------------------------------------------------
# Чтение (для проверки конвертеров)
# ---------------------------------------------------------------------------
def read_fle3d(path):
    """Читает .fle3d и возвращает словарь с заголовком и данными."""
    with open(path, "rb") as f:
        data = f.read()

    if data[0:4] != MAGIC:
        raise ValueError("не тот файл: %s" % path)

    h = {}
    (h["version"], h["flags"], h["vertCount"], h["triCount"], h["frameCount"],
     h["animFps"], h["vtxStride"], _) = struct.unpack_from("<8H", data, 4)
    (h["vertexOffset"], h["indexOffset"], h["animOffset"], h["totalSize"],
     h["modelRadius"], h["modelHeightMm"]) = struct.unpack_from("<IIIIff", data, 20)

    verts = []
    for i in range(h["vertCount"]):
        off = h["vertexOffset"] + i * h["vtxStride"]
        p = struct.unpack_from("<3h", data, off)
        n = struct.unpack_from("<3b", data, off + 6)
        c = struct.unpack_from("<4B", data, off + 9)
        uv = struct.unpack_from("<2H", data, off + 13)
        verts.append({"pos": (float(p[0]), float(p[1]), float(p[2])),
                      "nrm": (n[0] / 127.0, n[1] / 127.0, n[2] / 127.0),
                      "col": c, "uv": (uv[0] / 65535.0, uv[1] / 65535.0)})

    tris = []
    for i in range(h["triCount"]):
        off = h["indexOffset"] + i * 6
        tris.append(struct.unpack_from("<3H", data, off))

    frames = []
    if h["frameCount"] > 1:
        for f in range(h["frameCount"]):
            base = h["animOffset"] + f * h["vertCount"] * 6
            fr = []
            for i in range(h["vertCount"]):
                p = struct.unpack_from("<3h", data, base + i * 6)
                fr.append((float(p[0]), float(p[1]), float(p[2])))
            frames.append(fr)

    h["vertices"] = verts
    h["triangles"] = tris
    h["frames"] = frames
    h["fileSize"] = len(data)
    return h


if __name__ == "__main__":
    import sys
    if len(sys.argv) < 2:
        print("usage: fle3d.py <file.fle3d>")
        sys.exit(1)
    info = read_fle3d(sys.argv[1])
    print("version      : %d" % info["version"])
    print("flags        : 0x%04X" % info["flags"])
    print("vertices     : %d" % info["vertCount"])
    print("triangles    : %d" % info["triCount"])
    print("frames       : %d @ %d fps" % (info["frameCount"], info["animFps"]))
    print("vertex stride: %d" % info["vtxStride"])
    print("model radius : %.1f ед." % info["modelRadius"])
    print("file size    : %d байт" % info["fileSize"])
