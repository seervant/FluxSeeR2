#include "Effects.h"
#include "LedGeometry.h"
#include "Params.h"

// ============================================================================
// ЗАГЛУШКИ ЭФФЕКТОВ 2..6
//
// Эти эффекты ещё не перенесены из проекта 0.2.1 и будут реализованы
// по одному блоку. Пока рисуется узнаваемый маркер (бегущая точка по
// окружности) — чтобы было видно, что слот эффекта работает и что
// крутилка энкодера / кнопки переключают именно то, что нужно.
//
// ВАЖНО: заглушка рисует ОДНОКРАТНЫЙ кадр, яркость берётся из параметра
// эффекта, поэтому эффект № в списке на OLED помечен звёздочкой.
// ============================================================================

// Бегущая точка по окружности боковой матрицы
static void placeholderRunner(uint8_t effectId, uint8_t hueBase) {
  const uint8_t eff = effectId;
  uint8_t speedParam = paramsFindByKind(eff, PK_SPEED);
  int16_t speed = (speedParam != 0xFF) ? paramsValue(eff, speedParam) : 8;

  // Скорость приходит как знаковое значение (-32..+32): знак задаёт направление
  static int32_t phase[EFFECT_COUNT] = { 0 };
  const int32_t total = (int32_t)MATRIX_WIDTH * MATRIX_HEIGHT;
  phase[eff] = (phase[eff] + speed) % total;
  if (phase[eff] < 0) phase[eff] += total;

  uint8_t x = (uint8_t)(phase[eff] % MATRIX_WIDTH);
  uint8_t y = (uint8_t)((phase[eff] / MATRIX_WIDTH) % MATRIX_HEIGHT);

  CRGB color = CHSV((uint8_t)(hueBase + phase[eff]), 255, 255);

  // Точка на боковой матрице
  for (int8_t dy = -1; dy <= 1; dy++) {
    for (int8_t dx = -1; dx <= 1; dx++) {
      int16_t nx = (int16_t)x + dx;
      int16_t ny = (int16_t)y + dy;
      if (nx < 0 || nx >= MATRIX_WIDTH || ny < 0 || ny >= MATRIX_HEIGHT) continue;
      uint8_t weight = (dx == 0 && dy == 0) ? 255 : 90;
      sideAdd((uint8_t)nx, (uint8_t)ny, color.nscale8(weight));
    }
  }

  // Соответствующая точка на верхнем диске (тот же угол, внешнее кольцо)
  uint8_t ring = DISK_RING_COUNT - 1;
  uint8_t seg = (uint8_t)(phase[eff] % diskRingSize(ring));
  diskAdd(ring, seg, color);}

// Эффект 2 — Динамическая Z-радуга (заглушка)
void renderEffect_RainbowZ() {
  placeholderRunner(EFFECT_RAINBOW_Z, 0);
}

// Эффект 3 — Динамическая круговая радуга (заглушка)
void renderEffect_RainbowCircle() {
  placeholderRunner(EFFECT_RAINBOW_CIRCLE, 40);
}

// Эффект 4 — Жидкость (заглушка; порт физики из 0.2.1 — следующий блок)
void renderEffect_Fluid() {
  placeholderRunner(EFFECT_FLUID, 96);
}

// Эффект 5 — Спираль (заглушка)
void renderEffect_Spiral() {
  placeholderRunner(EFFECT_SPIRAL, 160);
}

// Эффект 6 — Мячи в вакууме (заглушка)
void renderEffect_Balls() {
  placeholderRunner(EFFECT_BALLS, 210);
}
