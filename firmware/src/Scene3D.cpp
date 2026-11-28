#include "Scene3D.h"
#include "Config.h"
#include "LedGeometry.h"
#include "Model3D.h"

// ============================================================================
// РЕНДЕР 3D-СЦЕНЫ — реализация
// ============================================================================

// Фигура задаётся набором полупространств:  n · p <= d
// (n — внешняя нормаль грани, d — расстояние от начала координат)
#define SCENE_MAX_PLANES 10

struct SceneShape {
  uint8_t count = 0;
  Vec3    n[SCENE_MAX_PLANES];
  float   d[SCENE_MAX_PLANES];
};

// ---------------------------------------------------------------------------
// Построение фигур
// ---------------------------------------------------------------------------
static void addPlane(SceneShape& sh, const Vec3& n, float d) {
  if (sh.count >= SCENE_MAX_PLANES) return;
  sh.n[sh.count] = n.normalized();
  sh.d[sh.count] = d;
  sh.count++;
}

// Куб: |x| <= S, |y| <= S, |z| <= S
static void buildCube(SceneShape& sh, float S) {
  sh = SceneShape();
  addPlane(sh, Vec3( 1, 0, 0), S);
  addPlane(sh, Vec3(-1, 0, 0), S);
  addPlane(sh, Vec3( 0, 1, 0), S);
  addPlane(sh, Vec3( 0,-1, 0), S);
  addPlane(sh, Vec3( 0, 0, 1), S);
  addPlane(sh, Vec3( 0, 0,-1), S);
}

// Октаэдр: |x| + |y| + |z| <= S
static void buildOcta(SceneShape& sh, float S) {
  sh = SceneShape();
  const float k = 0.5773502692f;   // 1/sqrt(3)
  addPlane(sh, Vec3( 1, 1, 1), S * k);
  addPlane(sh, Vec3( 1, 1,-1), S * k);
  addPlane(sh, Vec3( 1,-1, 1), S * k);
  addPlane(sh, Vec3( 1,-1,-1), S * k);
  addPlane(sh, Vec3(-1, 1, 1), S * k);
  addPlane(sh, Vec3(-1, 1,-1), S * k);
  addPlane(sh, Vec3(-1,-1, 1), S * k);
  addPlane(sh, Vec3(-1,-1,-1), S * k);
}

// Шестигранная колонна: сечение — правильный шестиугольник в плоскости XZ,
// высота 2S по оси Y
static void buildPrism(SceneShape& sh, float S) {
  sh = SceneShape();
  const float apothem = S * 0.8660254038f;   // S * cos(30°)
  for (uint8_t i = 0; i < 6; i++) {
    float a = (float)i * (PI / 3.0f);
    addPlane(sh, Vec3(sinf(a), 0.0f, cosf(a)), apothem);
  }
  addPlane(sh, Vec3(0, 1, 0), S);
  addPlane(sh, Vec3(0,-1, 0), S);
}

// ---------------------------------------------------------------------------
// Пересечение луча с выпуклым многогранником (отсечение по плоскостям)
// ---------------------------------------------------------------------------
static bool convexHit(const SceneShape& sh, const Vec3& o, const Vec3& dir,
                       float& tHit, uint8_t& faceIdx) {
  const float INF = 1e30f;
  float t0 = -INF, t1 = INF;
  uint8_t enter = 0xFF;

  for (uint8_t i = 0; i < sh.count; i++) {
    float denom = sh.n[i].dot(dir);
    float num = sh.d[i] - sh.n[i].dot(o);

    if (fabsf(denom) < 1e-6f) {
      if (num < 0.0f) return false;      // луч параллелен и вне тела
      continue;
    }
    float t = num / denom;

    // Условие входа в полупространство: n·(o + t*dir) <= d
    //   при denom > 0 ограничение сверху (t <= t)
    //   при denom < 0 ограничение снизу (t >= t)
    if (denom > 0.0f) {
      if (t < t1) t1 = t;                 // верхняя граница
    } else {
      if (t > t0) { t0 = t; enter = i; }  // нижняя граница — вход в тело
    }
    if (t0 > t1) return false;
  }

  if (enter == 0xFF) return false;

  if (t0 > 0.0f) {
    tHit = t0;
    faceIdx = enter;
    return true;
  }
  if (t1 > 0.0f) {                        // луч начался внутри тела
    tHit = t1;
    faceIdx = 0xFF;
    return true;
  }
  return false;
}

// Пересечение луча со сферой радиуса S с центром в начале координат
static bool raySphere(const Vec3& o, const Vec3& dir, float S, float& tHit) {
  float b = o.dot(dir);
  float c = o.lengthSquared() - S * S;
  float disc = b * b - c;
  if (disc < 0.0f) return false;
  float sq = sqrtf(disc);
  float t = -b - sq;
  if (t <= 0.0f) t = -b + sq;
  if (t <= 0.0f) return false;
  tHit = t;
  return true;
}

