#include "Display.h"
#include "Config.h"
#include "Controls.h"
#include "Effects.h"
#include "Params.h"
#include "Sensors.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>

// ============================================================================
// ДИСПЛЕЙ — реализация
// ============================================================================

static Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
static bool g_displayReady = false;

// ---------------------------------------------------------------------------
// Постраничное обновление
//
// На одной плате обновление экрана не должно съедать кадр (24 fps). Полная
// перерисовка — это 1024 байта по I2C: на 100 кГц получается ~92 мс блокировки,
// то есть почти 77 % времени кадра. Поэтому шлём в шину только изменившиеся
// страницы (по 128 байт), а если ничего не изменилось — не трогаем I2C вообще.
//
// SSD1306: 8 страниц по 128 байт. Команды: 0xB0|p — страница,
// 0x00 — младший байт столбца, 0x10 — старший байт столбца,
// далее 128 байт данных страницы (в режиме page addressing это ровно страница).
// ---------------------------------------------------------------------------
static uint8_t g_shadow[SCREEN_WIDTH * SCREEN_HEIGHT / 8];   // что уже на экране
static bool g_shadowValid = false;

static void displaySendPage(uint8_t page, const uint8_t* data) {
  Wire.beginTransmission(OLED_ADDR);
  Wire.write((uint8_t)0x00);             // Co=0, D/C=0 -> одиночная команда
  Wire.write((uint8_t)(0xB0 | page));    // адрес страницы
  Wire.write((uint8_t)0x00);             // младший байт столбца = 0
  Wire.write((uint8_t)0x10);             // старший байт столбца = 0
  Wire.write(data, SCREEN_WIDTH);        // 128 байт данных страницы
  Wire.endTransmission();
}

static void displayFlush() {
  if (!g_displayReady) return;

  const uint8_t* buf = display.getBuffer();
  uint8_t firstPage = 0xFF;
  uint8_t lastPage = 0;

  for (uint8_t page = 0; page < (SCREEN_HEIGHT / 8); page++) {
    bool dirty = !g_shadowValid;
    if (!dirty) {
      const uint8_t* pageBuf = buf + (uint16_t)page * SCREEN_WIDTH;
      const uint8_t* shadowPage = g_shadow + (uint16_t)page * SCREEN_WIDTH;
      for (uint16_t col = 0; col < SCREEN_WIDTH; col++) {
        if (pageBuf[col] != shadowPage[col]) { dirty = true; break; }
      }
    }
    if (dirty) {
      if (page < firstPage) firstPage = page;
      lastPage = page;
    }
  }

  if (firstPage == 0xFF) return;   // экран не изменился — шину не трогаем

  for (uint8_t page = firstPage; page <= lastPage; page++) {
    uint16_t offset = (uint16_t)page * SCREEN_WIDTH;
    displaySendPage(page, buf + offset);
    memcpy(g_shadow + offset, buf + offset, SCREEN_WIDTH);
  }
  g_shadowValid = true;
}

// ---------------------------------------------------------------------------
// Инициализация: три попытки, при неудаче работаем без экрана
// ---------------------------------------------------------------------------
void displayInit() {
  for (int attempt = 0; attempt < 3; attempt++) {
    if (display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
      g_displayReady = true;
      display.clearDisplay();
      display.display();
      memset(g_shadow, 0, sizeof(g_shadow));
      g_shadowValid = true;
      return;
    }
    delay(50);
  }
  g_displayReady = false;   // дальше работаем без экрана, лампа не «висит»
}

bool displayReady() { return g_displayReady; }

// ---------------------------------------------------------------------------
// Периодическое обновление
// ---------------------------------------------------------------------------
void displayTick(uint32_t intervalMs) {
  static uint32_t lastMs = 0;
  uint32_t now = millis();
  if (now - lastMs < intervalMs) return;
  lastMs = now;
  displayRefresh();
}

