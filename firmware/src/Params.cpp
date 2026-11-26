#include "Params.h"
#include "Config.h"

// ============================================================================
// РЕЕСТР ПАРАМЕТРОВ ЭФФЕКТОВ — реализация
// ============================================================================

// ---------------------------------------------------------------------------
// Таблицы параметров (диапазоны — по ТЗ, раздел 5)
// ---------------------------------------------------------------------------

// Эффект 1 — Статический: Яркость, Основной цвет
const ParamSpec PARAMS_STATIC[] = {
  { "BRIGHT",  1,  16, PK_BRIGHTNESS, 0 },
  { "COLOR",   1,  64, PK_COLOR,     PF_WRAP },
};
static const int16_t DEF_STATIC[] = { 8, 1 };

// Эффект 2 — Динамическая Z-радуга: Яркость, Масштаб, Скорость
const ParamSpec PARAMS_RAINBOW_Z[] = {
  { "BRIGHT",  1,   16, PK_BRIGHTNESS, 0 },
  { "SCALE",   1,   32, PK_SCALE,     0 },
  { "SPEED", -32,   32, PK_SPEED,     PF_WRAP },
};
static const int16_t DEF_RAINBOW_Z[] = { 8, 16, 8 };

// Эффект 3 — Динамическая круговая радуга: Яркость, Скорость
const ParamSpec PARAMS_RAINBOW_CIRCLE[] = {
  { "BRIGHT",  1,   16, PK_BRIGHTNESS, 0 },
  { "SPEED", -32,   32, PK_SPEED,     PF_WRAP },
};
static const int16_t DEF_RAINBOW_CIRCLE[] = { 8, 8 };

// Эффект 4 — Жидкость
const ParamSpec PARAMS_FLUID[] = {
  { "MODE",      0,    1, PK_MODE,      PF_WRAP },  // 0 = Авто, 1 = Ручной
  { "FILL",      1,  100, PK_PERCENT,   0 },
  { "VISCOS",    1,   16, PK_SCALE,     0 },
  { "STIFF",     1,   16, PK_SCALE,     0 },
  { "CORR X",    1,  360, PK_ANGLE,     PF_WRAP },
  { "CORR Y",    1,  360, PK_ANGLE,     PF_WRAP },
  { "CORR Z",    1,  360, PK_ANGLE,     PF_WRAP },
  { "G POWER",   0,   17, PK_G_POWER,   0 },
  { "BUB CNT",   1,   32, PK_COUNT,     0 },
  { "BUB SIZE",  1,   16, PK_SIZE,      0 },
  { "BUB SPD",   1,   16, PK_SCALE,     0 },
  { "BUB TAIL",  0,   16, PK_SCALE,     0 },
  { "BUB COLOR", 1,   64, PK_COLOR,     PF_WRAP },
  { "LIQ COLOR", 1,   64, PK_COLOR,     PF_WRAP },
};
static const int16_t DEF_FLUID[] = {
  0,   // MODE: Авто
  60,  // FILL: 60 %
  8,   // VISCOS
  5,   // STIFF
  1,   // CORR X
  1,   // CORR Y
  1,   // CORR Z
  10,  // G POWER -> 100 %
  6,   // BUB CNT
  2,   // BUB SIZE
  8,   // BUB SPD
  3,   // BUB TAIL
  16,  // BUB COLOR
  200, // LIQ COLOR
};

// Эффект 5 — Спираль
const ParamSpec PARAMS_SPIRAL[] = {
  { "ANGLE",    0,  180, PK_ANGLE,     0 },
  { "CONTRAST", 1,   32, PK_COUNT,     0 },
  { "COLOR",    1,   64, PK_COLOR,     PF_WRAP },
  { "BG COLOR", 1,   64, PK_COLOR,     PF_WRAP },
  { "SPEED",  -32,   32, PK_SPEED,     PF_WRAP },
};
static const int16_t DEF_SPIRAL[] = { 45, 8, 1, 1, 8 };

// Эффект 6 — Мячи в вакууме
const ParamSpec PARAMS_BALLS[] = {
  { "COUNT",  1, 25, PK_COUNT, 0 },
  { "SIZE",   2, 16, PK_SIZE,  0 },
  { "SPEED",  1, 16, PK_SCALE, 0 },
};
static const int16_t DEF_BALLS[] = { 8, 5, 6 };

// Эффект 7 — Лавовая лампа
// COVER — доля площади матрицы, занятая лавой (по умолчанию половина);
// COUNT — число сгустков; SPREAD — разброс их размеров
const ParamSpec PARAMS_LAVA[] = {
  { "BRIGHT",  1,  16, PK_BRIGHTNESS, 0 },
  { "COVER",  10,  90, PK_PERCENT,   0 },
  { "COUNT",   1,  12, PK_COUNT,     0 },
  { "SPREAD",  1,  16, PK_SCALE,     0 },
  { "VISCOS",  1,  16, PK_SCALE,     0 },
  { "ELAST",   1,  16, PK_SCALE,     0 },
  { "HEAT",    1,  16, PK_SCALE,     0 },
  { "MERGE",   0,  20, PK_PERCENT,   0 },
  { "SPLIT",   0,  20, PK_PERCENT,   0 },
  { "SPEED",   1,  16, PK_SCALE,     0 },
  { "COLOR",   1,  64, PK_COLOR,     PF_WRAP },
};
static const int16_t DEF_LAVA[] = {
  8,   // BRIGHT
  50,  // COVER: половина площади матрицы
  7,   // COUNT
  7,   // SPREAD
  8,   // VISCOS
  8,   // ELAST
  8,   // HEAT
  2,   // MERGE: редко
  1,   // SPLIT: очень редко
  8,   // SPEED
  1,   // COLOR
};