// ---------------------------------------------------------------------------
// Расстояние от точки попадания до ближайшего ребра грани
// ---------------------------------------------------------------------------
static float edgeDistance(const SceneShape& sh, uint8_t faceIdx, const Vec3& q) {
  if (faceIdx >= sh.count) return 1e9f;

  const Vec3& ni = sh.n[faceIdx];
  float best = 1e9f;

  for (uint8_t j = 0; j < sh.count; j++) {
    if (j == faceIdx) continue;
    Vec3 w = ni.cross(sh.n[j]);
    float w2 = w.lengthSquared();
    if (w2 < 1e-6f) continue;             // параллельные грани — ребра нет

    // Точка на линии пересечения двух плоскостей
    Vec3 p0 = (sh.n[j].cross(w) * sh.d[faceIdx] + w.cross(ni) * sh.d[j]) / w2;
    float dist = fabsf((q - p0).dot(w)) / sqrtf(w2);
    if (dist < best) best = dist;
  }
  return best;
}

// ---------------------------------------------------------------------------
// Цвет точки
// ---------------------------------------------------------------------------
static CRGB shadeColor(uint8_t hue, float shade, float edgeK) {
  // Грань: насыщенный оттенок с затенением по нормали
  uint8_t valFace = (uint8_t)constrain(255.0f * shade, 0.0f, 255.0f);
  CRGB face = CHSV(hue, 235, valFace);

  // Ребро: почти белый, с лёгким оттенком фигуры
  uint8_t valEdge = (uint8_t)constrain(255.0f * (shade * 0.45f + 0.55f), 0.0f, 255.0f);
  CRGB edge = CHSV((uint8_t)(hue + 12), 70, valEdge);

  // Плавное смешивание к ребру
  CRGB out;
  out.r = (uint8_t)constrain(face.r + (edge.r - (int)face.r) * edgeK, 0.0f, 255.0f);
  out.g = (uint8_t)constrain(face.g + (edge.g - (int)face.g) * edgeK, 0.0f, 255.0f);
  out.b = (uint8_t)constrain(face.b + (edge.b - (int)face.b) * edgeK, 0.0f, 255.0f);
  return out;
}

// ---------------------------------------------------------------------------
// Сдвиг оттенка (для моделей: позволяет перекрасить модель параметром COLOR)
// ---------------------------------------------------------------------------
static CRGB hueShift(CRGB c, uint8_t shift) {
  if (shift == 0) return c;

  float r = c.r / 255.0f, g = c.g / 255.0f, b = c.b / 255.0f;
  float mx = fmaxf(r, fmaxf(g, b));
  float mn = fminf(r, fminf(g, b));
  float d = mx - mn;

  float h = 0.0f;
  if (d > 1e-6f) {
    if (mx == r)      h = fmodf((g - b) / d, 6.0f);
    else if (mx == g) h = (b - r) / d + 2.0f;
    else              h = (r - g) / d + 4.0f;
    h *= 60.0f;
    if (h < 0) h += 360.0f;
  }
  float s = (mx <= 1e-6f) ? 0.0f : d / mx;
  float v = mx;

  h = fmodf(h + (float)shift * 1.40625f, 360.0f);   // 1 единица параметра = 1.40625°

  float c2 = v * s;
  float x = c2 * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
  float mm = v - c2;
  float rr = 0, gg = 0, bb = 0;
  if (h < 60)       { rr = c2; gg = x;  bb = 0; }
  else if (h < 120) { rr = x;  gg = c2; bb = 0; }
  else if (h < 180) { rr = 0;  gg = c2; bb = x; }
  else if (h < 240) { rr = 0;  gg = x;  bb = c2; }
  else if (h < 300) { rr = x;  gg = 0;  bb = c2; }
  else              { rr = c2; gg = 0;  bb = x; }

  return CRGB((uint8_t)((rr + mm) * 255.0f),
              (uint8_t)((gg + mm) * 255.0f),
              (uint8_t)((bb + mm) * 255.0f));
}

