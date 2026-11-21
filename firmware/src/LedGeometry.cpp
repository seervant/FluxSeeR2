#include "LedGeometry.h"
#include "Pins.h"

// ============================================================================
// ГЕОМЕТРИЯ СВЕТОДИОДОВ — реализация
// ============================================================================

CRGB g_ledsSide[SIDE_LED_COUNT];
CRGB g_ledsDisk[DISK_LED_COUNT];

// Смещение начала каждого кольца в ленте диска (в ФИЗИЧЕСКОМ порядке)
static uint16_t g_diskRingOffset[DISK_RING_COUNT];

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
  // Боковая матрица и верхний диск — две отдельные ленты
  FastLED.addLeds<WS2812B, LED_SIDE_DATA_PIN, GRB>(g_ledsSide, SIDE_LED_COUNT);
  FastLED.addLeds<WS2812B, LED_DISK_DATA_PIN, GRB>(g_ledsDisk, DISK_LED_COUNT);

  // Ограничение по току FastLED — как дополнительный предохранитель.
  // Основное ограничение (общее на ОБЕ ленты) делает ledsApplyPowerLimit().
  FastLED.setMaxPowerInVoltsAndMilliamps(5.0f, LED_POWER_LIMIT_MA);
  FastLED.setBrightness(64);

  buildRingOffsets();
  ledsClear();
  FastLED.show();
}

void ledsClear() {
  fill_solid(g_ledsSide, SIDE_LED_COUNT, CRGB(0, 0, 0));
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

  for (uint16_t i = 0; i < SIDE_LED_COUNT; i++) {
    const CRGB& c = g_ledsSide[i];
    sum += (uint32_t)c.r + (uint32_t)c.g + (uint32_t)c.b;
  }
  for (uint16_t i = 0; i < DISK_LED_COUNT; i++) {
    const CRGB& c = g_ledsDisk[i];
    sum += (uint32_t)c.r + (uint32_t)c.g + (uint32_t)c.b;
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

  // Масштаб обеих лент одинаковый — иначе одна из поверхностей окажется
  // ярче другой при общей просадке питания.
  uint32_t scale = (uint32_t)budgetMilliAmps * 255 / current;
  if (scale > 255) scale = 255;
  if (scale == 0) scale = 1;

  for (uint16_t i = 0; i < SIDE_LED_COUNT; i++) {
    g_ledsSide[i].r = (uint8_t)(g_ledsSide[i].r * scale / 255);
    g_ledsSide[i].g = (uint8_t)(g_ledsSide[i].g * scale / 255);
    g_ledsSide[i].b = (uint8_t)(g_ledsSide[i].b * scale / 255);
  }
  for (uint16_t i = 0; i < DISK_LED_COUNT; i++) {
    g_ledsDisk[i].r = (uint8_t)(g_ledsDisk[i].r * scale / 255);
    g_ledsDisk[i].g = (uint8_t)(g_ledsDisk[i].g * scale / 255);
    g_ledsDisk[i].b = (uint8_t)(g_ledsDisk[i].b * scale / 255);
  }
}

// ---------------------------------------------------------------------------
// Боковая матрица
// ---------------------------------------------------------------------------
uint16_t sideIndex(uint8_t x, uint8_t y) {
  if (x >= MATRIX_WIDTH || y >= MATRIX_HEIGHT) return 0;

  uint8_t tx = FLIP_X ? (uint8_t)(MATRIX_WIDTH - 1 - x) : x;
  uint8_t ty = FLIP_Y ? (uint8_t)(MATRIX_HEIGHT - 1 - y) : y;

  // «Змейка»: чётные столбцы снизу вверх, нечётные сверху вниз
  return (tx % 2 == 1)
      ? (uint16_t)(tx * MATRIX_HEIGHT + (MATRIX_HEIGHT - 1 - ty))
      : (uint16_t)(tx * MATRIX_HEIGHT + ty);
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
  g_ledsSide[sideIndex(x, y)] = color;
}

void sideAdd(uint8_t x, uint8_t y, const CRGB& color) {
  if (x >= MATRIX_WIDTH || y >= MATRIX_HEIGHT) return;
  g_ledsSide[sideIndex(x, y)] += color;
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
  fill_solid(g_ledsSide, SIDE_LED_COUNT, color);
}

void diskFill(const CRGB& color) {
  fill_solid(g_ledsDisk, DISK_LED_COUNT, color);
}