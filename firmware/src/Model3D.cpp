#include "Model3D.h"
#include "Config.h"
#include "Pins.h"
#include <SD.h>
#include <SPI.h>
#include <esp_heap_caps.h>

// ============================================================================
// 3D-МОДЕЛИ С SD-КАРТЫ — реализация
// ============================================================================

// --- Состояние ---
static bool     g_cardReady = false;
static uint8_t  g_fileCount = 0;
static char     g_fileNames[MODEL_MAX_FILES][MODEL_NAME_LEN];

static uint8_t* g_file = nullptr;      // весь файл в PSRAM
static size_t   g_fileSize = 0;

static uint16_t g_vertCount = 0;
static uint16_t g_triCount = 0;
static uint16_t g_frameCount = 0;
static uint16_t g_animFps = 0;
static uint16_t g_vtxStride = 0;
static uint32_t g_vertexOffset = 0;
static uint32_t g_indexOffset = 0;
static uint32_t g_animOffset = 0;
static float    g_modelRadius = 1.0f;

// Рабочие буферы в PSRAM
static Vec3*    g_pos = nullptr;       // позиции вершин текущего кадра
static CRGB*    g_col = nullptr;       // цвета вершин
static uint32_t* g_triIdx = nullptr;   // индексы треугольников (файл)
static uint32_t* g_triOrder = nullptr; // порядок треугольников в BVH

// --- BVH ---
#define BVH_LEAF_MAX 4

struct BvhNode {
  float   bmin[3];
  float   bmax[3];
  int32_t left;    // левый ребёнок (для внутреннего узла)
  int32_t start;   // первый треугольник (для листа)
  int32_t count;   // число треугольников в листе (0 = внутренний узел)
};

static BvhNode* g_bvh = nullptr;
static int32_t  g_bvhCount = 0;

// Временные буферы для построения BVH
static float*   g_centroid = nullptr;  // центр тяжести треугольников
static int32_t  g_bvhUsed = 0;

// Время последней загрузки (мс)
static uint32_t g_loadMs = 0;

// Индекс загруженной модели (0xFF — ничего не загружено)
static uint8_t  g_loadedIndex = 0xFF;

// Форвард-объявление пересборки дерева (используется при смене кадра)
static void bvhRefresh();