// ---------------------------------------------------------------------------
// Отрисовка 3D-модели с SD-карты
//
// Тот же луч «глаз -> поверхность цилиндра», но пересечение ищется
// в сетке треугольников (BVH внутри Model3D). Цвет берётся из вершин
// с интерполяцией по барицентрическим координатам, рёбра подсвечиваются
// по расстоянию до края треугольника.
// ---------------------------------------------------------------------------
static void renderModel(const SceneOptions& opt, const Quat& bodyToScene, const Vec3& eyeScene) {
  if (!model3dLoaded()) return;

  const float R      = SCENE_CYL_RADIUS_MM;
  const float H      = SCENE_CYL_HEIGHT_MM;
  const float rowH   = H / (float)MATRIX_HEIGHT;
  const float colAng = 2.0f * PI / (float)MATRIX_WIDTH;

  // Радиус модели в мм (параметр SIZE) и перевод единиц модели в миллиметры
  float radiusMm = R * constrain(opt.sizePct, 5.0f, 100.0f) / 100.0f;
  float radiusUnits = model3dRadius();
  if (radiusUnits <= 0.0f) return;
  float scale = radiusMm / radiusUnits;

  Vec3 eyeUnits = eyeScene / scale;

  const float edgeWidth = opt.edgeMm;
  const float edgeFade  = 1.5f;

  for (uint8_t x = 0; x < MATRIX_WIDTH; x++) {
    float ang = (float)SCENE_COL_DIR * (float)x * colAng;
    float sn = sinf(ang), cs = cosf(ang);
    Vec3 nBody(sn, 0.0f, cs);

    for (uint8_t y = 0; y < MATRIX_HEIGHT; y++) {
      float h = ((float)y + 0.5f) * rowH - H * 0.5f;

      Vec3 pBody(sn * R, h, cs * R);
      Vec3 pScene = bodyToScene.rotate(pBody);
      Vec3 nScene = bodyToScene.rotate(nBody);

      Vec3 toEye = eyeScene - pScene;
      float dist = toEye.length();
      if (dist < 1e-3f) continue;
      if (nScene.dot(toEye) <= 0.0f) continue;

      Vec3 dir = (toEye * -1.0f) / dist;      // от наблюдателя вглубь сцены

      ModelHit hit = model3dCast(eyeUnits, dir);
      if (!hit.hit) continue;

      float tMm = hit.t * scale;
      Vec3 q = eyeScene + dir * tMm;

      // Треугольник и его вершины (в единицах модели)
      uint16_t i0, i1, i2;
      model3dTriangle(hit.tri, i0, i1, i2);
      Vec3 a = model3dVertex(i0);
      Vec3 b = model3dVertex(i1);
      Vec3 c = model3dVertex(i2);

      Vec3 e1 = b - a;
      Vec3 e2 = c - a;
      Vec3 cr = e1.cross(e2);
      float len = cr.length();
      if (len < 1e-9f) continue;
      Vec3 nrm = cr / len;

      float lambert = -nrm.dot(dir);
      if (lambert < 0.0f) lambert = 0.0f;
      float shade = 0.16f + 0.84f * lambert;

      float depth = constrain((tMm - 380.0f) / 420.0f, 0.0f, 1.0f);
      shade *= (1.0f - 0.20f * depth);

      // Расстояние до ребра треугольника: минимальная барицентрическая
      // координата, умноженная на высоту треугольника
      float wgt = 1.0f - hit.u - hit.v;
      float minBary = fminf(hit.u, fminf(hit.v, wgt));
      if (minBary < 0.0f) minBary = 0.0f;
      float area = 0.5f * len;
      float maxEdge = fmaxf(e1.length(), fmaxf((c - b).length(), (a - c).length()));
      float alt = (maxEdge > 1e-6f) ? (2.0f * area / maxEdge) : 0.0f;
      float distEdge = minBary * alt * scale;
      float edgeK = constrain((edgeWidth + edgeFade - distEdge) / edgeFade, 0.0f, 1.0f);

      // Цвет из вершин (интерполяция по барицентрическим координатам)
      CRGB c0 = model3dVertexColor(i0);
      CRGB c1 = model3dVertexColor(i1);
      CRGB c2 = model3dVertexColor(i2);
      CRGB base((uint8_t)(c0.r * wgt + c1.r * hit.u + c2.r * hit.v),
                (uint8_t)(c0.g * wgt + c1.g * hit.u + c2.g * hit.v),
                (uint8_t)(c0.b * wgt + c1.b * hit.u + c2.b * hit.v));

      // Сдвиг оттенка параметром COLOR
      base = hueShift(base, opt.hue);

      // Затемнение и подсветка ребра
      uint8_t val = (uint8_t)constrain(255.0f * shade, 0.0f, 255.0f);
      CRGB face((uint8_t)(base.r * val / 255),
                (uint8_t)(base.g * val / 255),
                (uint8_t)(base.b * val / 255));
      uint8_t valE = (uint8_t)constrain(255.0f * (shade * 0.45f + 0.55f), 0.0f, 255.0f);
      CRGB edge((uint8_t)constrain(base.r * valE / 255 + 90, 0.0f, 255.0f),
                (uint8_t)constrain(base.g * valE / 255 + 90, 0.0f, 255.0f),
                (uint8_t)constrain(base.b * valE / 255 + 90, 0.0f, 255.0f));

      CRGB out;
      out.r = (uint8_t)constrain(face.r + (edge.r - (int)face.r) * edgeK, 0.0f, 255.0f);
      out.g = (uint8_t)constrain(face.g + (edge.g - (int)face.g) * edgeK, 0.0f, 255.0f);
      out.b = (uint8_t)constrain(face.b + (edge.b - (int)face.b) * edgeK, 0.0f, 255.0f);

      sideSet(x, y, out);
    }
  }
}

