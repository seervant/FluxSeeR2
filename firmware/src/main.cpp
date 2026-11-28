#include <Arduino.h>
#include <Wire.h>
#include "Config.h"
#include "Pins.h"
#include "Types.h"
#include "Params.h"
#include "LedGeometry.h"
#include "Sensors.h"
#include "Controls.h"
#include "Display.h"
#include "Effects.h"
#include "Settings.h"
#include "ServiceMode.h"
#include "Orientation.h"
#include "Model3D.h"

// ============================================================================
// FleXSeeR2 — точка входа
// Версия прошивки 0.011.22.2 (железо 0.011.21)
// Архитектура: ОДНА плата ESP32-S3-N16R8 (4 линии данных + OLED + датчики)
// ============================================================================

void setup() {
  // --- Отладочный вывод через нативный USB ---
  Serial.begin(115200);

  // --- Ввод/вывод ---
  controlsInit();

  // --- Шина I2C: OLED + MAX17043 + MPU-6050 (+ MPR121 в резерве) ---
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, I2C_FREQ);

  // --- Дисплей (не блокирует работу лампы при отсутствии экрана) ---
  displayInit();

  // --- Датчики ---
  sensorsInit();

  // --- Светодиоды: 4 линии данных (3 панели 16x16 + диск) ---
  ledsInit();

  // --- Реестр параметров и сохранённые настройки ---
  paramsInit();
  settingsLoad();

  // --- Сервисный режим: вход по зажатой кнопке энкодера при включении ---
  serviceInit();

  // --- Ориентация лампы: загрузка калибровки 3D-сцены из NVS ---
  orientationInit();

  // --- SD-карта: список 3D-моделей для эффектов 8 и 9 ---
  model3dInit();

  // --- Фиксация начальной ориентации (для эффекта «Жидкость») ---
  calibrateGravityStartup();

  Serial.printf("[FleXSeeR] FW %s / HW %s | display=%d | service=%d\n",
                FIRMWARE_VERSION, HARDWARE_VERSION,
                displayReady() ? 1 : 0, serviceActive() ? 1 : 0);
}

void loop() {
  // 1. Датчики (MPU-6050 и батарея — с разной частотой, внутри sensorsTick)
  //    Ориентация корпуса обновляется там же, с частотой гироскопа
  sensorsTick();

  // 2. Ввод: энкодер, сенсорные кнопки
  controlsTick();

  if (serviceActive()) {
    // 3a. Сервисный режим: тестовый паттерн вместо эффекта
    serviceRender();
    serviceDisplay();
  } else {
    // 3b. Кадр эффекта -> ленты
    effectsRender();

    // 4. Интерфейс на OLED — не чаще ~8 раз в секунду
    displayTick(DISPLAY_REFRESH_MS);
  }

  // 5. Сохранение настроек в NVS (не чаще раза в 2 с)
  static uint32_t lastSaveMs = 0;
  if (controlsIsDirty() && (millis() - lastSaveMs) > 2000) {
    lastSaveMs = millis();
    settingsSave();
  }
}