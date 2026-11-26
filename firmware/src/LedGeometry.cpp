#include "LedGeometry.h"
#include "Pins.h"

// ============================================================================
// ГЕОМЕТРИЯ СВЕТОДИОДОВ — реализация
// ============================================================================

CRGB g_ledsPanel[PANEL_COUNT][PANEL_LED_COUNT];
CRGB g_ledsDisk[DISK_LED_COUNT];

// Смещение начала каждого кольца в ленте диска (в ФИЗИЧЕСКОМ порядке)
static uint16_t g_diskRingOffset[DISK_RING_COUNT];

// Общая таблица буферов — для лимитера тока. Все 4 лиенты масштабируются
// ОДИНАКОВЫМ коэффициентом, иначе панели и диск будут светиться по-разному.
struct LedBuffer {
  CRGB* data;
  uint16_t count;
};
static LedBuffer g_ledBuffers[PANEL_COUNT + 1];

// ---------------------------------------------------------------------------
// Служебное: физический номер кольца для логического
// ---------------------------------------------------------------------------
static inline uint8_t physRingOf(uint8_t ring) {
  return DISK_RING_REVERSED ? (uint8_t)(DISK_RING_COUNT - 1 - ring) : ring;
}

// ---------------------------------------------------------------------------
// Смещения колец: лента может быть прошита от внешнего кольца к центру
// ---------------------------------------------------------------------------
static void buildRingOffsets() {
  uint16_t off = 0;
  for (uint8_t i = 0; i < DISK_RING_COUNT; i++) {
    uint8_t phys = physRingOf(i);
    g_diskRingOffset[phys] = off;
    off += DISK_RING_SIZES[phys];
  }
}

// ---------------------------------------------------------------------------
// Инициализация
// ---------------------------------------------------------------------------
void ledsInit() {
  // Четыре линии данных: 3 панели боковой матрицы + диск. На ESP32-S3 всего
  // 4 TX-канала RMT (SOC_RMT_TX_CANDIDATES_PER_GROUP = 4) — больше нельзя.
  FastLED.addLeds<WS2812B, LED_PANEL1_DATA_PIN, GRB>(g_ledsPanel[0], PANEL_LED_COUNT);
  FastLED.addLeds<WS2812B, LED_PANEL2_DATA_PIN, GRB>(g_ledsPanel[1], PANEL_LED_COUNT);
  FastLED.addLeds<WS2812B, LED_PANEL3_DATA_PIN, GRB>(g_ledsPanel[2], PANEL_LED_COUNT);
  FastLED.addLeds<WS2812B, LED_DISK_DATA_PIN, GRB>(g_ledsDisk, DISK_LED_COUNT);

  // Ограничение по току считаем сами (ledsApplyPowerLimit) — единый бюджет на
  // все 4 лиенты. Собственный лимитер FastLED работает по каждому контроллеру
  // отдельно и при 4 лиентах дал бы разную яркость панелей, поэтому его
  // фактически отключаем (1 А — ограничение не срабатывает).
  FastLED.setMaxPowerInVoltsAndMilliamps(5.0f, 1000);
  FastLED.setBrightness(64);

  for (uint8_t p = 0; p < PANEL_COUNT; p++) {
    g_ledBuffers[p].data = g_ledsPanel[p];
    g_ledBuffers[p].count = PANEL_LED_COUNT;
  }
  g_ledBuffers[PANEL_COUNT].data = g_ledsDisk;
  g_ledBuffers[PANEL_COUNT].count = DISK_LED_COUNT;

  buildRingOffsets();
  ledsClear();
  FastLED.show();
}

void ledsClear() {
  for (uint8_t p = 0; p < PANEL_COUNT; p++) {
    fill_solid(g_ledsPanel[p], PANEL_LED_COUNT, CRGB(0, 0, 0));
  }
  fill_solid(g_ledsDisk, DISK_LED_COUNT, CRGB(0, 0, 0));
}

// ---------------------------------------------------------------------------
// Ограничение мощности
//
// 949 WS2812B в худшем случае (белый на полную) тянут 56.9 А, а преобразователь
// даёт 20 А. FastLED умеет ограничивать мощность, но его лимитер в FastLED 3.10
// считает бюджет по одному контроллеру (одной ленте). Наша лента — две,
// поэтому лимит считаем сами: общий ток обеих лент и масштабирование картинки.
//
// Оценка: WS2812B — источник тока на канал, до ~20 мА на канал при полной
// яркости. Сумма байт R+G+B (0..765) даёт долю каналов, умноженную на 20 мА.
// ---------------------------------------------------------------------------
uint32_t ledsEstimateMilliAmps(uint8_t globalBrightness) {
  uint32_t sum = 0;

  for (uint8_t b = 0; b < PANEL_COUNT + 1; b++) {
    const CRGB* buf = g_ledBuffers[b].data;
    for (uint16_t i = 0; i < g_ledBuffers[b].count; i++) {
      const CRGB& c = buf[i];
      sum += (uint32_t)c.r + (uint32_t)c.g + (uint32_t)c.b;
    }
  }

  uint32_t mA = sum * LED_CHANNEL_MAX_MA / 255;   // ток каналов
  mA = mA * globalBrightness / 255;               // с учётом глобальной яркости
  return mA;
}

