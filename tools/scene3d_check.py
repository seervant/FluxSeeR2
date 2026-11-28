#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Проверка математики эффектов 3D-сцены на стороне хоста.
Полностью повторяет формулы firmware/src/Scene3D.cpp и Orientation.cpp.
"""
import math
import sys
try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

# --- геометрия корпуса ---
R = 80.0          # радиус, мм
H = 160.0         # высота, мм
W = 48            # столбцов
HT = 16           # строк
COL_DIR = 1

# Столбец с «прицелом» — как в firmware/include/Config.h.
# Наблюдатель стоит напротив него, поэтому шов ленты (47 -> 0)
# оказывается за спиной, а объект в центре сцены не разрезается.
CROSSHAIR_COL = 24


def eye_in_cal_frame(dist_mm, eye_h_mm):
    """Положение наблюдателя — как eyeInCalFrame() в EffectScene3D.cpp."""
    ang = COL_DIR * CROSSHAIR_COL * (2 * math.pi / W)
    return (math.sin(ang) * dist_mm, eye_h_mm, math.cos(ang) * dist_mm)

def vadd(a, b): return (a[0]+b[0], a[1]+b[1], a[2]+b[2])
def vsub(a, b): return (a[0]-b[0], a[1]-b[1], a[2]-b[2])
def vmul(a, s): return (a[0]*s, a[1]*s, a[2]*s)
def vdot(a, b): return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]
def vcross(a, b):
    return (a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0])
def vlen(a): return math.sqrt(vdot(a, a))
def vnorm(a):
    l = vlen(a)
    return (0.0, 0.0, 0.0) if l < 1e-9 else vmul(a, 1.0/l)

def qmul(a, b):
    aw, ax, ay, az = a
    bw, bx, by, bz = b
    return (aw*bw - ax*bx - ay*by - az*bz,
            aw*bx + ax*bw + ay*bz - az*by,
            aw*by - ax*bz + ay*bw + az*bx,
            aw*bz + ax*by - ay*bx + az*bw)

def qnorm(q):
    l = math.sqrt(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3])
    return (q[0]/l, q[1]/l, q[2]/l, q[3]/l)

def qrot(q, v):
    """v' = q v q*"""
    w, x, y, z = q
    qv = (x, y, z)
    t = vmul(vcross(qv, v), 2.0)
    return vadd(vadd(v, vmul(t, w)), vcross(qv, t))

def qrot_inv(q, v):
    w, x, y, z = q
    qv = (-x, -y, -z)
    t = vmul(vcross(qv, v), 2.0)
    return vadd(vadd(v, vmul(t, w)), vcross(qv, t))

def qaxisangle(axis, angle):
    a = vnorm(axis)
    s = math.sin(angle*0.5)
    return (math.cos(angle*0.5), a[0]*s, a[1]*s, a[2]*s)

# --- фигуры ---
def build_cube(S):
    return [((1,0,0),S), ((-1,0,0),S), ((0,1,0),S), ((0,-1,0),S),
            ((0,0,1),S), ((0,0,-1),S)]

def build_octa(S):
    k = 1/math.sqrt(3)
    pl = []
    for sx in (1,-1):
        for sy in (1,-1):
            for sz in (1,-1):
                pl.append(((sx*k, sy*k, sz*k), S))
    return pl

def build_prism(S):
    ap = S*math.cos(math.radians(30))
    pl = []
    for i in range(6):
        a = i*math.pi/3
        pl.append(((math.sin(a),0.0,math.cos(a)), ap))
    pl.append(((0,1,0),S)); pl.append(((0,-1,0),S))
    return pl

def convex_hit(planes, o, d):
    t0, t1 = -1e30, 1e30
    enter = None
    for i,(n,dd) in enumerate(planes):
        denom = vdot(n, d)
        num = dd - vdot(n, o)
        if abs(denom) < 1e-6:
            if num < 0: return None
            continue
        t = num/denom
        if denom > 0:
            if t < t1: t1 = t
        else:
            if t > t0: t0, enter = t, i
        if t0 > t1: return None
    if enter is None: return None
    if t0 > 0: return (t0, enter)
    if t1 > 0: return (t1, None)
    return None

