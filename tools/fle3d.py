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