void ledsApplyPowerLimit(uint32_t budgetMilliAmps) {
  if (budgetMilliAmps == 0) return;

  uint8_t brightness = FastLED.getBrightness();
  uint32_t current = ledsEstimateMilliAmps(brightness);
  if (current <= budgetMilliAmps) return;

  // Масштабируем ВСЕ лиенты одним и тем же коэффициентом — иначе одна из
  // поверхностей окажется ярче другой при общей просадке питания.
  uint32_t scale = (uint32_t)budgetMilliAmps * 255 / current;
  if (scale > 255) scale = 255;
  if (scale == 0) scale = 1;

  for (uint8_t b = 0; b < PANEL_COUNT + 1; b++) {
    CRGB* buf = g_ledBuffers[b].data;
    for (uint16_t i = 0; i < g_ledBuffers[b].count; i++) {
      buf[i].r = (uint8_t)(buf[i].r * scale / 255);
      buf[i].g = (uint8_t)(buf[i].g * scale / 255);
      buf[i].b = (uint8_t)(buf[i].b * scale / 255);
    }
  }
}

// ---------------------------------------------------------------------------
// Боковая матрица
// ---------------------------------------------------------------------------
// Индекс LED внутри панели по локальному столбцу и строке.
// «Змейка»: чётные столбцы снизу вверх, нечётные сверху вниз.
// Ширина панели (16) чётная, поэтому чётность локального столбца совпадает с
// чётностью глобального — раскладка не «поедет» на границах панелей.
static inline uint16_t panelLocalIndex(uint8_t localCol, uint8_t ty) {
  return (localCol % 2 == 1)
      ? (uint16_t)(localCol * PANEL_HEIGHT + (PANEL_HEIGHT - 1 - ty))
      : (uint16_t)(localCol * PANEL_HEIGHT + ty);
}

// Глобальный столбец -> номер панели и столбец внутри неё
static inline void sideSplit(uint8_t tx, uint8_t& panel, uint8_t& localCol) {
  panel = tx / PANEL_WIDTH;
  localCol = tx % PANEL_WIDTH;
}

uint16_t sideIndex(uint8_t x, uint8_t y) {
  if (x >= MATRIX_WIDTH || y >= MATRIX_HEIGHT) return 0;

  uint8_t tx = FLIP_X ? (uint8_t)(MATRIX_WIDTH - 1 - x) : x;
  uint8_t ty = FLIP_Y ? (uint8_t)(MATRIX_HEIGHT - 1 - y) : y;

  uint8_t panel, localCol;
  sideSplit(tx, panel, localCol);
  return (uint16_t)(panel * PANEL_LED_COUNT + panelLocalIndex(localCol, ty));
}

// Прямая ссылка на пиксель — без промежуточного индекса
static inline CRGB& sidePixel(uint8_t x, uint8_t y) {
  uint8_t tx = FLIP_X ? (uint8_t)(MATRIX_WIDTH - 1 - x) : x;
  uint8_t ty = FLIP_Y ? (uint8_t)(MATRIX_HEIGHT - 1 - y) : y;

  uint8_t panel, localCol;
  sideSplit(tx, panel, localCol);
  return g_ledsPanel[panel][panelLocalIndex(localCol, ty)];
}

float sideAngle(uint8_t x) {
  return (float)x * (2.0f * PI / (float)MATRIX_WIDTH);
}

// ---------------------------------------------------------------------------
// Верхний диск
// ---------------------------------------------------------------------------
uint8_t diskRingSize(uint8_t ring) {
  if (ring >= DISK_RING_COUNT) return 0;
  return DISK_RING_SIZES[ring];
}

float diskRingRadius(uint8_t ring) {
  if (ring >= DISK_RING_COUNT) return 0.0f;
  return (float)ring / (float)(DISK_RING_COUNT - 1);
}

uint16_t diskIndex(uint8_t ring, uint8_t seg) {
  if (ring >= DISK_RING_COUNT) return 0;

  uint8_t phys = physRingOf(ring);
  uint8_t cnt = DISK_RING_SIZES[phys];
  if (seg >= cnt) seg = 0;

  // Направление обхода сегментов внутри кольца
  uint8_t physSeg = DISK_SEG_REVERSED ? (uint8_t)(cnt - 1 - seg) : seg;

  return (uint16_t)(g_diskRingOffset[phys] + physSeg);
}

// ---------------------------------------------------------------------------
// Запись цветов
// ---------------------------------------------------------------------------
void sideSet(uint8_t x, uint8_t y, const CRGB& color) {
  if (x >= MATRIX_WIDTH || y >= MATRIX_HEIGHT) return;
  sidePixel(x, y) = color;
}

void sideAdd(uint8_t x, uint8_t y, const CRGB& color) {
  if (x >= MATRIX_WIDTH || y >= MATRIX_HEIGHT) return;
  sidePixel(x, y) += color;
}

void diskSet(uint8_t ring, uint8_t seg, const CRGB& color) {
  if (ring >= DISK_RING_COUNT) return;
  g_ledsDisk[diskIndex(ring, seg)] = color;
}

void diskAdd(uint8_t ring, uint8_t seg, const CRGB& color) {
  if (ring >= DISK_RING_COUNT) return;
  g_ledsDisk[diskIndex(ring, seg)] += color;
}

void sideFill(const CRGB& color) {
  for (uint8_t p = 0; p < PANEL_COUNT; p++) {
    fill_solid(g_ledsPanel[p], PANEL_LED_COUNT, color);
  }
}

void diskFill(const CRGB& color) {
  fill_solid(g_ledsDisk, DISK_LED_COUNT, color);
}