// Эффект 8 — 3D-сцена, привязанная к гравитации
// Эффект 9 — 3D-сцена, привязанная к корпусу
// Набор параметров одинаковый: отличается только логика привязки.
// FIGURE — тип фигуры, SIZE — размер в % от радиуса корпуса,
// EDGE — ширина подсветки рёбер (мм), DIST — расстояние до наблюдателя (мм),
// EYE H — высота наблюдателя над центром лампы (мм).
const ParamSpec PARAMS_SCENE_GRAV[] = {
  { "BRIGHT",  1,   16, PK_BRIGHTNESS, 0 },
  { "FIGURE",  0,    3, PK_FIGURE,     PF_WRAP },
  { "SIZE",   10,  100, PK_PERCENT,    0 },
  { "COLOR",   1,   64, PK_COLOR,      PF_WRAP },
  { "EDGE",    0,   20, PK_SCALE,      0 },
  { "DIST",  300,  900, PK_SCALE,      0 },
  { "EYE H",   0,  400, PK_SCALE,      0 },
};
static const int16_t DEF_SCENE_GRAV[] = {
  8,    // BRIGHT
  0,    // FIGURE: куб
  55,   // SIZE: 55 % от радиуса корпуса
  1,    // COLOR
  8,    // EDGE: 8 мм
  600,  // DIST: вытянутая рука
  100,  // EYE H: 100 мм над центром лампы
};

const ParamSpec PARAMS_SCENE_BODY[] = {
  { "BRIGHT",  1,   16, PK_BRIGHTNESS, 0 },
  { "FIGURE",  0,    3, PK_FIGURE,     PF_WRAP },
  { "SIZE",   10,  100, PK_PERCENT,    0 },
  { "COLOR",   1,   64, PK_COLOR,      PF_WRAP },
  { "EDGE",    0,   20, PK_SCALE,      0 },
  { "DIST",  300,  900, PK_SCALE,      0 },
  { "EYE H",   0,  400, PK_SCALE,      0 },
};
static const int16_t DEF_SCENE_BODY[] = {
  8,    // BRIGHT
  0,    // FIGURE: куб
  55,   // SIZE
  1,    // COLOR
  8,    // EDGE
  600,  // DIST
  100,  // EYE H
};

// ---------------------------------------------------------------------------
// Экспоненциальная шкала силы G
// ---------------------------------------------------------------------------
const uint16_t G_POWER_TABLE[] = {
  0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100,
  110, 125, 150, 200, 300, 500, 1000
};
const uint8_t G_POWER_TABLE_SIZE = sizeof(G_POWER_TABLE) / sizeof(G_POWER_TABLE[0]);

int16_t paramsGPowerPercent(int16_t index) {
  if (index < 0) index = 0;
  if (index >= (int16_t)G_POWER_TABLE_SIZE) index = (int16_t)G_POWER_TABLE_SIZE - 1;
  return (int16_t)G_POWER_TABLE[index];
}

// ---------------------------------------------------------------------------
// Хранилище значений
// ---------------------------------------------------------------------------
static int16_t g_values[EFFECT_COUNT][PARAM_TABLE_MAX];

// Таблицы и значения по умолчанию сведены в общий массив для удобства инициализации
static const ParamSpec* const SPECS[EFFECT_COUNT] = {
  PARAMS_STATIC, PARAMS_RAINBOW_Z, PARAMS_RAINBOW_CIRCLE,
  PARAMS_FLUID,  PARAMS_SPIRAL,    PARAMS_BALLS,
  PARAMS_LAVA,   PARAMS_SCENE_GRAV, PARAMS_SCENE_BODY
};

static const int16_t* const DEFAULTS[EFFECT_COUNT] = {
  DEF_STATIC, DEF_RAINBOW_Z, DEF_RAINBOW_CIRCLE,
  DEF_FLUID,  DEF_SPIRAL,    DEF_BALLS,
  DEF_LAVA,   DEF_SCENE_GRAV, DEF_SCENE_BODY
};

