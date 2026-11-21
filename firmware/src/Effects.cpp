#include "Effects.h"
#include "Config.h"
#include "Controls.h"
#include "LedGeometry.h"
#include "Params.h"

// ============================================================================
// РЕЕСТР ЭФФЕКТОВ — реализация
// ============================================================================

const EffectDescriptor g_effects[EFFECT_COUNT] = {
  { "STATIC",   renderEffect_Static,        true  },  // 1. Статический
  { "RAINBOW Z", renderEffect_RainbowZ,      true  },  // 2. Динамическая Z-радуга
  { "RAINBOW C", renderEffect_RainbowCircle, false },  // 3. Динамическая круговая радуга
  { "FLUID",    renderEffect_Fluid,          true  },  // 4. Жидкость
  { "SPIRAL",   renderEffect_Spiral,         false },  // 5. Спираль
  { "BALLS",    renderEffect_Balls,          false },  // 6. Мячи в вакууме
};

// ---------------------------------------------------------------------------
// Кадр: очистка -> отрисовка эффекта -> общая яркость -> ограничение тока -> выдача
// ---------------------------------------------------------------------------
void effectsRender() {
  uint8_t eff = controlsCurrentEffect();
  if (eff >= EFFECT_COUNT) eff = EFFECT_STATIC;

  ledsClear();
  g_effects[eff].render();

  // Общая яркость эффекта (параметр типа PK_BRIGHTNESS)
  uint8_t brightParam = paramsFindByKind(eff, PK_BRIGHTNESS);
  if (brightParam != 0xFF) {
    FastLED.setBrightness((uint8_t)paramsRealValue(eff, brightParam));
  } else {
    FastLED.setBrightness(255);
  }

  // Ограничение тока общим бюджетом на ОБЕ ленты (иначе 949 белых LED
  // потянут 56.9 А при пределе преобразователя 20 А)
  ledsApplyPowerLimit(LED_POWER_LIMIT_MA);

  FastLED.show();
}