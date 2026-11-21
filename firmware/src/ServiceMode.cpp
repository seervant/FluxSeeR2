#include "ServiceMode.h"
#include "Config.h"
#include "Display.h"
#include "LedGeometry.h"
#include "Pins.h"

// ============================================================================
// РЕЖИМ ПРОВЕРКИ РАСКЛАДКИ — реализация
// ============================================================================

static bool    g_active = false;
static uint8_t g_screen = 0;

// Значения по экранам: 0 — столбец, 1 — строка, 2 — кольцо, 3 — сегмент
static int16_t g_value[SERVICE_SCREEN_COUNT] = { 0, 0, 0, 0 };

// ---------------------------------------------------------------------------
// Максимальное значение для текущего экрана
// ---------------------------------------------------------------------------
static int16_t screenMax() {
  switch (g_screen) {
    case 0: return MATRIX_WIDTH - 1;                                  // столбец
    case 1: return MATRIX_HEIGHT - 1;                                 // строка
    case 2: return DISK_RING_COUNT - 1;                                // кольцо
    case 3: return (int16_t)diskRingSize((uint8_t)g_value[2]) - 1;    // сегмент
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Вход/выход
// ---------------------------------------------------------------------------
void serviceInit() {
  // Кнопка энкодера должна быть зажата в момент включения питания
  g_active = (digitalRead(ENC_SW_PIN) == LOW);
}

bool serviceActive() { return g_active; }

void serviceToggle() {
  g_active = !g_active;
  if (g_active) {
    g_screen = 0;
    g_value[0] = g_value[1] = g_value[2] = g_value[3] = 0;
  }
}

// ---------------------------------------------------------------------------
// Ввод
// ---------------------------------------------------------------------------
void serviceTick(int32_t encDelta) {
  if (!g_active) return;

  // --- Сенсорные кнопки: переключение экранов ---
  static bool     lastLeft  = false;
  static bool     lastRight = false;
  static uint32_t lastChangeMs = 0;

  bool left  = (digitalRead(TOUCH_LEFT_PIN)  == HIGH);
  bool right = (digitalRead(TOUCH_RIGHT_PIN) == HIGH);
  uint32_t now = millis();

  if ((left != lastLeft || right != lastRight) &&
      (now - lastChangeMs) > ENCODER_DEBOUNCE_MS) {
    lastChangeMs = now;
    if (left  && !lastLeft) {
      g_screen = (uint8_t)((g_screen + SERVICE_SCREEN_COUNT - 1) % SERVICE_SCREEN_COUNT);
    }
    if (right && !lastRight) {
      g_screen = (uint8_t)((g_screen + 1) % SERVICE_SCREEN_COUNT);
    }
    lastLeft  = left;
    lastRight = right;
  }

  // --- Энкодер: изменение значения ---
  if (encDelta != 0) {
    int16_t max = screenMax();
    if (max < 0) max = 0;

    int32_t v = (int32_t)g_value[g_screen] + encDelta;
    if (v < 0) v = 0;
    if (v > max) v = max;
    g_value[g_screen] = (int16_t)v;

    // Сегмент не может выйти за размер выбранного кольца
    if (g_screen == 2) {
      int16_t segMax = (int16_t)diskRingSize((uint8_t)g_value[2]) - 1;
      if (segMax < 0) segMax = 0;
      if (g_value[3] > segMax) g_value[3] = segMax;
    }
  }
}

// ---------------------------------------------------------------------------
// Тестовый паттерн
// ---------------------------------------------------------------------------
void serviceRender() {
  ledsClear();

  switch (g_screen) {
    case 0: {
      // Весь столбец — вертикальная линия на цилиндре
      for (uint8_t y = 0; y < MATRIX_HEIGHT; y++) {
        sideSet((uint8_t)g_value[0], y, CRGB(0, 140, 255));
      }
      break;
    }
    case 1: {
      // Вся строка — ломаная линия из-за «змейки» (проверка чётности столбцов)
      for (uint8_t x = 0; x < MATRIX_WIDTH; x++) {
        sideSet(x, (uint8_t)g_value[1], CRGB(255, 70, 0));
      }
      break;
    }
    case 2: {
      // Кольцо целиком
      uint8_t ring = (uint8_t)g_value[2];
      uint8_t n = diskRingSize(ring);
      for (uint8_t s = 0; s < n; s++) diskSet(ring, s, CRGB(0, 255, 130));
      break;
    }
    default: {
      // Один сегмент кольца
      uint8_t ring = (uint8_t)g_value[2];
      diskSet(ring, (uint8_t)g_value[3], CRGB(255, 0, 255));
      break;
    }
  }

  FastLED.setBrightness(255);
  FastLED.show();
}

// ---------------------------------------------------------------------------
// OLED
// ---------------------------------------------------------------------------
void serviceDisplay() {
  static uint32_t lastMs = 0;
  uint32_t now = millis();
  if (now - lastMs < 100) return;
  lastMs = now;

  char line1[24];
  char line2[24];

  switch (g_screen) {
    case 0: snprintf(line1, sizeof(line1), "SIDE COL %d", (int)g_value[0]); break;
    case 1: snprintf(line1, sizeof(line1), "SIDE ROW %d", (int)g_value[1]); break;
    case 2: snprintf(line1, sizeof(line1), "DISK RING %d", (int)g_value[2]); break;
    default:
      snprintf(line1, sizeof(line1), "DISK R%d S%d", (int)g_value[2], (int)g_value[3]);
      break;
  }
  snprintf(line2, sizeof(line2), "MAX %d", (int)screenMax());

  displayService(line1, line2);
}