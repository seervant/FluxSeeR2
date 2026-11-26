#pragma once
// ============================================================================
// НАЗНАЧЕНИЕ ПИНОВ ESP32-S3 (ESP32-S3-DevKitC-1, Liludin)
// Версия железа 0.011.21
//
// Пины сверены с документацией Espressif:
//   - GPIO35/36/37 — заняты внутренней Octal PSRAM модуля N16R8, НЕ использовать
//   - GPIO19/20   — USB OTG (D-/D+), заняты вторым разъёмом Type-C
//   - GPIO43/44   — UART0, оставлены под отладочный Serial / прошивку
//   - GPIO38/48   — встроенный адресный RGB-LED платы (v1.1 -> 38, v1.0 -> 48)
//   - GPIO0/3/45/46 — strapping-пины (boot mode, JTAG, выбор VDD_SPI)
// ============================================================================

// --- Адресные светодиоды WS2812B (данные через 74AHCT125 -> 330 Ом) ---
// На ESP32-S3 ровно 4 TX-канала RMT (SOC_RMT_TX_CANDIDATES_PER_GROUP = 4),
// поэтому 4 линии данных — это потолок. Боковая матрица разбита на 3 панели
// 16 x 16, панели разъединяются (перемычки DOUT -> DIN снимаются), кольца
// диска остаются последовательными внутри одной ленты.
#define LED_PANEL1_DATA_PIN  21   // J3 #18 — панель 1 (столбцы 0..15)
#define LED_PANEL2_DATA_PIN  14   // J1 #20 — панель 2 (столбцы 16..31)
#define LED_PANEL3_DATA_PIN  15   // J1 #8  — панель 3 (столбцы 32..47)
#define LED_DISK_DATA_PIN    39   // J3 #9  — верхний диск, 8 колец (181 LED)
#define LED_LINE_COUNT         4   // 3 панели + диск

// --- I2C: одна шина на 4 устройства (OLED, MAX17043, MPU-6050, MPR121) ---
#define I2C_SDA_PIN          8   // J1 #12
#define I2C_SCL_PIN          9   // J1 #15
// 400 кГц: на одной плате обновление OLED не должно съедать кадр (24 fps).
// Требует подтяжек 2.2 кОм вместо штатных 4.7 кОм на модулях (см. HARDWARE.md).
#define I2C_FREQ          400000   // 400 кГц (быстрый режим I2C)

// --- SPI: карта памяти microSD ---
#define SPI_SCK_PIN         12   // J1 #18  (D13 шилда)
#define SPI_MOSI_PIN        11   // J1 #17  (D11)
#define SPI_MISO_PIN        13   // J1 #19  (D12)
#define SPI_CS_PIN          10   // J1 #16  (D10)

// --- Аналоговые входы (только ADC1: GPIO1..GPIO10) ---
#define MIC_PIN              1   // J3 #4  — MAX9814 Out
#define AUDIO_SPECTRUM_PIN   2   // J3 #5  — MSGEQ7 Out (через делитель 1:2)

// --- Анализатор спектра MSGEQ7 ---
#define MSGEQ7_STR_PIN       6   // J1 #6
#define MSGEQ7_RST_PIN       7   // J1 #7

// --- Аудио: I2S -> PCM5102A -> PAM8403 -> колонки ---
// MP3-TF-16P из проекта исключён: своя память не нужна, звук берём с microSD.
#define I2S_BCLK_PIN        16   // J1 #9  — PCM5102A BCLK
#define I2S_LRC_PIN         17   // J1 #10 — PCM5102A LRC
#define I2S_DOUT_PIN        18   // J1 #11 — PCM5102A DIN

// --- Энкодер Glover ---
#define ENC_A_PIN           40   // J3 #8
#define ENC_B_PIN           41   // J3 #7
#define ENC_SW_PIN          42   // J3 #6

// --- Сенсорные кнопки TTP223 ---
#define TOUCH_LEFT_PIN      47   // J3 #17 — предыдущий эффект
#define TOUCH_RIGHT_PIN      4   // J1 #4  — следующий эффект

// --- Усилитель PAM8403 ---
#define PAM8403_SD_PIN       5   // J1 #5 — низкий уровень = mute
