#include "Controls.h"
#include "Config.h"
#include "Pins.h"
#include "Params.h"
#include "Sensors.h"
#include "ServiceMode.h"

// ============================================================================
// УПРАВЛЕНИЕ — реализация
// ============================================================================

static uint8_t g_currentEffect = EFFECT_STATIC;
static uint8_t g_currentParam  = 0;
static bool     g_dirty         = false;

// Накопленная дельта вращения энкодера (читается и обнуляется в основном цикле)
static volatile int32_t g_encDelta = 0;

// Вспомогательные обработчики (определены ниже)
static void handleEncoderButton();
static void handleTouchButtons();

// ---------------------------------------------------------------------------
// Инициализация
// ---------------------------------------------------------------------------
void controlsInit() {
  // Энкодер: механические контакты -> подтяжка к питанию
  pinMode(ENC_A_PIN,  INPUT_PULLUP);
  pinMode(ENC_B_PIN,  INPUT_PULLUP);
  pinMode(ENC_SW_PIN, INPUT_PULLUP);

  // TTP223: выход push-pull, при касании = HIGH
  pinMode(TOUCH_LEFT_PIN,  INPUT);
  pinMode(TOUCH_RIGHT_PIN, INPUT);

  attachInterrupt(digitalPinToInterrupt(ENC_A_PIN), handleEncoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_B_PIN), handleEncoderISR, CHANGE);
}

// ---------------------------------------------------------------------------
// Прерывание энкодера
// ---------------------------------------------------------------------------
void IRAM_ATTR handleEncoderISR() {
  static uint8_t oldState = 0;
  static int8_t  subPos   = 0;
  // Таблица переходов квадратурного счётчика (код Грея)
  static const int8_t encTable[16] = {
     0, -1,  1,  0,  1,  0,  0, -1,
    -1,  0,  0,  1,  0,  1, -1,  0
  };

  uint8_t currentState = (uint8_t)((digitalRead(ENC_A_PIN) << 1) | digitalRead(ENC_B_PIN));
  oldState = (uint8_t)(((oldState << 2) | currentState) & 0x0F);
  subPos  += encTable[oldState];

  // Один щелчок = 4 изменения квадратурного сигнала
  if (subPos >= 4)       { g_encDelta++; subPos = 0; }
  else if (subPos <= -4) { g_encDelta--; subPos = 0; }
}

// ---------------------------------------------------------------------------
// Основной опрос
// ---------------------------------------------------------------------------
void controlsTick() {
  // --- 1. Вращение энкодера ---
  int32_t delta = 0;
  noInterrupts();
  delta = g_encDelta;
  g_encDelta = 0;
  interrupts();

  // --- 2. Кнопка энкодера: всегда (в т.ч. удержание -> сервисный режим) ---
  handleEncoderButton();

  // --- 3. В сервисном режиме остальной ввод не обрабатывается ---
  if (serviceActive()) {
    serviceTick(delta);
    return;
  }

  if (delta != 0) {
    paramsAdjust(g_currentEffect, g_currentParam, delta);
    g_dirty = true;
  }

  // --- 4. Сенсорные кнопки: предыдущий / следующий эффект ---
  handleTouchButtons();
}

// ---------------------------------------------------------------------------
// Кнопка энкодера
//
// Короткое нажатие — следующий параметр (или сервисный сброс MAX17043
// при пяти нажатиях). Удержание 2 секунды — вход/выход из сервисного режима.
// ---------------------------------------------------------------------------
static void handleEncoderButton() {
  static bool     lastState    = true;    // разомкнуто = HIGH
  static uint32_t lastChangeMs = 0;
  static uint32_t lastClickMs  = 0;
  static uint32_t pressStartMs = 0;
  static uint8_t  clickCount   = 0;

  bool raw = (digitalRead(ENC_SW_PIN) == LOW);   // нажата
  uint32_t now = millis();

  if (raw != lastState && (now - lastChangeMs) > ENCODER_DEBOUNCE_MS) {
    lastChangeMs = now;
    lastState = raw;

    if (raw) {
      pressStartMs = now;               // начало нажатия
    } else {
      uint32_t duration = now - pressStartMs;
      if (duration >= SERVICE_LONG_PRESS_MS) {
        serviceToggle();                // удержание -> сервисный режим
        clickCount = 0;
      } else {
        clickCount++;
        lastClickMs = now;
      }
    }
  }

  if (clickCount > 0 && (now - lastClickMs) > ENCODER_CLICK_WINDOW_MS) {
    if (serviceActive()) {
      // В сервисном режиме одиночное нажатие — подтверждение действия
      if (clickCount == 1) serviceOnClick();
    } else {
      if (clickCount == 1) {
        controlsNextParam();
      } else if (clickCount >= 5) {
        resetMAX17043();                // сервисный сброс контроллера заряда
      }
    }
    clickCount = 0;
  }
}

// ---------------------------------------------------------------------------
// Сенсорные кнопки TTP223
// ---------------------------------------------------------------------------
static void handleTouchButtons() {
  static bool     lastLeft  = false;
  static bool     lastRight = false;
  static uint32_t lastChangeMs = 0;
  uint32_t now = millis();

  bool left  = (digitalRead(TOUCH_LEFT_PIN)  == HIGH);
  bool right = (digitalRead(TOUCH_RIGHT_PIN) == HIGH);

  bool changed = (left != lastLeft) || (right != lastRight);
  if (changed && (now - lastChangeMs) > ENCODER_DEBOUNCE_MS) {
    lastChangeMs = now;
    if (left  && !lastLeft)  controlsPrevEffect();
    if (right && !lastRight) controlsNextEffect();
    lastLeft  = left;
    lastRight = right;
  }
}

// ---------------------------------------------------------------------------
// Состояние
// ---------------------------------------------------------------------------
uint8_t controlsCurrentEffect() { return g_currentEffect; }
uint8_t controlsCurrentParam()  { return g_currentParam; }
bool    controlsIsDirty()        { return g_dirty; }
void    controlsClearDirty()     { g_dirty = false; }

void controlsSetEffect(uint8_t effect) {
  if (effect >= EFFECT_COUNT) return;
  g_currentEffect = effect;
  g_currentParam  = 0;
  g_dirty = true;
}

void controlsNextEffect() {
  g_currentEffect = (uint8_t)((g_currentEffect + 1) % EFFECT_COUNT);
  g_currentParam  = 0;
  g_dirty = true;
}

void controlsPrevEffect() {
  g_currentEffect = (uint8_t)((g_currentEffect + EFFECT_COUNT - 1) % EFFECT_COUNT);
  g_currentParam  = 0;
  g_dirty = true;
}

void controlsNextParam() {
  uint8_t n = paramsCount(g_currentEffect);
  if (n == 0) return;
  g_currentParam = (uint8_t)((g_currentParam + 1) % n);
  g_dirty = true;
}

void controlsPrevParam() {
  uint8_t n = paramsCount(g_currentEffect);
  if (n == 0) return;
  g_currentParam = (uint8_t)((g_currentParam + n - 1) % n);
  g_dirty = true;
}