// Количество параметров каждого эффекта (вычисляется на этапе компиляции)
static const uint8_t SPEC_COUNT[EFFECT_COUNT] = {
  (uint8_t)(sizeof(PARAMS_STATIC)        / sizeof(ParamSpec)),
  (uint8_t)(sizeof(PARAMS_RAINBOW_Z)     / sizeof(ParamSpec)),
  (uint8_t)(sizeof(PARAMS_RAINBOW_CIRCLE)/ sizeof(ParamSpec)),
  (uint8_t)(sizeof(PARAMS_FLUID)         / sizeof(ParamSpec)),
  (uint8_t)(sizeof(PARAMS_SPIRAL)        / sizeof(ParamSpec)),
  (uint8_t)(sizeof(PARAMS_BALLS)         / sizeof(ParamSpec)),
  (uint8_t)(sizeof(PARAMS_LAVA)          / sizeof(ParamSpec)),
  (uint8_t)(sizeof(PARAMS_SCENE_GRAV)    / sizeof(ParamSpec)),
  (uint8_t)(sizeof(PARAMS_SCENE_BODY)    / sizeof(ParamSpec))
};

// ---------------------------------------------------------------------------
// Функции
// ---------------------------------------------------------------------------

void paramsInit() {
  for (uint8_t e = 0; e < EFFECT_COUNT; e++) {
    uint8_t n = paramsCount(e);
    for (uint8_t p = 0; p < n; p++) {
      g_values[e][p] = DEFAULTS[e][p];
    }
  }
}

uint8_t paramsCount(uint8_t effect) {
  if (effect >= EFFECT_COUNT) return 0;
  return SPEC_COUNT[effect];
}

const ParamSpec* paramsSpec(uint8_t effect) {
  if (effect >= EFFECT_COUNT) return PARAMS_STATIC;
  return SPECS[effect];
}

int16_t paramsValue(uint8_t effect, uint8_t param) {
  if (effect >= EFFECT_COUNT || param >= PARAM_TABLE_MAX) return 0;
  return g_values[effect][param];
}

void paramsSetValue(uint8_t effect, uint8_t param, int16_t value) {
  if (effect >= EFFECT_COUNT || param >= PARAM_TABLE_MAX) return;
  const ParamSpec* sp = &SPECS[effect][param];
  if (value < sp->min) value = sp->min;
  if (value > sp->max) value = sp->max;
  g_values[effect][param] = value;
}

void paramsAdjust(uint8_t effect, uint8_t param, int32_t delta) {
  if (effect >= EFFECT_COUNT || param >= PARAM_TABLE_MAX) return;
  const ParamSpec* sp = &SPECS[effect][param];
  if (delta == 0) return;

  int32_t v = (int32_t)g_values[effect][param] + delta;
  if (sp->flags & PF_WRAP) {
    int32_t range = (int32_t)sp->max - (int32_t)sp->min + 1;
    v = ((v - sp->min) % range + range) % range + sp->min;
  } else {
    if (v < sp->min) v = sp->min;
    if (v > sp->max) v = sp->max;
  }
  g_values[effect][param] = (int16_t)v;
}

int16_t paramsDisplayValue(uint8_t effect, uint8_t param) {
  if (effect >= EFFECT_COUNT || param >= PARAM_TABLE_MAX) return 0;
  const ParamSpec* sp = &SPECS[effect][param];
  int16_t v = g_values[effect][param];

  switch (sp->kind) {
    case PK_G_POWER: return paramsGPowerPercent(v);
    case PK_MODE:    return v;
    default:         return v;
  }
}

const char* paramsDisplayText(uint8_t effect, uint8_t param) {
  if (effect >= EFFECT_COUNT || param >= PARAM_TABLE_MAX) return "";
  const ParamSpec* sp = &SPECS[effect][param];
  int16_t v = g_values[effect][param];

  if (sp->kind == PK_MODE) {
    return (v == 0) ? "AUTO" : "MANU";
  }
  if (sp->kind == PK_FIGURE) {
    switch (v) {
      case 0:  return "CUBE";
      case 1:  return "OCTA";
      case 2:  return "PRISM";
      default: return "SPHERE";
    }
  }
  return "";
}

int16_t paramsRealValue(uint8_t effect, uint8_t param) {
  if (effect >= EFFECT_COUNT || param >= PARAM_TABLE_MAX) return 0;
  const ParamSpec* sp = &SPECS[effect][param];
  int16_t v = g_values[effect][param];

  switch (sp->kind) {
    case PK_BRIGHTNESS: return (int16_t)map(v, 1, 16, 16, 255);   // 1..16 -> 16..255
    case PK_COLOR:      return (int16_t)map(v - 1, 0, 63, 0, 255); // 1..64 -> Hue 0..255
    case PK_G_POWER:    return paramsGPowerPercent(v);
    default:            return v;
  }
}

uint8_t paramsSliderFraction(uint8_t effect, uint8_t param) {
  if (effect >= EFFECT_COUNT || param >= PARAM_TABLE_MAX) return 0;
  const ParamSpec* sp = &SPECS[effect][param];
  int16_t v = g_values[effect][param];
  if (sp->max <= sp->min) return 0;
  return (uint8_t)map(v, sp->min, sp->max, 0, 255);
}

uint8_t paramsFindByKind(uint8_t effect, uint8_t kind) {
  if (effect >= EFFECT_COUNT) return 0xFF;
  uint8_t n = paramsCount(effect);
  for (uint8_t p = 0; p < n; p++) {
    if (SPECS[effect][p].kind == kind) return p;
  }
  return 0xFF;
}