// ---------------------------------------------------------------------------
// Отрисовка интерфейса
//
// Раскладка 128x64:
//   y=0   версия прошивки + статус питания
//   y=9   разделитель
//   y=12  напряжение / заряд аккумулятора
//   y=21  разделитель
//   y=24  всего/текущий эффект + название
//   y=34  всего/текущий параметр + название + значение
//   y=45  ползунок значения параметра
//   y=54  подсказка по управлению
// ---------------------------------------------------------------------------
void displayRefresh() {
  if (!g_displayReady) return;

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  uint8_t eff = controlsCurrentEffect();
  uint8_t par = controlsCurrentParam();

  // --- Строка 1: версия + статус ---
  display.setCursor(0, 0);
  display.print("FX ");
  display.print(FIRMWARE_VERSION);

  display.setCursor(90, 0);
  if (data.i2cErrorMAX)              display.print("MAXERR");
  else if (!data.batteryConnected)  display.print("NOBAT");
  else if (data.criticalLow)        display.print("LOW!");
  else if (data.isCharging)         display.print("CHRG");
  else                              display.print("BAT");
  display.drawFastHLine(0, 9, 128, SSD1306_WHITE);

  // --- Строка 2: напряжение и заряд пакета ---
  display.setCursor(0, 12);
  if (!data.batteryConnected || data.i2cErrorMAX) {
    display.print("BATTERY OFFLINE");
  } else {
    uint16_t mv = (uint16_t)(data.voltage * 1000.0f + 0.5f);
    display.printf("V:%u.%02uV %3d%%", (unsigned)(mv / 1000), (unsigned)((mv % 1000) / 10),
                   (int)data.battery);
  }
  display.drawFastHLine(0, 21, 128, SSD1306_WHITE);

  // --- Строка 3: всего/текущий эффект + название ---
  display.setCursor(0, 24);
  display.printf("%d/%d ", (int)EFFECT_COUNT, (int)(eff + 1));
  display.print(g_effects[eff].name);
  if (!g_effects[eff].implemented) display.print("*");

  // --- Строка 4: всего/текущий параметр + название + значение ---
  display.setCursor(0, 34);
  uint8_t parCount = paramsCount(eff);
  if (par >= parCount) par = (uint8_t)(parCount ? parCount - 1 : 0);
  if (parCount == 0) {
    display.print("NO PARAMS");
  } else {
    display.printf("%d/%d ", (int)parCount, (int)(par + 1));
    display.print(paramsSpec(eff)[par].name);

    const char* modeText = paramsDisplayText(eff, par);
    if (modeText[0] != '\0') {
      display.print(" ");
      display.print(modeText);
    } else {
      display.printf(" %d", (int)paramsDisplayValue(eff, par));
    }
  }

  // --- Строка 5: ползунок значения параметра ---
  uint8_t fraction = paramsSliderFraction(eff, par);
  uint8_t barW = (uint8_t)map(fraction, 0, 255, 0, 128);
  display.fillRect(0, 45, barW, 5, SSD1306_WHITE);
  display.drawRect(0, 45, 128, 5, SSD1306_WHITE);

  // --- Строка 6: подсказка ---
  display.setCursor(0, 54);
  display.print("L:R FX  ENC:PARAM");

  displayFlush();
}

// ---------------------------------------------------------------------------
// Служебный экран (режим проверки раскладки)
// ---------------------------------------------------------------------------
void displayService(const char* line1, const char* line2) {
  if (!g_displayReady) return;

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.print("SERVICE MODE");
  display.drawFastHLine(0, 9, 128, SSD1306_WHITE);

  display.setCursor(0, 14);
  display.print(line1);

  display.setCursor(0, 26);
  display.print(line2);

  display.setCursor(0, 44);
  display.print("ENC:VALUE  L/R:SCR");

  display.setCursor(0, 54);
  display.print("HOLD ENC:BACK");

  displayFlush();
}