#pragma once
#include <Arduino.h>
#include <Wire.h>
#include "Config.h"
#include "Types.h"

// ============================================================================
// ДАТЧИКИ
// ============================================================================

// Глобальная телеметрия
extern Telemetry data;

// Инициализация I2C-устройств (не блокирует, ошибки не фатальны)
void sensorsInit();

// Периодический опрос: MPU-6050 и MAX17043
void sensorsTick();

// --- MPU-6050 ---
void initMPU6050();
void readMPU6050();

// Фиксация начальной ориентации (базовой гравитации) — вызывается один раз
void calibrateGravityStartup();

// --- MAX17043 ---
void initMAX17043();
void readBattery();

// Программный сброс MAX17043 (команда 0x4000 в регистр 0x06)
void resetMAX17043();
