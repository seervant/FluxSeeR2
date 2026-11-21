#include "Effects.h"
#include "Config.h"
#include "LedGeometry.h"
#include "Params.h"
#include "Sensors.h"
#include <math.h>

// ============================================================================
// ЭФФЕКТ 4 — ЖИДКОСТЬ
//
// Физика перенесена из проекта 0.2.1:
//   - «пружина — затухание» на векторе гравитации (LiquidPhysics3D):
//     корпус качается, жидкость отстаёт с задержкой;
//   - потенциал в цилиндрических координатах
//       P(x,y) = A*cos(θ)*gx + A*sin(θ)*gy + (y - 7.5)*gz
//     поверхность получается наклонной и следует за вектором тяжести;
//   - пузырьки всплывают против градиента потенциала, оставляя шлейфы.
//
// Что добавлено по ТЗ:
//   - наполненность 1–100 % и режим «Авто» (уровень следует за зарядом);
//   - коррекция ориентации по трём осям X, Y, Z (1–360°);
//   - процент силы притяжения G (1–1000 %) — усиливает наклон поверхности;
//   - индивидуальный размер пузырьков;
//   - верхний диск: бассейн жидкости, смещающийся при наклоне корпуса.
//
// Оси: y = 0 — нижний ряд матрицы, y = 15 — верхний.
// ============================================================================

// --- Состояние эффекта (переживает переключение эффектов) ---
static LiquidPhysics3D liquid;
static Bubble bubbles[MAX_BUBBLES];

// --- Индексы параметров эффекта «Жидкость» ---
static const uint8_t P_MODE     = 0;   // 0 = Авто (уровень по заряду), 1 = Ручной
static const uint8_t P_FILL     = 1;   // наполненность, %
static const uint8_t P_VISCOS   = 2;   // вязкость (затухание)
static const uint8_t P_STIFF    = 3;   // отклик жёсткости
static const uint8_t P_CORR_X   = 4;   // коррекция по X, градусы
static const uint8_t P_CORR_Y   = 5;   // коррекция по Y, градусы
static const uint8_t P_CORR_Z   = 6;   // коррекция по Z, градусы
static const uint8_t P_GPOWER   = 7;   // процент силы притяжения G
static const uint8_t P_BUBCNT   = 8;   // количество пузырьков
static const uint8_t P_BUBSIZE  = 9;   // размер пузырьков
static const uint8_t P_BUBSPD   = 10;  // скорость пузырьков
static const uint8_t P_BUBTAIL  = 11;  // длина шлейфа
static const uint8_t P_BUBCOLOR = 12;  // цвет пузырьков
static const uint8_t P_LIQCOLOR = 13;  // цвет жидкости

// Косинусы/синусы по периметру цилиндра (не меняются от кадра к кадру)
static float s_cosW[MATRIX_WIDTH];
static float s_sinW[MATRIX_WIDTH];
static bool  s_trigReady = false;

static void prepareTrig() {
  if (s_trigReady) return;
  for (uint8_t x = 0; x < MATRIX_WIDTH; x++) {
    float a = (float)x * (2.0f * PI / (float)MATRIX_WIDTH);
    s_cosW[x] = cosf(a);
    s_sinW[x] = sinf(a);
  }
  s_trigReady = true;
}

// ---------------------------------------------------------------------------
// Поворот вектора гравитации в систему координат лампы
//   Z — вокруг оси лампы (плоскость XY)
//   X — в плоскости YZ
//   Y — в плоскости XZ
// ---------------------------------------------------------------------------
static void rotateGravity(float& gx, float& gy, float& gz,
                          float angleX, float angleY, float angleZ) {
  float c = cosf(angleZ), s = sinf(angleZ);
  float nx = gx * c - gy * s;
  float ny = gx * s + gy * c;
  gx = nx; gy = ny;

  c = cosf(angleX); s = sinf(angleX);
  ny = gy * c - gz * s;
  float nz = gy * s + gz * c;
  gy = ny; gz = nz;

  c = cosf(angleY); s = sinf(angleY);
  nx = gx * c + gz * s;
  nz = -gx * s + gz * c;
  gx = nx; gz = nz;
}