// ---------------------------------------------------------------------------
// Основная отрисовка
// ---------------------------------------------------------------------------
void scene3dRender(const SceneOptions& opt, const Quat& bodyToScene, const Vec3& eyeScene) {
  // Модель с SD-карты — отдельный путь
  if (opt.figure == SCENE_FIG_MODEL) {
    renderModel(opt, bodyToScene, eyeScene);
    return;
  }

  const float R      = SCENE_CYL_RADIUS_MM;
  const float H      = SCENE_CYL_HEIGHT_MM;
  const float rowH   = H / (float)MATRIX_HEIGHT;
  const float colAng = 2.0f * PI / (float)MATRIX_WIDTH;

  // Полуразмер объекта
  float S = R * constrain(opt.sizePct, 5.0f, 100.0f) / 100.0f;

  // Фигура
  SceneShape sh;
  bool isSphere = (opt.figure == SCENE_FIG_SPHERE);
  switch (opt.figure) {
    case SCENE_FIG_OCTA:  buildOcta(sh, S);  break;
    case SCENE_FIG_PRISM: buildPrism(sh, S); break;
    default:              buildCube(sh, S);  break;
  }

  const float edgeWidth = opt.edgeMm;
  const float edgeFade  = 1.5f;     // плавный переход к ребру, мм

  for (uint8_t x = 0; x < MATRIX_WIDTH; x++) {
    // Азимут столбца в системе корпуса и его внешняя нормаль
    float ang = (float)SCENE_COL_DIR * (float)x * colAng;
    float sn = sinf(ang), cs = cosf(ang);
    Vec3 nBody(sn, 0.0f, cs);

    for (uint8_t y = 0; y < MATRIX_HEIGHT; y++) {
      // Центр ряда по высоте (ряд 0 — низ матрицы)
      float h = ((float)y + 0.5f) * rowH - H * 0.5f;

      Vec3 pBody(sn * R, h, cs * R);
      Vec3 pScene = bodyToScene.rotate(pBody);
      Vec3 nScene = bodyToScene.rotate(nBody);

      Vec3 toEye = eyeScene - pScene;
      float dist = toEye.length();
      if (dist < 1e-3f) continue;

      // Столбец виден, только если его нормаль смотрит на наблюдателя
      if (nScene.dot(toEye) <= 0.0f) continue;

      Vec3 dir = (toEye * -1.0f) / dist;   // от наблюдателя вглубь сцены

      float t = 0.0f;
      uint8_t face = 0xFF;
      bool hit;
      if (isSphere) {
        hit = raySphere(eyeScene, dir, S, t);
      } else {
        hit = convexHit(sh, eyeScene, dir, t, face);
      }
      if (!hit) continue;

      Vec3 q = eyeScene + dir * t;

      // Нормаль грани в системе сцены
      Vec3 nrm;
      if (isSphere) {
        nrm = q.normalized();
      } else if (face < sh.count) {
        nrm = sh.n[face];
      } else {
        nrm = dir * -1.0f;          // луч начался внутри тела — грубая нормаль
      }

      // Затенение: косинус угла между нормалью и направлением на глаз
      float lambert = -nrm.dot(dir);
      if (lambert < 0.0f) lambert = 0.0f;
      float shade = 0.16f + 0.84f * lambert;

      // Удалённость: дальние точки чуть тусклее (воздушная перспектива)
      float depth = constrain((t - 380.0f) / 420.0f, 0.0f, 1.0f);
      shade *= (1.0f - 0.20f * depth);

      // Подсветка рёбер
      float edgeK = 0.0f;
      if (isSphere) {
        // У сферы подсвечивается контур (силуэт)
        float rim = 1.0f - lambert;
        edgeK = constrain((rim - 0.72f) / 0.26f, 0.0f, 1.0f);
      } else {
        float d = edgeDistance(sh, face, q);
        edgeK = constrain((edgeWidth + edgeFade - d) / edgeFade, 0.0f, 1.0f);
      }

      sideSet(x, y, shadeColor(opt.hue, shade, edgeK));
    }
  }
}
