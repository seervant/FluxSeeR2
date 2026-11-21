#pragma once
#include <Arduino.h>
#include "Types.h"

// ============================================================================
// РЕЕСТР ЭФФЕКТОВ
//
// Каждый эффект — функция отрисовки, которая заполняет буферы лент.
// Общая яркость применяется централизованно в effectsRender().
// ============================================================================

struct EffectDescriptor {
  const char* name;         // ASCII-название для OLED
  void (*render)();         // Функция отрисовки
  bool implemented;         // false -> эффект ещё не перенесён (заглушка)
};

extern const EffectDescriptor g_effects[EFFECT_COUNT];

// Отрисовка активного эффекта + выдача в ленты
void effectsRender();

// --- Отрисовка конкретных эффектов ---
void renderEffect_Static();          // Эффект 1 (реализован)
void renderEffect_RainbowZ();        // Эффект 2 (реализован)
void renderEffect_RainbowCircle();   // Эффект 3 (заглушка)
void renderEffect_Fluid();           // Эффект 4 (заглушка, порт из 0.2.1 — след. блок)
void renderEffect_Spiral();          // Эффект 5 (заглушка)
void renderEffect_Balls();           // Эффект 6 (заглушка)