// ---------------------------------------------------------------------------
// Круг с субпиксельной точностью по вертикали (для пузырьков)
// ---------------------------------------------------------------------------
static void drawDisc(float cx, float cy, float radius, CRGB color, float weight) {
  if (radius < 0.5f) return;
  int r = (int)ceilf(radius);

  for (int dx = -r; dx <= r; dx++) {
    float px = cx + (float)dx;
    if (px < 0.0f || px >= (float)MATRIX_WIDTH) continue;

    for (int dy = -r; dy <= r; dy++) {
      float dist = sqrtf((float)(dx * dx) + (float)(dy * dy));
      if (dist > radius + 0.5f) continue;

      // мягкий край: ярче в центре круга
      float w = weight * (1.0f - 0.45f * dist / (radius + 0.5f));
      if (w <= 0.0f) continue;

      float py = cy + (float)dy;
      int y0 = (int)floorf(py);
      float frac = py - (float)y0;

      if (y0 >= 0 && y0 < MATRIX_HEIGHT) {
        sideAdd((uint8_t)px, (uint8_t)y0,
                color.nscale8((uint8_t)((1.0f - frac) * w * 255.0f)));
      }
      int y1 = y0 + 1;
      if (y1 >= 0 && y1 < MATRIX_HEIGHT) {
        sideAdd((uint8_t)px, (uint8_t)y1,
                color.nscale8((uint8_t)(frac * w * 255.0f)));
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Потенциал в точке (x, y)
// ---------------------------------------------------------------------------
static float getPotential(float x, float y, float gx, float gy, float gz) {
  float colAngle = x * (2.0f * PI / (float)MATRIX_WIDTH);
  return (LIQ_TILT_BASE * cosf(colAngle)) * gx +
         (LIQ_TILT_BASE * sinf(colAngle)) * gy +
         (y - 7.5f) * gz;
}

// ---------------------------------------------------------------------------
// Пузырьки: всплытие по градиенту потенциала + шлейф
// ---------------------------------------------------------------------------
static void updateAndDrawBubbles(float gx, float gy, float gz, float surfaceThreshold,
                                  uint8_t bubbleCount, float baseSpeed, uint8_t tailLength,
                                  float baseHue, uint8_t baseRadius) {
  for (uint8_t i = 0; i < MAX_BUBBLES; i++) {
    if (i >= bubbleCount) { bubbles[i].active = false; continue; }

    if (!bubbles[i].active) {
      // шанс появления: при зарядке лампа «кипит» активнее
      if (random8() < (data.isCharging ? 16 : 6)) {
        float rx = random(0, MATRIX_WIDTH);
        float ry = random(0, MATRIX_HEIGHT);
        if (getPotential(rx, ry, gx, gy, gz) > (surfaceThreshold + 1.5f)) {
          bubbles[i].x = rx;
          bubbles[i].y = ry;
          bubbles[i].speedMult = random(80, 120) / 100.0f;
          bubbles[i].hueOffset = (random(0, 2) == 0) ? random(0, 12) : (256 - random(1, 12));
          // индивидуальный размер: +/- 1 от общего
          int8_t d = (int8_t)random(0, 3) - 1;
          int r = (int)baseRadius + d;
          if (r < 1) r = 1;
          if (r > 4) r = 4;
          bubbles[i].size = (uint8_t)r;
          bubbles[i].active = true;
        }
      }
      continue;
    }

    float curP = getPotential(bubbles[i].x, bubbles[i].y, gx, gy, gz);
    if (curP <= surfaceThreshold) { bubbles[i].active = false; continue; }

    float moveStep = baseSpeed * bubbles[i].speedMult;
    float dirX = -(getPotential(bubbles[i].x + 0.5f, bubbles[i].y, gx, gy, gz) - curP);
    float dirY = -(getPotential(bubbles[i].x, bubbles[i].y + 0.5f, gx, gy, gz) - curP);

    float dirLen = sqrtf(dirX * dirX + dirY * dirY);
    if (dirLen > 0.001f) {
      dirX /= dirLen;
      dirY /= dirLen;
    }

    bubbles[i].x += dirX * moveStep;
    bubbles[i].y += dirY * moveStep;

    if (bubbles[i].x < 0) bubbles[i].x += MATRIX_WIDTH;
    if (bubbles[i].x >= MATRIX_WIDTH) bubbles[i].x -= MATRIX_WIDTH;
    bubbles[i].y = constrain(bubbles[i].y, 0.0f, (float)(MATRIX_HEIGHT - 1));

    CRGB bColor = CHSV((uint8_t)(baseHue + bubbles[i].hueOffset), 220, 255);
    drawDisc(bubbles[i].x, bubbles[i].y, (float)bubbles[i].size, bColor, 1.0f);

    // шлейф: точки за пузырьком, затухающие с расстоянием
    if (tailLength > 0 && dirLen > 0.001f) {
      float tailR = (float)bubbles[i].size * 0.5f;
      if (tailR < 0.5f) tailR = 0.5f;
      for (uint8_t t = 1; t <= tailLength; t++) {
        float tailX = bubbles[i].x - (dirX * t * 0.6f);
        float tailY = bubbles[i].y - (dirY * t * 0.6f);
        if (tailX < 0) tailX += MATRIX_WIDTH;
        if (tailX >= MATRIX_WIDTH) tailX -= MATRIX_WIDTH;

        float tailWeight = 1.0f - ((float)t / (float)(tailLength + 1));
        drawDisc(tailX, tailY, tailR, bColor, tailWeight * 0.7f);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Верхний диск: бассейн жидкости, смещающийся при наклоне корпуса
//
// Смотрим на лампу сверху: поверхность жидкости пересекает цилиндр по
// окружности, радиус которой задаётся наполненностью (V ~ h, R ~ sqrt(V)),
// а при наклоне корпуса бассейн смещается в сторону «низа».
// ---------------------------------------------------------------------------
static void renderDiskPool(float fillFraction, float gx, float gy,
                            const CRGB& liquidColor, const CRGB& surfaceColor) {
  float R = sqrtf(constrain(fillFraction, 0.0f, 1.0f));

  // направление и величина наклона в плоскости диска
  float tiltMag = sqrtf(gx * gx + gy * gy);
  float dirX = (tiltMag > 0.001f) ? gx / tiltMag : 0.0f;
  float dirY = (tiltMag > 0.001f) ? gy / tiltMag : 0.0f;

  // смещение не выходит за границы диска
  float off = (1.0f - R) * 0.45f * constrain(tiltMag, 0.0f, 1.0f);
  float cx = dirX * off;
  float cy = dirY * off;

  for (uint8_t ring = 0; ring < DISK_RING_COUNT; ring++) {
    float rr = diskRingRadius(ring);
    uint8_t n = diskRingSize(ring);

    for (uint8_t seg = 0; seg < n; seg++) {
      float ang = (float)seg * (2.0f * PI / (float)n);
      float px = rr * cosf(ang) - cx;
      float py = rr * sinf(ang) - cy;
      float d = sqrtf(px * px + py * py);

      if (d > R) continue;

      float edge = (R - d) / 0.18f;                 // 0 у кромки, 1 внутри
      if (edge < 0.0f) continue;

      uint8_t v = (uint8_t)(80 + 150 * constrain(edge, 0.0f, 1.0f));
      uint8_t blend = (edge < 0.35f) ? (uint8_t)(255 * (1.0f - edge / 0.35f)) : 0;

      CRGB c = CRGB((uint8_t)((int)liquidColor.r * v / 255),
                    (uint8_t)((int)liquidColor.g * v / 255),
                    (uint8_t)((int)liquidColor.b * v / 255));

      // блик на кромке (мениск)
      if (blend > 0) {
        c.r = (uint8_t)((int)c.r + ((int)surfaceColor.r * blend) / 255);
        c.g = (uint8_t)((int)c.g + ((int)surfaceColor.g * blend) / 255);
        c.b = (uint8_t)((int)c.b + ((int)surfaceColor.b * blend) / 255);
      }

      diskSet(ring, seg, c);
    }
  }
}

// ---------------------------------------------------------------------------
// Основной рендер
// ---------------------------------------------------------------------------
void renderEffect_Fluid() {
  prepareTrig();

  // --- Параметры ---
  const uint8_t eff = EFFECT_FLUID;

  // вязкость (затухание) и жёсткость (отклик)
  liquid.damping   = mapf((float)paramsValue(eff, P_VISCOS), 1, 16, 60, 95) / 100.0f;
  liquid.stiffness = mapf((float)paramsValue(eff, P_STIFF),  1, 16,  5, 35) / 100.0f;

  // наполненность: авто (по заряду) или ручной уровень
  float fillPct = (paramsValue(eff, P_MODE) == 0)
                ? data.smoothBattery
                : (float)paramsValue(eff, P_FILL);
  fillPct = constrain(fillPct, 0.0f, 100.0f);

  // порог потенциала: 0 % -> пусто, 100 % -> полностью
  float surfaceThreshold = 7.6f - (fillPct / 100.0f) * 16.2f;

  // --- Физика: пружина-затухание на векторе гравитации ---
  if (data.isCharging) {
    liquid.velG.x += (float)random(-25, 25) / 100.0f;
    liquid.velG.y += (float)random(-25, 25) / 100.0f;
  }

  liquid.velG.x = (liquid.velG.x + (data.accX - liquid.currentG.x) * liquid.stiffness) * liquid.damping;
  liquid.velG.y = (liquid.velG.y + (data.accY - liquid.currentG.y) * liquid.stiffness) * liquid.damping;
  liquid.velG.z = (liquid.velG.z + (data.accZ - liquid.currentG.z) * liquid.stiffness) * liquid.damping;

  liquid.currentG.x += liquid.velG.x;
  liquid.currentG.y += liquid.velG.y;
  liquid.currentG.z += liquid.velG.z;

  float gNorm = sqrtf(liquid.currentG.x * liquid.currentG.x +
                       liquid.currentG.y * liquid.currentG.y +
                       liquid.currentG.z * liquid.currentG.z);
  if (gNorm > 0.001f) {
    liquid.currentG.x /= gNorm;
    liquid.currentG.y /= gNorm;
    liquid.currentG.z /= gNorm;
  }

  // --- Коррекция ориентации (1 = 0 градусов) ---
  const float DEG = PI / 180.0f;
  float ax = ((float)paramsValue(eff, P_CORR_X) - 1.0f) * DEG;
  float ay = ((float)paramsValue(eff, P_CORR_Y) - 1.0f) * DEG;
  float az = ((float)paramsValue(eff, P_CORR_Z) - 1.0f) * DEG;

  float gx = liquid.currentG.x;
  float gy = liquid.currentG.y;
  float gz = liquid.currentG.z;
  rotateGravity(gx, gy, gz, ax, ay, az);

  // --- Процент силы притяжения G: усиливает наклон поверхности ---
  float gPower = (float)paramsRealValue(eff, P_GPOWER) / 100.0f;
  gx *= gPower;
  gy *= gPower;

  // --- Цвета ---
  uint8_t liquidHue  = (uint8_t)paramsRealValue(eff, P_LIQCOLOR);
  uint8_t surfaceHue = (uint8_t)(liquidHue + 10);
  CRGB liquidColor   = CHSV(liquidHue, 255, 200);
  CRGB surfaceColor  = CHSV(surfaceHue, 255, 255);

  // --- Боковая матрица: тело жидкости и поверхность ---
  for (uint8_t x = 0; x < MATRIX_WIDTH; x++) {
    float horiz = (LIQ_TILT_BASE * s_cosW[x]) * gx + (LIQ_TILT_BASE * s_sinW[x]) * gy;
    for (uint8_t y = 0; y < MATRIX_HEIGHT; y++) {
      float potential = horiz + ((float)y - 7.5f) * gz;

      if (potential > (surfaceThreshold + LIQ_SURFACE_BAND)) {
        // тело: яркость растёт к поверхности
        uint8_t v = (uint8_t)mapf((float)y, 0, MATRIX_HEIGHT - 1, 120, 225);
        sideSet(x, y, CHSV(liquidHue, 255, v));
      } else if (potential >= (surfaceThreshold - LIQ_SURFACE_BAND)) {
        // поверхность: при зарядке мерцает
        uint8_t v = data.isCharging ? (uint8_t)(200 + sin8((uint8_t)(millis() / 2))) : 255;
        sideSet(x, y, CHSV(surfaceHue, 255, v));
      }
    }
  }

  // --- Пузырьки ---
  uint8_t bubbleCount = (uint8_t)constrain((int)paramsValue(eff, P_BUBCNT), 1, MAX_BUBBLES);
  float baseSpeed = mapf((float)paramsValue(eff, P_BUBSPD), 1, 16, 8, 260) / 1000.0f;
  uint8_t tailLength = (uint8_t)constrain((int)paramsValue(eff, P_BUBTAIL), 0, 16);
  float baseHue = (float)paramsRealValue(eff, P_BUBCOLOR);
  uint8_t baseRadius = (uint8_t)mapf((float)paramsValue(eff, P_BUBSIZE), 1, 16, 1, 4);

  updateAndDrawBubbles(gx, gy, gz, surfaceThreshold,
                       bubbleCount, baseSpeed, tailLength, baseHue, baseRadius);

  // --- Верхний диск: бассейн жидкости ---
  renderDiskPool(fillPct / 100.0f, gx, gy, liquidColor, surfaceColor);
}