def ray_sphere(o, d, S):
    b = vdot(o, d)
    c = vdot(o,o) - S*S
    disc = b*b - c
    if disc < 0: return None
    sq = math.sqrt(disc)
    t = -b - sq
    if t <= 0: t = -b + sq
    if t <= 0: return None
    return (t, None)

def edge_distance(planes, face, q):
    ni = planes[face][0]
    best = 1e9
    for j,(nj,dj) in enumerate(planes):
        if j == face: continue
        w = vcross(ni, nj)
        w2 = vdot(w,w)
        if w2 < 1e-6: continue
        p0 = vmul(vadd(vmul(vcross(nj,w), planes[face][1]), vmul(vcross(w,ni), dj)), 1.0/w2)
        dist = abs(vdot(vsub(q,p0), w))/math.sqrt(w2)
        best = min(best, dist)
    return best

def render(figure, size_pct, edge_mm, body_to_scene, eye_scene):
    S = R*size_pct/100.0
    sphere = (figure == 3)
    planes = [] if sphere else {0: build_cube, 1: build_octa, 2: build_prism}[figure](S)
    row_h = H/HT
    grid = [[' ']*W for _ in range(HT)]
    for x in range(W):
        ang = COL_DIR*x*(2*math.pi/W)
        sn, cs = math.sin(ang), math.cos(ang)
        n_body = (sn, 0.0, cs)
        for y in range(HT):
            hh = (y+0.5)*row_h - H*0.5
            p_body = (sn*R, hh, cs*R)
            p_scene = qrot(body_to_scene, p_body)
            n_scene = qrot(body_to_scene, n_body)
            to_eye = vsub(eye_scene, p_scene)
            dist = vlen(to_eye)
            if vdot(n_scene, to_eye) <= 0: continue
            d = vmul(to_eye, -1.0/dist)
            hit = ray_sphere(eye_scene, d, S) if sphere else convex_hit(planes, eye_scene, d)
            if hit is None: continue
            t, face = hit
            q = vadd(eye_scene, vmul(d, t))
            if sphere:
                nrm = vnorm(q); lam = max(0.0, -vdot(nrm, d)); ek = max(0.0, min(1.0, ((1-lam)-0.72)/0.26))
            else:
                nrm = planes[face][0]; lam = max(0.0, -vdot(nrm, d))
                dk = edge_distance(planes, face, q)
                ek = max(0.0, min(1.0, (edge_mm+1.5-dk)/1.5))
            shade = (0.16+0.84*lam)
            shade *= (1.0-0.20*max(0.0,min(1.0,(t-380.0)/420.0)))
            grid[HT-1-y][x] = ' .:-=+*#%@'[max(0, min(9, int(shade*9.99)))]
            if ek > 0.5: grid[HT-1-y][x] = '@'
    return grid

def show(title, grid):
    print("=== %s ===" % title)
    for row in grid: print("|" + "".join(row) + "|")
    print()

IDENT = (1.0, 0.0, 0.0, 0.0)

# 1) Прямо перед лампой, объект по центру
eye = eye_in_cal_frame(600.0, 100.0)
show("Куб, взгляд прямо, глаз на 100 мм выше центра", render(0, 55, 8, IDENT, eye))

# 2) Взгляд сверху: глаз выше и ближе
show("Куб, взгляд сверху (глаз 250 мм выше, 450 мм)",
     render(0, 55, 8, IDENT, eye_in_cal_frame(450.0, 250.0)))

# 3) Лампа повёрнута на 90° вокруг вертикали -> эффект 1 (гравитация):
#    объект остаётся в мире, значит в системе корпуса он повёрнут на -90°
yaw = qaxisangle((0,1,0), math.radians(-90))
show("Эффект 1: лампа повёрнута на 90°, объект зафиксирован в мире", render(0, 55, 8, yaw, eye))

# 4) Эффект 2 (корпус): система сцены = корпус, глаз перемещается
show("Эффект 2: лампа повёрнута на 90°, объект жёстко связан с корпусом",
     render(0, 55, 8, IDENT, qrot_inv(yaw, eye)))

