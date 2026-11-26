#include "Calib3D.h"
#include "Config.h"
#include "Display.h"
#include "LedGeometry.h"
#include "Orientation.h"
#include "Scene3D.h"
#include "Sensors.h"

// ============================================================================
// МАСТЕР КАЛИБРОВКИ 3D-СЦЕНЫ — реализация
// ============================================================================

static uint8_t g_step = CALIB3D_STEP_IDLE;
static Vec3    g_grav[3];      // измеренные векторы гравитации (в системе MPU)
static Quat    g_relAtAim;     // относительная ориентация в момент наведения
static bool    g_calOk = false;
static float   g_orthoErr = 0.0f;
static float   g_axisCheck = 0.0f;

// ---------------------------------------------------------------------------
// Запуск и остановка
// ---------------------------------------------------------------------------
void calib3dStart() {
  g_step = CALIB3D_STEP_AIM;
  g_calOk = false;
  g_orthoErr = 0.0f;
  g_axisCheck = 0.0f;
  for (uint8_t i = 0; i < 3; i++) g_grav[i] = Vec3();
}

void calib3dCancel() {
  g_step = CALIB3D_STEP_IDLE;
}

bool calib3dActive() { return g_step != CALIB3D_STEP_IDLE; }
uint8_t calib3dStep() { return g_step; }

// ---------------------------------------------------------------------------
// Захват вектора гравитации (в системе координат датчика)
//
// data.acc направлен вниз (знак акселерометра инвертируется при чтении).
// ---------------------------------------------------------------------------
static Vec3 grabGravity() {
  return Vec3(data.accX, data.accY, data.accZ).normalized();
}

// ---------------------------------------------------------------------------
// Расчёт матрицы перехода MPU -> корпус
//
// Позы:
//   шаг 1: лампа вертикально -> гравитация = -Y (ось «вверх»)
//   шаг 2: наклон вправо     -> гравитация = +X (ось «влево от зрителя»)
//   шаг 3: наклон назад      -> гравитация = -Z (ось «вперёд»)
// Ось Z восстанавливается как X x Y, шаг 3 служит проверкой.
// ---------------------------------------------------------------------------
static void computeCalibration() {
  Vec3 y = (-g_grav[0]).normalized();    // ось «вверх» корпуса
  Vec3 x = g_grav[1].normalized();        // ось «влево от зрителя»

  // ортогонализация
  float d = x.dot(y);
  g_orthoErr = fabsf(d);
  x = (x - y * d).normalized();
  if (x.lengthSquared() < 0.5f) {
    // оси вырождены — поза выполнена неверно
    g_calOk = false;
    return;
  }
  Vec3 z = x.cross(y);                   // ось «вперёд» (наружу столбца 0)

  // проверка третьей оси
  g_axisCheck = g_grav[2].normalized().dot(-z);
  g_calOk = (g_orthoErr < 0.30f) && (g_axisCheck > 0.70f);

  if (g_calOk) {
    Mat3 m = Mat3::fromColumns(x, y, z);

    // Опорная поза — та, в которой прицел смотрит в глаза (шаг 1).
    // Относительная ориентация пересчитывается так, чтобы «мир» начинался
    // именно с этой позы, а не с последней (наклон назад).
    Quat now = orientationRel();
    orientationSetCalibration(m);
    orientationSetRel(g_relAtAim * now.conjugate());
  }
}

// ---------------------------------------------------------------------------
// Подтверждение шага
// ---------------------------------------------------------------------------
void calib3dTick() {
  switch (g_step) {
    case CALIB3D_STEP_AIM:
      // Поза, в которой прицел смотрит в глаза, становится опорной
      g_grav[0] = grabGravity();
      g_relAtAim = orientationRel();
      orientationResetRel();
      g_step = CALIB3D_STEP_RIGHT;
      break;

    case CALIB3D_STEP_RIGHT:
      g_grav[1] = grabGravity();
      g_step = CALIB3D_STEP_BACK;
      break;

    case CALIB3D_STEP_BACK:
      g_grav[2] = grabGravity();
      computeCalibration();
      g_step = CALIB3D_STEP_DONE;
      break;

    case CALIB3D_STEP_DONE:
      g_step = CALIB3D_STEP_IDLE;
      break;

    default:
      g_step = CALIB3D_STEP_IDLE;
      break;
  }
}

