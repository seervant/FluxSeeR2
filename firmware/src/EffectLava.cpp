#include "Effects.h"
#include "Config.h"
#include "LedGeometry.h"
#include "Params.h"
#include "Sensors.h"
#include <math.h>

// ============================================================================
// ЭФФЕКТ 7 — ЛАВОВАЯ ЛАМПА
//
// Физическая модель, а не анимация. Каждый пузырёк — сгусток «лавы»
// произвольной формы: он собран из нескольких каплевидных узлов (объединение
// кругов), поэтому контур получается неровным, вытянутым и асимметричным.
//
// Что происходит с каждым сгустком:
//   - НАГРЕВ/ОСТЫВАНИЕ: снизу лампа греет, сверху остывает. Температура
//     стремится к равновесной для своей высоты;
//   - ПЛАВУЧЕСТЬ: горячий сгусток легче среды и всплывает, остывший — тонет.
//     Получается естественная конвекция: подъём, остывание, спуск, нагрев;
//   - ДЕФОРМАЦИЯ: форма тянется по направлению движения (инерция) и
//     возвращается к базовой пружиной (упругость), затухание — вязкость;
//   - ВЗАИМОДЕЙСТВИЕ: при перекрытии — мягкое давление с демпфированием
//     относительной скорости (лаву нельзя сжать), на расстоянии — притяжение
//     (поверхностное натяжение);
//   - СЛИЯНИЕ (редко): два сгустка, глубоко перекрывшиеся и медленные,
//     объединяются; площадь, масса, температура и скорость сохраняются;
//   - ДЕЛЕНИЕ (очень редко): сильно растянутый сгусток распадается на два.
//
// Площадь: измеренная после растеризации площадь сравнивается с целевой, и
// масштаб формы плавно подстраивается — «лава несжимаема». Суммарная
// площадь держится на заданной доле матрицы (параметр COVER, по умолчанию
// 50 %), поэтому пузырьки никогда не заливают всю поверхность.
//
// Оси: y = 0 — нижний ряд матрицы, y = 15 — верхний.
// Верхний диск — вид сверху: тот же сгусток проецируется в полярные
// координаты диска (высота -> радиус, столбец -> угол), поэтому масса
// выглядит единой на боковой и верхней поверхностях.
// ============================================================================

// ---------------------------------------------------------------------------
// Состояние эффекта (переживает переключение эффектов)
// ---------------------------------------------------------------------------

// Каплевидный узел сгустка
struct LavaLobe {
  float ox, oy;   // текущее смещение относительно центра сгустка
  float r;        // текущий радиус узла
  float rx, ry;   // базовая (покоящаяся) форма — к ней тянет упругость
  float rr;       // базовый радиус
  float vx, vy;   // скорость деформации
  float vr;
};

struct LavaBlob {
  float x, y;         // центр в координатах матрицы
  float vx, vy;       // скорость
  float area;         // целевая площадь, пиксели
  float areaScale;    // текущий масштаб формы (поддержание площади)
  float weight;       // вес в распределении общей площади
  float temp;         // температура 0..1
  float heatBias;     // индивидуальный сдвиг по нагреву
  float visc;         // индивидуальная вязкость
  float elast;        // индивидуальная упругость
  float buoy;         // индивидуальная плотность
  float rot, vrot;    // поворот сгустка
  uint8_t lobeCount;
  LavaLobe lobe[LAVA_MAX_LOBES];
  int8_t  hue;        // индивидуальный сдвиг оттенка (может быть отрицательным)
  bool alive;
};

static LavaBlob g_lava[LAVA_MAX_BLOBS];
static uint8_t  g_lavaCount = 0;
static uint32_t g_lavaLastMs = 0;

// Последний разброс размеров, при котором менялись веса сгустков
static int16_t g_lavaLastSpread = -1;

// ---------------------------------------------------------------------------
// Индексы параметров эффекта
// ---------------------------------------------------------------------------
static const uint8_t P_BRIGHT = 0;   // яркость (применяется централизованно)
static const uint8_t P_COVER  = 1;   // покрытие матрицы лавой, %
static const uint8_t P_COUNT  = 2;   // число пузырьков
static const uint8_t P_SPREAD = 3;   // разброс размеров
static const uint8_t P_VISC   = 4;   // вязкость среды
static const uint8_t P_ELAST  = 5;   // упругость сгустков
static const uint8_t P_HEAT   = 6;   // скорость нагрева/остывания
static const uint8_t P_MERGE  = 7;   // частота слияний, %
static const uint8_t P_SPLIT  = 8;   // частота делений, %
static const uint8_t P_SPEED  = 9;   // общая скорость движения
static const uint8_t P_COLOR  = 10;  // базовый оттенок лавы

