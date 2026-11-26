#include "Orientation.h"
#include "Config.h"
#include "Sensors.h"
#include <Preferences.h>

// ============================================================================
// ОРИЕНТАЦИЯ ЛАМПЫ — реализация
// ============================================================================

Mat3 g_bodyAxesInMpu = Mat3::identity();
bool g_sceneCalibrated = false;

static Quat g_rel;             // корпус -> калибровочная система
static Preferences s_prefs;
static const char* SCENE_NS = "scene3d";

// Коэффициент коррекции по гравитации (доля ошибки, снимаемая за шаг)
static const float GRAV_CORRECT_GAIN = 0.035f;

// ---------------------------------------------------------------------------
// Переводы между системами координат
// ---------------------------------------------------------------------------
Vec3 mpuToBody(const Vec3& v) {
  return g_bodyAxesInMpu.transpose() * v;
}

Vec3 bodyToMpu(const Vec3& v) {
  return g_bodyAxesInMpu * v;
}

// ---------------------------------------------------------------------------
// Загрузка калибровки
// ---------------------------------------------------------------------------
void orientationInit() {
  s_prefs.begin(SCENE_NS, false);

  uint32_t magic = s_prefs.getUInt("magic", 0);
  if (magic == SCENE_CALIB_MAGIC) {
    float m[9];
    bool ok = true;
    for (uint8_t i = 0; i < 9; i++) {
      char key[6];
      snprintf(key, sizeof(key), "m%d", (int)i);
      m[i] = s_prefs.getFloat(key, 1.0f);
      if (!isfinite(m[i])) ok = false;
    }
    if (ok) {
      Mat3 mat;
      for (uint8_t i = 0; i < 9; i++) mat.m[i] = m[i];
      // Проверка базиса: оси должны быть ортонормированы
      Mat3 t = mat.transpose();
      float lx = (t * Vec3(1, 0, 0)).length();
      float ly = (t * Vec3(0, 1, 0)).length();
      float lz = (t * Vec3(0, 0, 1)).length();
      if (fabs(lx - 1.0f) < 0.05f && fabs(ly - 1.0f) < 0.05f && fabs(lz - 1.0f) < 0.05f) {
        g_bodyAxesInMpu = mat;
        g_sceneCalibrated = true;
      }
    }
  }

  s_prefs.end();
  g_rel = Quat();
}

// ---------------------------------------------------------------------------
// Применение и сохранение калибровки
// ---------------------------------------------------------------------------
void orientationSetCalibration(const Mat3& bodyAxesInMpu) {
  g_bodyAxesInMpu = bodyAxesInMpu;
  g_sceneCalibrated = true;
  g_rel = Quat();                 // новая опорная поза

  s_prefs.begin(SCENE_NS, false);
  for (uint8_t i = 0; i < 9; i++) {
    char key[6];
    snprintf(key, sizeof(key), "m%d", (int)i);
    s_prefs.putFloat(key, bodyAxesInMpu.m[i]);
  }
  s_prefs.putUInt("magic", SCENE_CALIB_MAGIC);
  s_prefs.end();
}

// ---------------------------------------------------------------------------
// Относительная ориентация
// ---------------------------------------------------------------------------
const Quat& orientationRel() { return g_rel; }

void orientationResetRel() { g_rel = Quat(); }

void orientationSetRel(const Quat& q) { g_rel = q.normalized(); }

// ---------------------------------------------------------------------------
// Шаг интегрирования
// ---------------------------------------------------------------------------
void orientationTick(uint32_t dtMs) {
  if (!g_sceneCalibrated) return;
  if (dtMs == 0) return;
  if (dtMs > 200) dtMs = 200;      // защита от «прыжка» после паузы

  float dt = (float)dtMs * 0.001f;

  // --- Гироскоп: из системы датчика в систему корпуса ---
  Vec3 wMpu(data.gyroX, data.gyroY, data.gyroZ);
  Vec3 wBody = mpuToBody(wMpu);

  // Интегрирование: v_cal = psi * v_body * psi^(-1)
  //   dpsi = psi * exp(w_body * dt)   (правое умножение)
  float wLen = wBody.length();
  if (wLen > 1e-6f) {
    Quat dq = Quat::fromAxisAngle(wBody, wLen * dt);
    g_rel = (g_rel * dq).normalized();
  }

  // --- Коррекция по гравитации (наклон) ---
  // Ожидаемое направление «вниз» в системе корпуса:
  //   в калибровочной системе вниз = (0, -1, 0)
  Vec3 gPred = g_rel.rotateInverse(Vec3(0.0f, -1.0f, 0.0f));
  // Измеренное направление «вниз» в системе корпуса:
  //   data.acc направлен вниз (знак инвертирован при чтении акселерометра)
  Vec3 gMeas = mpuToBody(Vec3(data.accX, data.accY, data.accZ));

  float mg = gMeas.length();
  if (mg > 0.5f) {                 // измерение осмысленно, только если модуль ~1
    gMeas = gMeas / mg;
    Vec3 err = gMeas.cross(gPred);
    float sinA = err.length();
    if (sinA > 1e-5f) {
      float angle = asinf(constrain(sinA, -1.0f, 1.0f));
      Vec3 axis = err / sinA;
      Quat corr = Quat::fromAxisAngle(axis, angle * GRAV_CORRECT_GAIN);
      g_rel = (g_rel * corr).normalized();
    }
  }
}