// ---------------------------------------------------------------------------
// Отрисовка прицела на матрице
// ---------------------------------------------------------------------------
void calib3dRender() {
  ledsClear();

  const uint8_t col = SCENE_CROSSHAIR_COL;
  const CRGB dimCol(0, 30, 70);
  const CRGB aimCol(0, 190, 255);
  const CRGB markCol(255, 120, 0);

  // Вертикальная линия по всему столбцу прицела
  for (uint8_t y = 0; y < MATRIX_HEIGHT; y++) {
    sideSet(col, y, dimCol);
  }

  if (g_step == CALIB3D_STEP_BACK) {
    // Смотрим сверху: прицел поднимаем к верхнему краю
    const uint8_t row = MATRIX_HEIGHT - 4;
    for (int8_t dx = -8; dx <= 8; dx++) {
      int x = (int)col + dx;
      while (x < 0) x += MATRIX_WIDTH;
      while (x >= MATRIX_WIDTH) x -= MATRIX_WIDTH;
      sideSet((uint8_t)x, row, aimCol);
    }
    for (uint8_t y = row; y < MATRIX_HEIGHT; y++) sideSet(col, y, aimCol);
    // стрелка «вверх» — лампу наклонить назад
    for (uint8_t y = MATRIX_HEIGHT - 9; y < MATRIX_HEIGHT - 1; y++) {
      sideSet((uint8_t)((col + 1) % MATRIX_WIDTH), y, markCol);
      sideSet((uint8_t)((col + MATRIX_WIDTH - 1) % MATRIX_WIDTH), y, markCol);
    }
  } else {
    // Прицел по центру высоты
    const uint8_t row = MATRIX_HEIGHT / 2;
    for (int8_t dx = -8; dx <= 8; dx++) {
      int x = (int)col + dx;
      while (x < 0) x += MATRIX_WIDTH;
      while (x >= MATRIX_WIDTH) x -= MATRIX_WIDTH;
      sideSet((uint8_t)x, row, aimCol);
    }
    // центральное перекрестье
    for (int8_t dy = -2; dy <= 2; dy++) {
      int y = (int)row + dy;
      if (y < 0 || y >= MATRIX_HEIGHT) continue;
      for (int8_t dx = -1; dx <= 1; dx++) {
        int x = (int)col + dx;
        while (x < 0) x += MATRIX_WIDTH;
        while (x >= MATRIX_WIDTH) x -= MATRIX_WIDTH;
        sideSet((uint8_t)x, (uint8_t)y, aimCol);
      }
    }

    if (g_step == CALIB3D_STEP_RIGHT) {
      // Стрелка «вправо» по верхнему краю — наклонять лампу вправо
      for (uint8_t x = (uint8_t)((col + 2) % MATRIX_WIDTH);
           x != (uint8_t)((col + 9) % MATRIX_WIDTH);
           x = (uint8_t)((x + 1) % MATRIX_WIDTH)) {
        sideSet(x, MATRIX_HEIGHT - 1, markCol);
        sideSet(x, MATRIX_HEIGHT - 2, markCol);
      }
      for (uint8_t i = 0; i < 3; i++) {
        uint8_t x = (uint8_t)((col + 9 - i) % MATRIX_WIDTH);
        sideSet(x, (uint8_t)(MATRIX_HEIGHT - 1 - i), markCol);
        sideSet(x, (uint8_t)(MATRIX_HEIGHT - 2 + i), markCol);
      }
    }
  }

  FastLED.setBrightness(150);
  FastLED.show();
}

// ---------------------------------------------------------------------------
// Инструкции на OLED
// ---------------------------------------------------------------------------
void calib3dDisplay() {
  char l1[24], l2[24], l3[24];

  switch (g_step) {
    case CALIB3D_STEP_AIM:
      snprintf(l1, sizeof(l1), "3D CAL STEP 1");
      snprintf(l2, sizeof(l2), "HOLD UPRIGHT");
      snprintf(l3, sizeof(l3), "AIM SIGHT AT YOU");
      break;

    case CALIB3D_STEP_RIGHT:
      snprintf(l1, sizeof(l1), "3D CAL STEP 2");
      snprintf(l2, sizeof(l2), "TILT RIGHT 90 DEG");
      snprintf(l3, sizeof(l3), "TOP TO THE RIGHT");
      break;

    case CALIB3D_STEP_BACK:
      snprintf(l1, sizeof(l1), "3D CAL STEP 3");
      snprintf(l2, sizeof(l2), "TILT BACK 90 DEG");
      snprintf(l3, sizeof(l3), "LOOK FROM ABOVE");
      break;

    case CALIB3D_STEP_DONE:
      if (g_calOk) {
        snprintf(l1, sizeof(l1), "3D CAL OK");
        snprintf(l2, sizeof(l2), "SAVED TO NVS");
        snprintf(l3, sizeof(l3), "ENC: EXIT");
      } else {
        snprintf(l1, sizeof(l1), "3D CAL FAILED");
        snprintf(l2, sizeof(l2), "ORTHO %.2f AX %.2f", g_orthoErr, g_axisCheck);
        snprintf(l3, sizeof(l3), "ENC: RETRY");
      }
      break;

    default:
      snprintf(l1, sizeof(l1), "3D CAL");
      snprintf(l2, sizeof(l2), "");
      snprintf(l3, sizeof(l3), "");
      break;
  }

  displayCalib(l1, l2, l3);
}
