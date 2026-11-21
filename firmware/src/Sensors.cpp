#include "Sensors.h"
#include "Pins.h"

// ============================================================================
// ДАТЧИКИ — реализация
// ============================================================================

Telemetry data;

// Калибровка нуля гравитации при старте
bool  g_isGravityCalibrated = false;
float g_fixedAccX = 0.0f;
float g_fixedAccY = 0.0f;
float g_fixedAccZ = -1.0f;

// ---------------------------------------------------------------------------
// Инициализация
// ---------------------------------------------------------------------------
void sensorsInit() {
  initMAX17043();
  initMPU6050();
}

// ---------------------------------------------------------------------------
// Периодический опрос
// ---------------------------------------------------------------------------
void sensorsTick() {
  static uint32_t lastMpuMs = 0;
  static uint32_t lastBattMs = 0;
  uint32_t now = millis();

  if (now - lastMpuMs >= MPU_POLL_MS) {
    lastMpuMs = now;
    readMPU6050();
  }

  if (now - lastBattMs >= SENSOR_POLL_MS) {
    lastBattMs = now;
    readBattery();
  }
}

// ---------------------------------------------------------------------------
// MPU-6050
// ---------------------------------------------------------------------------

// Вывод микросхемы из спящего режима: регистр PWR_MGMT_1 (0x6B) = 0
void initMPU6050() {
  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(0x6B);
  Wire.write(0x00);
  Wire.endTransmission(true);
}

// Чтение сырых ускорений и нормализация вектора
void readMPU6050() {
  static float prevAccelTotal = 1.0f;

  Wire.beginTransmission(MPU6050_ADDR);
  Wire.write(0x3B);                      // ACCEL_XOUT_H
  Wire.endTransmission(false);

  if (Wire.requestFrom((uint8_t)MPU6050_ADDR, (uint8_t)6, (uint8_t)true) < 6) {
    return;                             // микросхема не ответила — продолжаем работу
  }

  int16_t rawAX = (int16_t)((Wire.read() << 8) | Wire.read());
  int16_t rawAY = (int16_t)((Wire.read() << 8) | Wire.read());
  int16_t rawAZ = (int16_t)((Wire.read() << 8) | Wire.read());

  // Диапазон ±2 g => 16384 LSB/g
  float ax = rawAX / 16384.0f;
  float ay = rawAY / 16384.0f;
  float az = rawAZ / 16384.0f;

  float norm = sqrt(ax * ax + ay * ay + az * az);
  if (norm > 0.001f) {
    data.accX = -ax / norm;
    data.accY = -ay / norm;
    data.accZ = -az / norm;
  }

  // Детектор встряхивания (резкий перепад модуля ускорения)
  data.isShaken = (fabs(norm - prevAccelTotal) > 0.60f);
  prevAccelTotal = norm;
}

// Фиксация базовой ориентации (спустя 1 с после старта)
void calibrateGravityStartup() {
  if (!g_isGravityCalibrated && millis() > 1000) {
    readMPU6050();
    g_fixedAccX = data.accX;
    g_fixedAccY = data.accY;
    g_fixedAccZ = data.accZ;
    g_isGravityCalibrated = true;
    data.gravityCalibrated = true;
  }
}

// ---------------------------------------------------------------------------
// MAX17043 — контроллер заряда
//
// ВАЖНО: штатный регистр SOC чипа рассчитан на ОДНУ ячейку.
// У нас пакет 4S, поэтому:
//   - напряжение снимается через делитель 1:4 (три 10 кОм последовательно + 10 кОм)
//   - процент заряда рассчитывается в софте по напряжению пакета
// Для правильного SOC на 4S нужен MAX17048 (многоячеечный, адрес I2C тот же 0x36).
// ---------------------------------------------------------------------------

void initMAX17043() {
  resetMAX17043();
}

void resetMAX17043() {
  Wire.beginTransmission(MAX17043_ADDR);
  Wire.write(0x06);    // регистр COMMAND
  Wire.write(0x40);    // 0x4000 -> Power-On Reset
  Wire.write(0x00);
  Wire.endTransmission();
  data.maxResetTriggered = true;
}

void readBattery() {
  static uint8_t errorCount = 0;

  data.prevVoltage = data.voltage;

  // --- Чтение VCELL (регистр 0x02) ---
  Wire.beginTransmission(MAX17043_ADDR);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) {
    if (++errorCount > 10) data.i2cErrorMAX = true;
    return;
  }
  data.i2cErrorMAX = false;

  float rawVolts = 0.0f;
  if (Wire.requestFrom((uint8_t)MAX17043_ADDR, (uint8_t)2, (uint8_t)true) == 2) {
    uint16_t raw = (uint16_t)((Wire.read() << 8) | Wire.read());
    raw &= 0x0FFF;                      // 12-битное значение
    rawVolts = raw * 0.00125f;          // 1.25 мВ на единицу
  }

  // Напряжение пакета = напряжение на пине чипа * коэффициент делителя
  float packVolts = rawVolts * BAT_DIVIDER_RATIO;

  // --- Чтение SOC (регистр 0x04) ---
  Wire.beginTransmission(MAX17043_ADDR);
  Wire.write(0x04);
  Wire.endTransmission(false);

  uint8_t rawSoc = 0;
  if (Wire.requestFrom((uint8_t)MAX17043_ADDR, (uint8_t)2, (uint8_t)true) == 2) {
    rawSoc = constrain(Wire.read(), 0, 100);
  }

  // --- Проверка правдоподобности напряжения пакета 4S ---
  if (packVolts >= (BAT_PACK_EMPTY_V - 1.0f) && packVolts <= (BAT_PACK_FULL_V + 0.5f)) {
    errorCount = 0;
    data.voltage = packVolts;
    data.batteryConnected = true;

    // Для 4S SOC-регистр чипа непригоден -> считаем по напряжению пакета
    float socByVoltage = mapf(packVolts, BAT_PACK_EMPTY_V, BAT_PACK_FULL_V, 0.0f, 100.0f);
    if (rawSoc > 0 && rawSoc <= 100 && fabs(socByVoltage - (float)rawSoc) < 35.0f) {
      data.battery = rawSoc;            // доверяем чипу, если оценки близки
    } else {
      data.battery = (uint8_t)constrain(socByVoltage, 0.0f, 100.0f);
    }

    // Зарядка: напряжение выше 4.125 В на ячейку либо резкий рост
    data.isCharging = (packVolts > (BAT_CELLS * 4.125f) ||
                       (packVolts - data.prevVoltage) > 0.01f);

    // Критический разряд: ниже 3.3 В на ячейку или меньше 10 %
    data.criticalLow = (data.battery <= 10 ||
                        packVolts < (BAT_CELLS * 3.30f));

    // Сглаживание показателя заряда (EMA) для плавной анимации уровня
    if (data.smoothBattery == 0.0f) {
      data.smoothBattery = (float)data.battery;
    } else {
      data.smoothBattery += ((float)data.battery - data.smoothBattery) * 0.1f;
    }
  } else {
    if (++errorCount > 10) {
      data.batteryConnected = false;
      data.isCharging = false;
    }
  }
}