// ---------------------------------------------------------------------------
// Вспомогательное
// ---------------------------------------------------------------------------

// Разница координат по окружности цилиндра (48 столбцов замкнуты в кольцо)
static inline float lavaDx(float dx) {
  while (dx > (float)MATRIX_WIDTH * 0.5f) dx -= (float)MATRIX_WIDTH;
  while (dx < -(float)MATRIX_WIDTH * 0.5f) dx += (float)MATRIX_WIDTH;
  return dx;
}

// Эквивалентный радиус сгустка (по его площади)
static inline float lavaRadius(const LavaBlob& b) {
  return sqrtf(b.area / PI) * b.areaScale;
}

// Узлы сгустка в мировых координатах
struct LavaWorldLobe {
  float x, y, r;
};

static void lavaWorldLobes(const LavaBlob& b, LavaWorldLobe* out) {
  float cs = cosf(b.rot) * b.areaScale;
  float sn = sinf(b.rot) * b.areaScale;
  for (uint8_t i = 0; i < b.lobeCount; i++) {
    out[i].x = b.x + b.lobe[i].ox * cs - b.lobe[i].oy * sn;
    out[i].y = b.y + b.lobe[i].ox * sn + b.lobe[i].oy * cs;
    out[i].r = b.lobe[i].r * b.areaScale;
  }
}

// ---------------------------------------------------------------------------
// Создание сгустка
// ---------------------------------------------------------------------------
static void lavaSpawnBlob(uint8_t index, float cx, float cy, float area, int8_t hue) {
  LavaBlob& b = g_lava[index];

  b.x = cx;
  b.y = cy;
  b.vx = 0.0f;
  b.vy = 0.0f;
  b.area = area;
  b.areaScale = 0.45f;          // вырастает до нужного размера плавно
  b.temp = 0.5f + (float)random(-25, 25) / 100.0f;
  b.heatBias = (float)random(-15, 15) / 100.0f;
  b.visc = 0.75f + (float)random(0, 50) / 100.0f;
  b.elast = 0.70f + (float)random(0, 60) / 100.0f;
  b.buoy = 0.85f + (float)random(0, 30) / 100.0f;
  b.rot = (float)random(0, 628) / 100.0f;
  b.vrot = (float)random(-30, 30) / 100.0f;
  b.hue = hue;
  // вес в распределении общей площади (задаётся сразу, чтобы новый сгусток
  // сразу получил свою долю, а не появился нулевым)
  b.weight = 0.35f + (float)random(0, 100) / 100.0f;
  b.alive = true;

  // Форма: объединение капель. Число узлов и их размеры случайны — отсюда
  // неровный, вытянутый, асимметричный контур без «идеальных кругов».
  float r = sqrtf(area / PI);
  b.lobeCount = (uint8_t)(5 + random(0, LAVA_MAX_LOBES - 4));
  for (uint8_t i = 0; i < b.lobeCount; i++) {
    float ang = (float)i * (2.0f * PI / (float)b.lobeCount) +
                (float)random(-35, 35) / 100.0f;
    float rr = r * (0.25f + (float)random(0, 70) / 100.0f);
    b.lobe[i].ox = cosf(ang) * rr;
    b.lobe[i].oy = sinf(ang) * rr;
    b.lobe[i].r = r * (0.40f + (float)random(0, 45) / 100.0f);
    b.lobe[i].rx = b.lobe[i].ox;
    b.lobe[i].ry = b.lobe[i].oy;
    b.lobe[i].rr = b.lobe[i].r;
    b.lobe[i].vx = 0.0f;
    b.lobe[i].vy = 0.0f;
    b.lobe[i].vr = 0.0f;
  }
}

