#pragma once
#include <Arduino.h>
#include "Types.h"

// ============================================================================
// УПРАВЛЕНИЕ: ЭНКОДЕР + ДВЕ СЕНСОРНЫЕ КНОПКИ TTP223
//
//   Сенсор TTP223 L (левая)  — одинарное нажатие: предыдущий эффект
//   Сенсор TTP223 R (правая) — одинарное нажатие: следующий эффект
//   Энкодер: кнопка      — следующий параметр текущего эффекта
//           вращение     — изменение значения текущего параметра
//   5+ нажатий кнопки энкодера — сервисный сброс MAX17043
// ============================================================================

void controlsInit();

// Основной опрос ввода (вызывается каждый кадр)
void controlsTick();

// Обработчик прерывания энкодера (квадратурный счётчик с таблицей Грея)
void IRAM_ATTR handleEncoderISR();

// --- Текущее состояние ---
uint8_t controlsCurrentEffect();
uint8_t controlsCurrentParam();

void controlsSetEffect(uint8_t effect);
void controlsNextEffect();
void controlsPrevEffect();

void controlsNextParam();
void controlsPrevParam();

// Флаг «настройки изменились» — для сохранения в NVS
bool controlsIsDirty();
void controlsClearDirty();
