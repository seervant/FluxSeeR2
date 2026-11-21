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
#define LED_SIDE_DATA_PIN   21   // J3 #18 — боковая матрица 48 x 16 (768 LED)
#define LED_DISK_DATA_PIN   39   // J3 #9  — верхний диск, 8 колец (181 LED)

// --- I2C: одна шина на 4 устройства (OLED, MAX17043, MPU-6050, MPR121) ---
#define I2C_SDA_PIN          8   // J1 #12
#define I2C_SCL_PIN          9   // J1 #15
#define I2C_FREQ          100000   // 100 кГц — запас по помехам от 949 LED

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

// --- MP3-плеер MP3-TF-16P ---
#define MP3_BUSY_PIN        14   // J1 #20
#define MP3_IO1_PIN         15   // J1 #8
#define MP3_IO2_PIN         16   // J1 #9
#define MP3_UART_TX_PIN     17   // J1 #10 — TX ESP32 -> RX модуля
#define MP3_UART_RX_PIN     18   // J1 #11 — TX модуля -> RX ESP32
#define MP3_UART_BAUD      9600   // штатная скорость модуля

// --- Энкодер Glover ---
#define ENC_A_PIN           40   // J3 #8
#define ENC_B_PIN           41   // J3 #7
#define ENC_SW_PIN          42   // J3 #6

// --- Сенсорные кнопки TTP223 ---
#define TOUCH_LEFT_PIN      47   // J3 #17 — предыдущий эффект
#define TOUCH_RIGHT_PIN      4   // J1 #4  — следующий эффект

// --- Усилитель PAM8403 ---
#define PAM8403_SD_PIN       5   // J1 #5 — низкий уровень = mute