// Случайные веса сгустков: часть крупнее, часть мельче (степенной закон)
static void lavaMakeWeights(uint8_t count, float spread) {
  for (uint8_t i = 0; i < count; i++) {
    float u = (float)random(0, 1000) / 1000.0f;
    g_lava[i].weight = 0.35f + spread * 0.065f * (u * u * u);
  }
}

// ---------------------------------------------------------------------------
// Слияние двух сгустков (редкое событие)
//
// Площадь, масса, температура и скорость сохраняются; контур нового сгустка
// собирается из узлов обоих — поэтому он остаётся неровным.
// ---------------------------------------------------------------------------
static void lavaMerge(uint8_t ia, uint8_t ib) {
  LavaBlob& a = g_lava[ia];
  LavaBlob& b = g_lava[ib];

  float ra = lavaRadius(a);
  float rb = lavaRadius(b);
  float dx = lavaDx(b.x - a.x);
  float dy = b.y - a.y;
  float d = sqrtf(dx * dx + dy * dy);

  // перекрытие (круг радиуса (ra+rb-d)/2)
  float ov = 0.5f * (ra + rb - d);
  if (ov < 0.0f) ov = 0.0f;

  float newArea = a.area + b.area - PI * ov * ov;
  if (newArea < 6.0f) newArea = 6.0f;

  float wa = a.area, wb = b.area, wt = wa + wb;
  // центр по массе (в развёрнутых координатах, без замыкания кольца)
  float cx = a.x + dx * (wb / wt);
  float cy = a.y + dy * (wb / wt);

  float nvx = (a.vx * wa + b.vx * wb) / wt;
  float nvy = (a.vy * wa + b.vy * wb) / wt;
  float ntemp = (a.temp * wa + b.temp * wb) / wt;

  // смещения центров относительно нового
  float offAX = -dx * (wb / wt), offAY = -dy * (wb / wt);
  float offBX =  dx * (wa / wt), offBY =  dy * (wa / wt);

  bool keepA = (wa >= wb);
  LavaBlob& keep = keepA ? a : b;
  LavaBlob& drop = keepA ? b : a;
  float offKX = keepA ? offAX : offBX;
  float offKY = keepA ? offAY : offBY;
  float offDX = keepA ? offBX : offAX;
  float offDY = keepA ? offBY : offAY;

  // масштаб узлов под новую площадь
  float k = sqrtf(newArea / keep.area);

  LavaLobe tmp[LAVA_MAX_LOBES];
  uint8_t n = 0;
  for (uint8_t i = 0; i < keep.lobeCount && n < LAVA_MAX_LOBES; i++) {
    tmp[n].ox = keep.lobe[i].ox * k + offKX;
    tmp[n].oy = keep.lobe[i].oy * k + offKY;
    tmp[n].r  = keep.lobe[i].r * k;
    n++;
  }
  // два узла от меньшего сгустка — контур остаётся неровным
  for (uint8_t i = 0; i < 2 && i < drop.lobeCount && n < LAVA_MAX_LOBES; i++) {
    uint8_t idx = (uint8_t)((uint16_t)i * drop.lobeCount / 2);
    tmp[n].ox = drop.lobe[idx].ox * k + offDX;
    tmp[n].oy = drop.lobe[idx].oy * k + offDY;
    tmp[n].r  = drop.lobe[idx].r * k;
    n++;
  }

  // переносим результат в сгусток a
  for (uint8_t i = 0; i < n; i++) {
    a.lobe[i] = tmp[i];
    a.lobe[i].rx = a.lobe[i].ox;
    a.lobe[i].ry = a.lobe[i].oy;
    a.lobe[i].rr = a.lobe[i].r;
    a.lobe[i].vx = 0.0f;
    a.lobe[i].vy = 0.0f;
    a.lobe[i].vr = 0.0f;
  }
  a.lobeCount = n;
  a.x = cx;
  a.y = cy;
  a.vx = nvx;
  a.vy = nvy;
  a.temp = ntemp;
  a.area = newArea;
  a.areaScale = 1.0f;

  b.alive = false;
}

