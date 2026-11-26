#include "Effects.h"
#include "Config.h"
#include "LedGeometry.h"
#include "Math3D.h"
#include "Orientation.h"
#include "Params.h"
#include "Scene3D.h"

// ============================================================================
// ЭФФЕКТЫ 8 и 9 — 3D-СЦЕНА НА ЦИЛИНДРИЧЕСКОЙ МАТРИЦЕ
//
// Эффект 8 «Сцена, привязанная к гравитации»
//   Объект зафиксирован в мировом пространстве: вертикаль сцены всегда
//   по гравитации, поворот лампы в руках не вращает объект. Видна та
//   часть объекта, которая оказалась напротив наблюдателя, — эффект
//   «окна» в неподвижный мир. Наклон лампы меняет ракурс: посмотрев
//   сверху, пользователь видит верх объекта.
//
// Эффект 9 «Сцена, привязанная к корпусу»
//   Объект жёстко связан с корпусом и вращается вместе с ним: лампа —
//   контейнер сцены, а не окно в мир. Никакой стабилизации относительно
//   земли, изображение следует за поворотом корпуса без задержки.
//
// Оба эффекта используют один и тот же рендер, различается только система
// отсчёта, в которой живёт объект:
//
//   эффект 8: система сцены = калибровочная (мировая) система;
//              глаз в ней неподвижен, объект неподвижен;
//   эффект 9: система сцены = система корпуса;
//              глаз перемещается вместе с поворотом корпуса.
//
// Смещение между ними задаётся кватернионом ориентации, который
// рассчитывается по гироскопу с коррекцией наклона по гравитации.
// ============================================================================

// ---------------------------------------------------------------------------
// Чтение параметров эффекта
// ---------------------------------------------------------------------------
static void sceneReadParams(uint8_t eff, SceneOptions& opt,
                            float& distMm, float& eyeHmm) {
  opt.figure  = (uint8_t)constrain(paramsValue(eff, SCENE_P_FIGURE),
                                    0, SCENE_FIGURE_COUNT - 1);
  opt.sizePct = (float)constrain(paramsValue(eff, SCENE_P_SIZE), 5, 100);
  opt.hue     = (uint8_t)paramsRealValue(eff, SCENE_P_COLOR);
  opt.edgeMm  = (float)constrain(paramsValue(eff, SCENE_P_EDGE), 0, 30);

  distMm = (float)constrain(paramsValue(eff, SCENE_P_DIST), 200, 1200);
  eyeHmm = (float)constrain(paramsValue(eff, SCENE_P_EYEH), -200, 600);
}

// ---------------------------------------------------------------------------
// Положение наблюдателя в калибровочной системе
//
// Азимут задан столбцом с «прицелом»: во время калибровки пользователь
// поворачивает лампу так, чтобы прицел смотрел в глаза, поэтому именно
// этот столбец указывает на наблюдателя.
// ---------------------------------------------------------------------------
static Vec3 eyeInCalFrame(float distMm, float eyeHmm) {
  float ang = (float)SCENE_COL_DIR * (float)SCENE_CROSSHAIR_COL *
              (2.0f * PI / (float)MATRIX_WIDTH);
  return Vec3(sinf(ang) * distMm, eyeHmm, cosf(ang) * distMm);
}

// ---------------------------------------------------------------------------
// Эффект 8 — сцена привязана к гравитации
// ---------------------------------------------------------------------------
void renderEffect_SceneGravity() {
  const uint8_t eff = EFFECT_SCENE_GRAV;

  SceneOptions opt;
  float distMm = 0.0f, eyeHmm = 0.0f;
  sceneReadParams(eff, opt, distMm, eyeHmm);

  // Система сцены = калибровочная (мировая): объект и наблюдатель в ней
  // неподвижны, а корпус лампы поворачивается.
  const Quat bodyToScene = orientationRel();
  const Vec3  eyeScene   = eyeInCalFrame(distMm, eyeHmm);

  scene3dRender(opt, bodyToScene, eyeScene);
}

// ---------------------------------------------------------------------------
// Эффект 9 — сцена привязана к корпусу
// ---------------------------------------------------------------------------
void renderEffect_SceneBody() {
  const uint8_t eff = EFFECT_SCENE_BODY;

  SceneOptions opt;
  float distMm = 0.0f, eyeHmm = 0.0f;
  sceneReadParams(eff, opt, distMm, eyeHmm);

  // Система сцены = система корпуса: объект неподвижен в ней,
  // а наблюдатель перемещается при повороте лампы.
  const Quat bodyToScene;                              // тождественный поворот
  const Vec3  eyeScene = orientationRel().rotateInverse(eyeInCalFrame(distMm, eyeHmm));

  scene3dRender(opt, bodyToScene, eyeScene);
}
