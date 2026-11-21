#include "Settings.h"
#include "Config.h"
#include "Controls.h"
#include "Params.h"
#include <Preferences.h>

// ============================================================================
// СОХРАНЕНИЕ НАСТРОЕК — реализация
// ============================================================================

static Preferences prefs;
static const char* NS = "flexeer";

// ---------------------------------------------------------------------------
// Загрузка сохранённых настроек
// ---------------------------------------------------------------------------
void settingsLoad() {
  prefs.begin(NS, false);

  // Значения параметров: ключ вида "e<эффект>p<параметр>"
  for (uint8_t e = 0; e < EFFECT_COUNT; e++) {
    uint8_t n = paramsCount(e);
    for (uint8_t p = 0; p < n; p++) {
      char key[10];
      snprintf(key, sizeof(key), "e%dp%02d", (int)e, (int)p);
      int16_t v = prefs.getShort(key, paramsValue(e, p));
      paramsSetValue(e, p, v);
    }
  }

  // Активный эффект
  uint8_t eff = prefs.getUChar("effect", 0);
  if (eff < EFFECT_COUNT) controlsSetEffect(eff);

  // Активный параметр (сдвигаемся от 0 через счётчик)
  uint8_t par = prefs.getUChar("param", 0);
  uint8_t parCount = paramsCount(eff < EFFECT_COUNT ? eff : 0);
  if (par < parCount) {
    for (uint8_t i = 0; i < par; i++) controlsNextParam();
  }

  prefs.end();
  controlsClearDirty();
}

// ---------------------------------------------------------------------------
// Сохранение настроек
// ---------------------------------------------------------------------------
void settingsSave() {
  prefs.begin(NS, false);

  for (uint8_t e = 0; e < EFFECT_COUNT; e++) {
    uint8_t n = paramsCount(e);
    for (uint8_t p = 0; p < n; p++) {
      char key[10];
      snprintf(key, sizeof(key), "e%dp%02d", (int)e, (int)p);
      prefs.putShort(key, paramsValue(e, p));
    }
  }

  prefs.putUChar("effect", controlsCurrentEffect());
  prefs.putUChar("param",  controlsCurrentParam());

  prefs.end();
  controlsClearDirty();
}