// ---------------------------------------------------------------------------
// Деление сгустка (очень редкое событие)
//
// Возможно только когда сгусток заметно растянут: часть узлов ушла от
// базовой формы. Половина узлов уходит в новый сгусток, площадь делится.
// ---------------------------------------------------------------------------
static void lavaSplit(uint8_t ia, uint8_t ib) {
  LavaBlob& a = g_lava[ia];
  LavaBlob& b = g_lava[ib];

  // ось разделения — направление наибольшего растяжения
  float best = 0.0f, ax = 1.0f, ay = 0.0f;
  for (uint8_t i = 0; i < a.lobeCount; i++) {
    float ddx = a.lobe[i].ox - a.lobe[i].rx;
    float ddy = a.lobe[i].oy - a.lobe[i].ry;
    float len = sqrtf(ddx * ddx + ddy * ddy);
    if (len > best) { best = len; ax = ddx / len; ay = ddy / len; }
  }
  if (best < LAVA_SPLIT_STRETCH) return;   // не растянут — не делим

  // делим узлы по знаку проекции на ось
  LavaLobe la[LAVA_MAX_LOBES], lb[LAVA_MAX_LOBES];
  uint8_t na = 0, nb = 0;
  for (uint8_t i = 0; i < a.lobeCount; i++) {
    float proj = a.lobe[i].ox * ax + a.lobe[i].oy * ay;
    if (proj >= 0.0f) {
      if (na < LAVA_MAX_LOBES) la[na++] = a.lobe[i];
    } else {
      if (nb < LAVA_MAX_LOBES) lb[nb++] = a.lobe[i];
    }
  }
  if (na == 0 || nb == 0) return;            // делить нечего

  float f1 = 0.45f + (float)random(0, 10) / 100.0f;
  float areaA = a.area * f1;
  float areaB = a.area - areaA;
  if (areaA < 4.0f || areaB < 4.0f) return;

  // раздвигаем половинки вдоль оси
  float sep = sqrtf(areaA / PI) * 0.8f;
  float ca = cosf(a.rot), sa = sinf(a.rot);
  float axisX = ax * ca - ay * sa;
  float axisY = ax * sa + ay * ca;

  b.x = a.x + axisX * sep;
  b.y = a.y + axisY * sep;
  b.vx = a.vx + axisX * 0.25f;
  b.vy = a.vy + axisY * 0.25f;
  b.area = areaB;
  b.areaScale = a.areaScale;
  b.temp = a.temp;
  b.heatBias = a.heatBias;
  b.visc = a.visc;
  b.elast = a.elast;
  b.buoy = a.buoy;
  b.rot = a.rot;
  b.vrot = a.vrot;
  b.hue = a.hue;
  // вес делится вместе с площадью, чтобы суммарная площадь не изменилась
  b.weight = a.weight * 0.5f;
  b.alive = true;
  b.lobeCount = nb;
  for (uint8_t i = 0; i < nb; i++) {
    b.lobe[i] = lb[i];
    b.lobe[i].ox -= axisX * sep;
    b.lobe[i].oy -= axisY * sep;
    b.lobe[i].rx = b.lobe[i].ox;
    b.lobe[i].ry = b.lobe[i].oy;
    b.lobe[i].rr = b.lobe[i].r;
    b.lobe[i].vx = 0.0f;
    b.lobe[i].vy = 0.0f;
    b.lobe[i].vr = 0.0f;
  }

  a.lobeCount = na;
  for (uint8_t i = 0; i < na; i++) {
    a.lobe[i] = la[i];
    a.lobe[i].rx = a.lobe[i].ox;
    a.lobe[i].ry = a.lobe[i].oy;
    a.lobe[i].rr = a.lobe[i].r;
    a.lobe[i].vx = 0.0f;
    a.lobe[i].vy = 0.0f;
    a.lobe[i].vr = 0.0f;
  }
  a.area = areaA;
}

// ---------------------------------------------------------------------------
// Раскладка: число сгустков и распределение общей площади
// ---------------------------------------------------------------------------
static void lavaLayout(uint8_t count, float totalArea, int16_t spread) {
  // добавление новых сгустков
  for (uint8_t i = g_lavaCount; i < count; i++) {
    float x = (float)random(0, MATRIX_WIDTH - 1) + 0.5f;
    float y = 1.0f + (float)random(0, MATRIX_HEIGHT - 3);
    lavaSpawnBlob(i, x, y, totalArea / (float)count, (int8_t)random(-12, 12));
  }
  // лишние — гасим
  for (uint8_t i = count; i < g_lavaCount; i++) g_lava[i].alive = false;

  if (count == 0) { g_lavaCount = 0; return; }
  g_lavaCount = count;

  // веса — только если изменился разброс или состав
  if (spread != g_lavaLastSpread) {
    lavaMakeWeights(count, (float)spread);
    g_lavaLastSpread = spread;
  }

  // площади: общая доля матрицы делится между сгустками по весам
  float wsum = 0.0f;
  for (uint8_t i = 0; i < count; i++) wsum += g_lava[i].weight;
  if (wsum < 0.001f) wsum = 1.0f;
  for (uint8_t i = 0; i < count; i++) {
    g_lava[i].area = totalArea * g_lava[i].weight / wsum;
  }
}