// ---------------------------------------------------------------------------
// Вспомогательное: чтение полей заголовка
// ---------------------------------------------------------------------------
static inline uint16_t rdU16(const uint8_t* p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static inline uint32_t rdU32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline float rdF32(const uint8_t* p) {
  uint32_t v = rdU32(p);
  float f;
  memcpy(&f, &v, 4);
  return f;
}
static inline int16_t rdI16(const uint8_t* p) { return (int16_t)rdU16(p); }

// ---------------------------------------------------------------------------
// Список файлов на карте
// ---------------------------------------------------------------------------
void model3dInit() {
  g_fileCount = 0;
  g_cardReady = false;

  if (!SD.begin(SPI_CS_PIN, SPI, 20000000)) {
    return;                       // карты нет — эффекты останутся на фигурах
  }
  g_cardReady = true;

  // Список файлов в /fle3d/
  File dir = SD.open("/fle3d");
  if (!dir || !dir.isDirectory()) return;

  for (File f = dir.openNextFile(); f && g_fileCount < MODEL_MAX_FILES; f = dir.openNextFile()) {
    if (f.isDirectory()) continue;
    const char* name = f.name();
    size_t len = strlen(name);
    if (len < 5) continue;                          // пустое имя
    if (strcasecmp(name + len - 5, ".fle3d") != 0) continue;

    // Имя без расширения
    size_t n = len - 5;
    if (n >= MODEL_NAME_LEN) n = MODEL_NAME_LEN - 1;
    memcpy(g_fileNames[g_fileCount], name, n);
    g_fileNames[g_fileCount][n] = '\0';
    g_fileCount++;
  }
  dir.close();
}

bool model3dCardReady() { return g_cardReady; }
uint8_t model3dCount() { return g_fileCount; }

const char* model3dName(uint8_t index) {
  if (index >= g_fileCount) return "-";
  return g_fileNames[index];
}

// ---------------------------------------------------------------------------
// Освобождение памяти
// ---------------------------------------------------------------------------
void model3dUnload() {
  if (g_pos)      { heap_caps_free(g_pos);      g_pos = nullptr; }
  if (g_col)      { heap_caps_free(g_col);      g_col = nullptr; }
  if (g_triIdx)   { heap_caps_free(g_triIdx);   g_triIdx = nullptr; }
  if (g_triOrder) { heap_caps_free(g_triOrder); g_triOrder = nullptr; }
  if (g_centroid) { heap_caps_free(g_centroid); g_centroid = nullptr; }
  if (g_bvh)      { heap_caps_free(g_bvh);      g_bvh = nullptr; }
  if (g_file)     { heap_caps_free(g_file);     g_file = nullptr; }

  g_fileSize = 0;
  g_vertCount = 0;
  g_triCount = 0;
  g_frameCount = 0;
  g_bvhCount = 0;
  g_loadedIndex = 0xFF;
}

bool model3dLoaded() { return g_file != nullptr && g_triCount > 0; }
uint8_t model3dLoadedIndex() { return g_loadedIndex; }
uint32_t model3dLoadMillis() { return g_loadMs; }

// ---------------------------------------------------------------------------
// Загрузка модели
// ---------------------------------------------------------------------------
bool model3dLoad(uint8_t index) {
  uint32_t t0 = millis();
  model3dUnload();

  if (!g_cardReady || index >= g_fileCount) {
    g_loadMs = millis() - t0;
    return false;
  }

  char path[64];
  snprintf(path, sizeof(path), "/fle3d/%s.fle3d", g_fileNames[index]);

  File f = SD.open(path, FILE_READ);
  if (!f || f.size() < 64) {
    if (f) f.close();
    g_loadMs = millis() - t0;
    return false;
  }
  size_t size = f.size();
  if (size > MODEL_MAX_FILE_BYTES) {
    f.close();
    g_loadMs = millis() - t0;
    return false;                  // слишком крупная модель — не влезает в PSRAM
  }

  // Файл целиком в PSRAM
  g_file = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!g_file) {
    f.close();
    g_loadMs = millis() - t0;
    return false;
  }
  size_t got = f.read((uint8_t*)g_file, size);
  f.close();
  if (got != size) {
    model3dUnload();
    g_loadMs = millis() - t0;
    return false;
  }
  g_fileSize = size;

  // --- заголовок ---
  if (memcmp(g_file, "FLE3", 4) != 0) { model3dUnload(); g_loadMs = millis() - t0; return false; }
  uint16_t version = rdU16(g_file + 4);
  if (version != 1) { model3dUnload(); g_loadMs = millis() - t0; return false; }

  g_vertCount   = rdU16(g_file + 8);
  g_triCount    = rdU16(g_file + 10);
  g_frameCount  = rdU16(g_file + 12);
  g_animFps     = rdU16(g_file + 14);
  g_vtxStride   = rdU16(g_file + 16);
  g_vertexOffset = rdU32(g_file + 20);
  g_indexOffset  = rdU32(g_file + 24);
  g_animOffset   = rdU32(g_file + 28);
  g_modelRadius  = rdF32(g_file + 36);

  if (g_vertCount == 0 || g_triCount == 0 || g_vtxStride < 17) {
    model3dUnload();
    g_loadMs = millis() - t0;
    return false;
  }
  if (g_vertexOffset + (uint32_t)g_vertCount * g_vtxStride > size ||
      g_indexOffset + (uint32_t)g_triCount * 6 > size) {
    model3dUnload();
    g_loadMs = millis() - t0;
    return false;
  }
  if (g_frameCount > 1 &&
      g_animOffset + (uint32_t)g_frameCount * g_vertCount * 6 > size) {
    g_frameCount = 1;                 // анимация обрезана — работаем без неё
  }

  // --- рабочие буферы ---
  g_pos      = (Vec3*)heap_caps_malloc(sizeof(Vec3) * g_vertCount, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  g_col      = (CRGB*)heap_caps_malloc(sizeof(CRGB) * g_vertCount, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  g_triIdx   = (uint32_t*)heap_caps_malloc(sizeof(uint32_t) * g_triCount, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  g_triOrder = (uint32_t*)heap_caps_malloc(sizeof(uint32_t) * g_triCount, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  g_centroid = (float*)heap_caps_malloc(sizeof(float) * g_triCount * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  g_bvh      = (BvhNode*)heap_caps_malloc(sizeof(BvhNode) * (g_triCount * 2 + 1), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

  if (!g_pos || !g_col || !g_triIdx || !g_triOrder || !g_centroid || !g_bvh) {
    model3dUnload();
    g_loadMs = millis() - t0;
    return false;
  }

  // --- цвета вершин (из базового блока) ---
  for (uint16_t i = 0; i < g_vertCount; i++) {
    const uint8_t* v = g_file + g_vertexOffset + (uint32_t)i * g_vtxStride;
    g_col[i] = CRGB(v[9], v[10], v[11]);
  }

  // --- индексы треугольников ---
  for (uint16_t t = 0; t < g_triCount; t++) {
    const uint8_t* p = g_file + g_indexOffset + (uint32_t)t * 6;
    uint16_t i0 = rdU16(p), i1 = rdU16(p + 2), i2 = rdU16(p + 4);
    if (i0 >= g_vertCount || i1 >= g_vertCount || i2 >= g_vertCount) {
      model3dUnload();
      g_loadMs = millis() - t0;
      return false;
    }
    g_triIdx[t] = ((uint32_t)i0 << 16) | ((uint32_t)i1 << 8) | i2;
    g_triOrder[t] = t;
  }

  // --- первый кадр и BVH ---
  model3dSetTime(0.0f);
  g_loadedIndex = index;
  g_loadMs = millis() - t0;
  return true;
}

// ---------------------------------------------------------------------------
// Кадр анимации
// ---------------------------------------------------------------------------
void model3dSetTime(float seconds) {
  if (!g_file) return;

  if (g_frameCount <= 1) {
    // статичная модель: базовый блок вершин
    for (uint16_t i = 0; i < g_vertCount; i++) {
      const uint8_t* v = g_file + g_vertexOffset + (uint32_t)i * g_vtxStride;
      g_pos[i] = Vec3((float)rdI16(v), (float)rdI16(v + 2), (float)rdI16(v + 4));
    }
    bvhRefresh();
    return;
  }

  float fps = (g_animFps > 0) ? (float)g_animFps : 24.0f;
  float total = (float)g_frameCount / fps;
  float t = seconds;
  t = fmodf(t, total);
  if (t < 0) t += total;

  float fpos = t * fps;
  uint16_t f0 = (uint16_t)fpos;
  if (f0 >= g_frameCount) f0 = g_frameCount - 1;
  uint16_t f1 = (uint16_t)(f0 + 1);
  if (f1 >= g_frameCount) f1 = 0;
  float frac = fpos - (float)f0;

  const uint8_t* b0 = g_file + g_animOffset + (uint32_t)f0 * g_vertCount * 6;
  const uint8_t* b1 = g_file + g_animOffset + (uint32_t)f1 * g_vertCount * 6;

  for (uint16_t i = 0; i < g_vertCount; i++) {
    float x0 = (float)rdI16(b0 + (uint32_t)i * 6);
    float y0 = (float)rdI16(b0 + (uint32_t)i * 6 + 2);
    float z0 = (float)rdI16(b0 + (uint32_t)i * 6 + 4);
    float x1 = (float)rdI16(b1 + (uint32_t)i * 6);
    float y1 = (float)rdI16(b1 + (uint32_t)i * 6 + 2);
    float z1 = (float)rdI16(b1 + (uint32_t)i * 6 + 4);
    g_pos[i] = Vec3(x0 + (x1 - x0) * frac,
                    y0 + (y1 - y0) * frac,
                    z0 + (z1 - z0) * frac);
  }

  bvhRefresh();
}

// ---------------------------------------------------------------------------
// Доступ к геометрии
// ---------------------------------------------------------------------------
uint16_t model3dVertCount() { return g_vertCount; }
uint16_t model3dTriCount() { return g_triCount; }
float    model3dRadius() { return g_modelRadius; }

Vec3 model3dVertex(uint16_t i) {
  if (i >= g_vertCount) return Vec3();
  return g_pos[i];
}

CRGB model3dVertexColor(uint16_t i) {
  if (i >= g_vertCount) return CRGB(255, 255, 255);
  return g_col[i];
}

void model3dTriangle(uint16_t t, uint16_t& i0, uint16_t& i1, uint16_t& i2) {
  if (t >= g_triCount) { i0 = i1 = i2 = 0; return; }
  uint32_t v = g_triIdx[t];
  i0 = (uint16_t)(v >> 16);
  i1 = (uint16_t)((v >> 8) & 0xFF);
  i2 = (uint16_t)(v & 0xFF);
}

// ---------------------------------------------------------------------------
// Построение BVH (пересобирается на каждый кадр — модель движется)
// ---------------------------------------------------------------------------

// Сортировка подмножества треугольников по оси методом Хоара
static void sortRange(int32_t first, int32_t last, uint8_t axis) {
  while (first < last) {
    float pivot = g_centroid[(size_t)g_triOrder[(first + last) / 2] * 3 + axis];
    int32_t i = first, j = last;
    while (i <= j) {
      while (g_centroid[(size_t)g_triOrder[i] * 3 + axis] < pivot) i++;
      while (g_centroid[(size_t)g_triOrder[j] * 3 + axis] > pivot) j--;
      if (i <= j) {
        uint32_t tmp = g_triOrder[i];
        g_triOrder[i] = g_triOrder[j];
        g_triOrder[j] = tmp;
        i++;
        j--;
      }
    }
    // рекурсию делаем только на меньшую половину
    if (j - first < last - i) {
      sortRange(first, j, axis);
      first = i;
    } else {
      sortRange(i, last, axis);
      last = j;
    }
  }
}

// Возвращает номер созданного узла
static int32_t bvhBuild(int32_t first, int32_t count) {
  int32_t ni = g_bvhUsed++;
  BvhNode& node = g_bvh[ni];

  node.bmin[0] = node.bmin[1] = node.bmin[2] = 1e30f;
  node.bmax[0] = node.bmax[1] = node.bmax[2] = -1e30f;

  for (int32_t i = first; i < first + count; i++) {
    uint16_t i0, i1, i2;
    model3dTriangle((uint16_t)g_triOrder[i], i0, i1, i2);
    const Vec3* vv[3] = { &g_pos[i0], &g_pos[i1], &g_pos[i2] };
    for (int k = 0; k < 3; k++) {
      for (int ax = 0; ax < 3; ax++) {
        float v = vv[k]->at((uint8_t)ax);
        if (v < node.bmin[ax]) node.bmin[ax] = v;
        if (v > node.bmax[ax]) node.bmax[ax] = v;
      }
    }
    const Vec3& a = g_pos[i0];
    const Vec3& b = g_pos[i1];
    const Vec3& c = g_pos[i2];
    g_centroid[(size_t)i * 3 + 0] = (a.x + b.x + c.x) / 3.0f;
    g_centroid[(size_t)i * 3 + 1] = (a.y + b.y + c.y) / 3.0f;
    g_centroid[(size_t)i * 3 + 2] = (a.z + b.z + c.z) / 3.0f;
  }

  if (count <= BVH_LEAF_MAX) {
    node.left = 0;
    node.start = first;
    node.count = count;
    return ni;
  }

  // Ось разбиения — самая длинная
  uint8_t axis = 0;
  float ext = node.bmax[0] - node.bmin[0];
  float e1 = node.bmax[1] - node.bmin[1];
  float e2 = node.bmax[2] - node.bmin[2];
  if (e1 > ext) { ext = e1; axis = 1; }
  if (e2 > ext) { ext = e2; axis = 2; }

  sortRange(first, first + count - 1, axis);

  int32_t mid = count / 2;
  int32_t left = bvhBuild(first, mid);
  int32_t right = bvhBuild(first + mid, count - mid);
  (void)right;

  node.left = left;
  node.start = 0;
  node.count = 0;
  return ni;
}

// Пересобрать BVH (после смены кадра)
static void bvhRefresh() {
  if (!g_bvh || g_triCount == 0) return;
  g_bvhUsed = 0;
  bvhBuild(0, g_triCount);
  g_bvhCount = g_bvhUsed;
}

// ---------------------------------------------------------------------------
// Пересечение луча с моделью
// ---------------------------------------------------------------------------
static inline bool aabbHit(const BvhNode& n, const Vec3& o, const Vec3& inv) {
  float t0 = (n.bmin[0] - o.x) * inv.x;
  float t1 = (n.bmax[0] - o.x) * inv.x;
  if (t0 > t1) { float tmp = t0; t0 = t1; t1 = tmp; }
  float ty0 = (n.bmin[1] - o.y) * inv.y;
  float ty1 = (n.bmax[1] - o.y) * inv.y;
  if (ty0 > ty1) { float tmp = ty0; ty0 = ty1; ty1 = tmp; }
  if (t0 > ty1 || ty0 > t1) return false;
  if (ty0 > t0) t0 = ty0;
  if (ty1 < t1) t1 = ty1;
  float tz0 = (n.bmin[2] - o.z) * inv.z;
  float tz1 = (n.bmax[2] - o.z) * inv.z;
  if (tz0 > tz1) { float tmp = tz0; tz0 = tz1; tz1 = tmp; }
  if (t0 > tz1 || tz0 > t1) return false;
  return true;
}

ModelHit model3dCast(const Vec3& origin, const Vec3& dir) {
  ModelHit best;
  if (!model3dLoaded() || g_bvhCount == 0) return best;

  Vec3 inv(1.0f / dir.x, 1.0f / dir.y, 1.0f / dir.z);

  int32_t stack[48];
  int32_t sp = 0;
  stack[sp++] = 0;

  while (sp > 0) {
    int32_t ni = stack[--sp];
    const BvhNode& n = g_bvh[ni];
    if (!aabbHit(n, origin, inv)) continue;

    if (n.count > 0) {
      for (int32_t i = n.start; i < n.start + n.count; i++) {
        uint16_t i0, i1, i2;
        model3dTriangle((uint16_t)g_triOrder[i], i0, i1, i2);
        const Vec3& a = g_pos[i0];
        const Vec3& b = g_pos[i1];
        const Vec3& c = g_pos[i2];

        Vec3 e1 = b - a;
        Vec3 e2 = c - a;
        Vec3 p = dir.cross(e2);
        float det = e1.dot(p);
        if (fabsf(det) < 1e-12f) continue;
        float invDet = 1.0f / det;
        Vec3 tv = origin - a;
        float u = tv.dot(p) * invDet;
        if (u < 0.0f || u > 1.0f) continue;
        Vec3 q = tv.cross(e1);
        float v = dir.dot(q) * invDet;
        if (v < 0.0f || u + v > 1.0f) continue;
        float t = e2.dot(q) * invDet;
        if (t <= 0.0f) continue;

        if (!best.hit || t < best.t) {
          best.hit = true;
          best.t = t;
          best.tri = (uint16_t)g_triOrder[i];
          best.u = u;
          best.v = v;
        }
      }
    } else {
      if (sp + 2 <= 48) {
        stack[sp++] = n.left;
        stack[sp++] = n.left + 1;
      }
    }
  }

  return best;
}