# 4a) Наклон лампы вперёд на 30° — здесь режимы обязаны различаться:
#     в эффекте 1 куб остаётся вертикальным в мире,
#     в эффекте 2 куб наклоняется вместе с корпусом.
pitch = qaxisangle((1,0,0), math.radians(30))
g1 = render(0, 55, 8, pitch, eye)
g2 = render(0, 55, 8, IDENT, qrot_inv(pitch, eye))
show("Эффект 1: лампа наклонена на 30°, куб остаётся вертикальным в мире", g1)
show("Эффект 2: лампа наклонена на 30°, куб наклоняется вместе с корпусом", g2)
print("Наклон 30°: картинки режимов %s" % ("РАЗНЫЕ (верно)" if g1 != g2 else "ОДИНАКОВЫЕ (ошибка)"))
print()

# 4b) Рыскание на 45° — тоже должно давать разные картинки
yaw45 = qaxisangle((0,1,0), math.radians(45))
h1 = render(0, 55, 8, yaw45, eye)
h2 = render(0, 55, 8, IDENT, qrot_inv(yaw45, eye))
print("Рыскание 45°: картинки режимов %s" % ("РАЗНЫЕ (верно)" if h1 != h2 else "ОДИНАКОВЫЕ"))
print()

# 4c) Неподвижная лампа: оба режима обязаны совпадать
k1 = render(0, 55, 8, IDENT, eye)
k2 = render(0, 55, 8, IDENT, eye)
print("Лампа неподвижна: картинки совпадают: %s" % (k1 == k2))
print()

# 5) Прочие фигуры
for fig, name in ((1,"Октаэдр"), (2,"Призма"), (3,"Сфера")):
    show(name, render(fig, 55, 8, IDENT, eye))

# --- проверка ориентации: поворот лампы по рысканию (вокруг вертикали) ---
print("=== Проверка оценщика ориентации ===")
q = IDENT
w = (0.0, 1.0, 0.0)          # 1 рад/с вокруг вертикали тела, 50 Гц
for i in range(50):
    q = qnorm(qmul(q, qaxisangle(w, 1.0/50.0)))
print("Накопленный поворот за 1 с, рад: %.3f (ожидаем 1.000)" % (2*math.acos(max(-1,min(1,q[0])))))
v = qrot_inv(q, (0.0, 1.0, 0.0))
print("Мировая вертикаль в системе корпуса: x=%.4f y=%.4f (ожидаем 0, 1)"
      % (v[0], v[1]))

# Проверка: рыскание не должно «уводить» наклон, а поворот вокруг горизонтальной
# оси должен наклонять вертикаль ровно на угол поворота
q2 = IDENT
w2 = (1.0, 0.0, 0.0)         # поворот вокруг горизонтальной оси тела
for i in range(50):
    q2 = qnorm(qmul(q2, qaxisangle(w2, 1.0/50.0)))
v2 = qrot_inv(q2, (0.0, 1.0, 0.0))
print("Наклон мира в корпусе после поворота на 1 рад: y=%.3f (ожидаем cos=0.540)"
      % v2[1])

# --- проверка калибровки: восстановление осей корпуса из поз ---
print("=== Проверка калибровки по позам ===")
# Позы в мире: 1) вертикально, 2) наклон вправо, 3) наклон назад
def rot_axis(ax, ang):
    return qaxisangle(ax, ang)

q1 = rot_axis((0,0,1), math.radians(-90))   # наклон вправо (поворот вокруг Z тела)
q2 = rot_axis((1,0,0), math.radians(-90))   # наклон назад (вокруг X тела)
def body_in_world(v, q): return qrot(q, v)
g1 = body_in_world((0,-1,0), IDENT)   # гравитация в системе корпуса при позе 1
g2 = body_in_world((0,-1,0), q1)      # поза 2
g3 = body_in_world((0,-1,0), q1 and qmul(q1, q2))  # поза 3
# переводим в систему MPU: считаем, что MPU совпадает с миром в позе 1
y_axis = vnorm(vmul(g1, -1.0))
x_axis = vnorm(g2)
d = vdot(x_axis, y_axis)
x_axis = vnorm(vsub(x_axis, vmul(y_axis, d)))
z_axis = vcross(x_axis, y_axis)
print("ортогональность X,Y: %.3f (ожидаем 0)" % abs(d))
print("проверка оси Z по позе 3: %.3f (ожидаем 1.000)" % vdot(g3, vmul(z_axis,-1.0)))
print("det(X,Y,Z): %.3f" % (vdot(x_axis, vcross(y_axis, z_axis))))
