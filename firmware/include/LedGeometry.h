#pragma once
#include <Arduino.h>
#include <FastLED.h>
#include "Config.h"

// ============================================================================
// ГЕОМЕТРИЯ СВЕТОДИОДОВ
//
// Две независимые адресные ленты:
//   1) Боковая матрица 48 x 16 (768 LED), соединение «змейкой» по цилиндру
//   2) Верхний диск — 8 концентрических колец (181 LED)
//
// Модуль даёт единый способ обращаться к пикселям через (x, y) и (кольцо, сегмент),
// скрывая физическую раскладку лент.
// ============================================================================

extern CRGB g_ledsSide[SIDE_LED_COUNT];   // боковая матрица
extern CRGB g_ledsDisk[DISK_LED_COUNT];   // верхний диск

// Инициализация лент FastLED и очистка буферов
void ledsInit();

// Очистка обоих буферов
void ledsClear();

// --- Ограничение мощности ---
// Оценка суммарного тока обеих лент при текущей картинке и глобальной яркости
uint32_t ledsEstimateMilliAmps(uint8_t globalBrightness);

// Масштабирование обеих лент так, чтобы ток не превышал бюджет.
// Вызывать после заполнения буферов и до FastLED.show().
void ledsApplyPowerLimit(uint32_t budgetMilliAmps);

// --- Боковая матрица ---
// Индекс LED в ленте по координатам (x — столбец 0..47, y — строка 0..15)
uint16_t sideIndex(uint8_t x, uint8_t y);

// Угловая координата столбца, радианы (0..2*PI)
float sideAngle(uint8_t x);

// --- Верхний диск ---
// Количество сегментов в кольце (кольцо 0 — центр, 7 — внешнее)
uint8_t diskRingSize(uint8_t ring);

// Нормированный радиус кольца 0..1 (центр -> край)
float diskRingRadius(uint8_t ring);

// Индекс LED в ленте по (кольцо, сегмент)
uint16_t diskIndex(uint8_t ring, uint8_t seg);

// --- Запись цветов ---
void sideSet(uint8_t x, uint8_t y, const CRGB& color);
void sideAdd(uint8_t x, uint8_t y, const CRGB& color);
void diskSet(uint8_t ring, uint8_t seg, const CRGB& color);
void diskAdd(uint8_t ring, uint8_t seg, const CRGB& color);
void sideFill(const CRGB& color);
void diskFill(const CRGB& color);