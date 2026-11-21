#include "Effects.h"
#include "LedGeometry.h"
#include "Params.h"

// ============================================================================
// ЭФФЕКТ 1 — СТАТИЧЕСКИЙ
//
// Вся подсветка (боковая матрица и верхний диск) заливается одним ровным
// цветом. Без градиентов, пульсации, движения и мерцания.
// Яркость задаётся отдельным параметром эффекта (общая для лампы).
//
// Перенесено из проекта 0.2.1 (EffectStatic.cpp) с переходом на общую
// таблицу параметров и две независимые ленты.
// ============================================================================

void renderEffect_Static() {
  const uint8_t eff = EFFECT_STATIC;

  uint8_t colorParam = paramsFindByKind(eff, PK_COLOR);
  CRGB color = (colorParam != 0xFF)
             ? CHSV((uint8_t)paramsRealValue(eff, colorParam), 255, 255)
             : CRGB(255, 0, 0);

  sideFill(color);
  diskFill(color);
}