// ---------------------------------------------------------------------------
// Физика
// ---------------------------------------------------------------------------
static void lavaPhysics(float dt, float heatRate, float viscBase, float elastBase,
                        float buoyBase, float speedBase, float mergeChance,
                        float splitChance) {
  const float H = (float)(MATRIX_HEIGHT - 1);
  const float chargingBoost = data.isCharging ? 1.6f : 1.0f;

  // --- 1. Температура, плавучесть, вязкое сопротивление ---
  for (uint8_t i = 0; i < g_lavaCount; i++) {
    LavaBlob& b = g_lava[i];
    if (!b.alive) continue;

    // равновесная температура: снизу горячо, сверху холодно
    float up = b.y / H;
    float target = 0.5f + 0.42f * (0.5f - up) + b.heatBias;
    b.temp += (target - b.temp) * heatRate * chargingBoost * dt;
    // микро-возмущения среды
    b.temp += (float)random(-20, 20) / 1000.0f * dt * 6.0f;
    if (b.temp < 0.0f) b.temp = 0.0f;
    if (b.temp > 1.0f) b.temp = 1.0f;

    // плавучесть: горячее -> вверх (ось Y растёт вверх)
    float buoy = (b.temp - 0.5f) * 2.0f;
    b.vy += buoy * buoyBase * b.buoy * speedBase * dt;

    // вязкое сопротивление среды
    float drag = 1.0f - (viscBase * b.visc) * dt;
    if (drag < 0.0f) drag = 0.0f;
    b.vx *= drag;
    b.vy *= drag;

    // поворот сгустка
    b.vrot += ((float)random(-20, 20) / 100.0f) * dt;
    b.vrot *= (1.0f - 1.5f * dt);
    b.rot += b.vrot * dt;
  }

  // --- 2. Взаимодействие соседей: давление и притяжение ---
  for (uint8_t i = 0; i < g_lavaCount; i++) {
    LavaBlob& a = g_lava[i];
    if (!a.alive) continue;
    float ri = lavaRadius(a);

    for (uint8_t j = (uint8_t)(i + 1); j < g_lavaCount; j++) {
      LavaBlob& o = g_lava[j];
      if (!o.alive) continue;

      float dx = lavaDx(o.x - a.x);
      float dy = o.y - a.y;
      float d2 = dx * dx + dy * dy;
      float rj = lavaRadius(o);
      float sum = ri + rj;
      float reach = sum * LAVA_COHERE_RANGE;
      if (d2 > reach * reach) continue;

      float d = sqrtf(d2);
      if (d < 0.0001f) d = 0.0001f;
      float nx = dx / d, ny = dy / d;

      float f;
      if (d < sum) {
        // мягкое давление: чем глубже перекрытие, тем сильнее выталкивание
        float overlap = sum - d;
        f = LAVA_PRESSURE * overlap * (overlap / sum);
        // демпфирование относительной скорости вдоль нормали — лаву не сжать
        float rel = (a.vx - o.vx) * nx + (a.vy - o.vy) * ny;
        f += rel * overlap * 5.0f;
      } else {
        // притяжение (поверхностное натяжение) на расстоянии
        float t = 1.0f - (d - sum) / (sum * (LAVA_COHERE_RANGE - 1.0f));
        if (t < 0.0f) t = 0.0f;
        f = -LAVA_COHESION * t * (ri < rj ? ri : rj);
      }

      // масса пропорциональна площади: тяжёлый сгусток смещается меньше
      float ma = a.area / 60.0f;
      float mo = o.area / 60.0f;
      if (ma < 0.05f) ma = 0.05f;
      if (mo < 0.05f) mo = 0.05f;

      a.vx += (f * nx / ma) * dt;
      a.vy += (f * ny / ma) * dt;
      o.vx -= (f * nx / mo) * dt;
      o.vy -= (f * ny / mo) * dt;
    }
  }

  // --- 3. Перемещение и границы ---
  for (uint8_t i = 0; i < g_lavaCount; i++) {
    LavaBlob& b = g_lava[i];
    if (!b.alive) continue;

    b.x += b.vx * dt;
    b.y += b.vy * dt;

    // цилиндр замкнут по X
    while (b.x < 0.0f) b.x += (float)MATRIX_WIDTH;
    while (b.x >= (float)MATRIX_WIDTH) b.x -= (float)MATRIX_WIDTH;

    // дно и верх: упругий отскок
    float r = lavaRadius(b);
    if (b.y < r * 0.35f) { b.y = r * 0.35f; b.vy = -b.vy * 0.35f; }
    if (b.y > H - r * 0.35f) { b.y = H - r * 0.35f; b.vy = -b.vy * 0.35f; }
  }

  // --- 4. Деформация формы: инерция + упругость + вязкость ---
  for (uint8_t i = 0; i < g_lavaCount; i++) {
    LavaBlob& b = g_lava[i];
    if (!b.alive) continue;

    float inertia = 1.2f + b.visc * 0.8f;
    float k = b.elast * elastBase;
    float damp = 1.0f - min(0.85f, b.visc * viscBase * 0.10f * dt);

    for (uint8_t l = 0; l < b.lobeCount; l++) {
      LavaLobe& L = b.lobe[l];

      // форма отстаёт от движения (инерция) и возвращается пружиной
      L.vx += (-b.vx * inertia + (L.rx - L.ox) * k) * dt;
      L.vy += (-b.vy * inertia + (L.ry - L.oy) * k) * dt;

      // шевеление среды
      L.vx += (float)random(-30, 30) / 100.0f * dt * 3.0f;
      L.vy += (float)random(-30, 30) / 100.0f * dt * 3.0f;

      L.vx *= damp;
      L.vy *= damp;
      L.ox += L.vx * dt;
      L.oy += L.vy * dt;

      // радиус капли тоже слегка «дышит»
      L.vr += ((L.rr - L.r) * k * 0.6f +
               (float)random(-20, 20) / 100.0f * dt) * dt;
      L.vr *= damp;
      L.r += L.vr * dt;
      if (L.r < 0.4f) L.r = 0.4f;
    }
  }

  // --- 5. Редкие события: слияние и деление ---
  for (uint8_t i = 0; i < g_lavaCount; i++) {
    LavaBlob& a = g_lava[i];
    if (!a.alive) continue;

    for (uint8_t j = (uint8_t)(i + 1); j < g_lavaCount; j++) {
      LavaBlob& o = g_lava[j];
      if (!o.alive) continue;

      float dx = lavaDx(o.x - a.x);
      float dy = o.y - a.y;
      float d = sqrtf(dx * dx + dy * dy);
      float sum = lavaRadius(a) + lavaRadius(o);

      // --- слияние: глубокое перекрытие + низкая относительная скорость ---
      if (d < sum * LAVA_MERGE_DEPTH) {
        float rel = sqrtf((a.vx - o.vx) * (a.vx - o.vx) +
                          (a.vy - o.vy) * (a.vy - o.vy));
        if (rel < 1.2f && random(0, 1000) < (int)(mergeChance * 1000.0f)) {
          lavaMerge(i, j);
          break;
        }
      }

      // --- деление: сгусток растянут другим сгустком ---
      if (splitChance > 0.0f && d < sum * 1.15f) {
        if (random(0, 1000) < (int)(splitChance * 1000.0f)) {
          uint8_t free = 0xFF;
          for (uint8_t k = 0; k < g_lavaCount; k++) {
            if (!g_lava[k].alive) { free = k; break; }
          }
          if (free != 0xFF) {
            lavaSplit(i, free);
            break;
          }
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Отрисовка сгустка на боковой матрице
//
// Возвращает число закрашенных пикселей — по нему поддерживается площадь.
// ---------------------------------------------------------------------------
static uint16_t lavaRenderSide(LavaBlob& b, uint8_t baseHue) {
  LavaWorldLobe wl[LAVA_MAX_LOBES];
  lavaWorldLobes(b, wl);

  float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
  for (uint8_t i = 0; i < b.lobeCount; i++) {
    minX = min(minX, wl[i].x - wl[i].r);
    maxX = max(maxX, wl[i].x + wl[i].r);
    minY = min(minY, wl[i].y - wl[i].r);
    maxY = max(maxY, wl[i].y + wl[i].r);
  }

  int x0 = (int)floorf(minX), x1 = (int)ceilf(maxX);
  int y0 = (int)floorf(minY), y1 = (int)ceilf(maxY);
  if (y1 < 0 || y0 > MATRIX_HEIGHT - 1) return 0;
  if (y0 < 0) y0 = 0;
  if (y1 > MATRIX_HEIGHT - 1) y1 = MATRIX_HEIGHT - 1;

  uint16_t painted = 0;

  for (int py = y0; py <= y1; py++) {
    for (int px = x0; px <= x1; px++) {
      // расстояние до ближайшей капли
      float dmin = 1e9f;
      for (uint8_t i = 0; i < b.lobeCount; i++) {
        float ddx = (float)px - wl[i].x;
        float ddy = (float)py - wl[i].y;
        float d = sqrtf(ddx * ddx + ddy * ddy) - wl[i].r;
        if (d < dmin) dmin = d;
      }
      if (dmin > 0.9f) continue;

      // глубина внутрь: 0 у кромки, 1 в глубине
      float t = constrain(-dmin / 2.2f, 0.0f, 1.0f);
      // кромка — более красная, глубина — желтее и ярче
      uint8_t hue = (uint8_t)(baseHue + b.hue + (uint8_t)(20.0f * (1.0f - t)));
      uint8_t val = (uint8_t)(70.0f + 185.0f * t);
      CRGB c = CHSV(hue, 255, val);

      if (dmin > 0.0f) {
        // мягкий ореол снаружи капли
        float glow = 1.0f - dmin / 0.9f;
        c = CHSV(hue, 255, (uint8_t)(val * glow * 0.40f));
      }

      int wx = px % MATRIX_WIDTH;
      if (wx < 0) wx += MATRIX_WIDTH;
      sideAdd((uint8_t)wx, (uint8_t)py, c);

      if (dmin <= 0.0f) painted++;
    }
  }

  return painted;
}

// ---------------------------------------------------------------------------
// Отрисовка сгустка на верхнем диске (вид сверху)
//
// Высота сгустка -> радиус на диске, столбец -> угол. Масштаб меньше, чем на
// цилиндре: диск физически меньше по площади.
// ---------------------------------------------------------------------------
static void lavaRenderDisk(LavaBlob& b, uint8_t baseHue) {
  float ang = (b.x / (float)MATRIX_WIDTH) * 2.0f * PI;
  float rad = 1.0f - (b.y / (float)(MATRIX_HEIGHT - 1));
  float cx = cosf(ang) * rad;
  float cy = sinf(ang) * rad;

  float sc = LAVA_DISK_SCALE * b.areaScale;
  float cs = cosf(b.rot) * sc;
  float sn = sinf(b.rot) * sc;

  LavaWorldLobe wl[LAVA_MAX_LOBES];
  for (uint8_t i = 0; i < b.lobeCount; i++) {
    wl[i].x = cx + b.lobe[i].ox * cs - b.lobe[i].oy * sn;
    wl[i].y = cy + b.lobe[i].ox * sn + b.lobe[i].oy * cs;
    wl[i].r = b.lobe[i].r * sc;
  }

  float maxR = 0.0f;
  for (uint8_t i = 0; i < b.lobeCount; i++) maxR = max(maxR, wl[i].r);
  float bbx = maxR + 0.2f;

  for (uint8_t ring = 0; ring < DISK_RING_COUNT; ring++) {
    uint8_t n = diskRingSize(ring);
    float rr = diskRingRadius(ring);

    for (uint8_t seg = 0; seg < n; seg++) {
      float a = (float)seg * (2.0f * PI / (float)n);
      float px = rr * cosf(a);
      float py = rr * sinf(a);
      if (fabsf(px - cx) > bbx || fabsf(py - cy) > bbx) continue;

      float dmin = 1e9f;
      for (uint8_t i = 0; i < b.lobeCount; i++) {
        float ddx = px - wl[i].x;
        float ddy = py - wl[i].y;
        float d = sqrtf(ddx * ddx + ddy * ddy) - wl[i].r;
        if (d < dmin) dmin = d;
      }
      if (dmin > 0.0f) continue;

      float t = constrain(-dmin / 1.4f, 0.0f, 1.0f);
      uint8_t hue = (uint8_t)(baseHue + b.hue + (uint8_t)(20.0f * (1.0f - t)));
      uint8_t val = (uint8_t)(60.0f + 175.0f * t);
      diskAdd(ring, seg, CHSV(hue, 255, val));
    }
  }
}

// ---------------------------------------------------------------------------
// Основной рендер
// ---------------------------------------------------------------------------
void renderEffect_Lava() {
  const uint8_t eff = EFFECT_LAVA;

  // --- Параметры ---
  int16_t coverPct  = paramsValue(eff, P_COVER);
  uint8_t count     = (uint8_t)constrain((int)paramsValue(eff, P_COUNT), 1, LAVA_MAX_BLOBS);
  int16_t spread    = paramsValue(eff, P_SPREAD);
  float heatParam   = (float)paramsValue(eff, P_HEAT);
  float viscParam   = (float)paramsValue(eff, P_VISC);
  float elastParam  = (float)paramsValue(eff, P_ELAST);
  float speedParam  = (float)paramsValue(eff, P_SPEED);
  float mergePct    = (float)paramsValue(eff, P_MERGE);
  float splitPct    = (float)paramsValue(eff, P_SPLIT);
  uint8_t baseHue   = (uint8_t)paramsRealValue(eff, P_COLOR);

  // --- Раскладка: число сгустков и общая площадь ---
  // Общая площадь лавы: доля матрицы. Делим на 768 пикселей боковой матрицы
  // и учитываем, что перекрытия сгустков считаются дважды.
  float totalArea = (coverPct / 100.0f) * (float)(MATRIX_WIDTH * MATRIX_HEIGHT) /
                    LAVA_COVER_FUDGE;

  // раскладку обновляем всегда: она сама следит за числом сгустков и
  // пересчитывает площади при смене покрытия (плавно, за счёт areaScale)
  lavaLayout(count, totalArea, spread);

  // --- Шаг физики: реальное время, ограниченное сверху и снизу ---
  uint32_t now = millis();
  float dt = (g_lavaLastMs == 0) ? (1.0f / 24.0f)
                                 : (float)(now - g_lavaLastMs) / 1000.0f;
  g_lavaLastMs = now;
  if (dt < LAVA_DT_MIN) dt = LAVA_DT_MIN;
  if (dt > LAVA_DT_MAX) dt = LAVA_DT_MAX;

  // --- Физические коэффициенты из параметров ---
  float heatRate  = LAVA_HEAT_RELAX * mapf(heatParam, 1, 16, 0.35f, 2.2f);
  float viscBase  = mapf(viscParam, 1, 16, 0.6f, 5.0f);
  float elastBase = mapf(elastParam, 1, 16, 3.0f, 12.0f);
  float buoyBase  = LAVA_BUOYANCY * mapf(speedParam, 1, 16, 0.35f, 1.8f);
  float mergeCh   = mergePct / 100.0f * 0.06f;
  float splitCh   = splitPct / 100.0f * 0.02f;

  lavaPhysics(dt, heatRate, viscBase, elastBase, buoyBase, 1.0f, mergeCh, splitCh);

  // --- Отрисовка ---
  for (uint8_t i = 0; i < g_lavaCount; i++) {
    LavaBlob& b = g_lava[i];
    if (!b.alive) continue;

    uint16_t painted = lavaRenderSide(b, baseHue);
    lavaRenderDisk(b, baseHue);

    // Поддержание площади: «лава несжимаема». Масштаб формы подстраивается
    // так, чтобы закрашенная площадь совпадала с целевой.
    if (painted > 0 && b.area > 1.0f) {
      float err = b.area / (float)painted;
      b.areaScale *= 1.0f + (err - 1.0f) * 0.25f;
      if (b.areaScale < 0.4f) b.areaScale = 0.4f;
      if (b.areaScale > 2.2f) b.areaScale = 2.2f;
    }
  }
}
