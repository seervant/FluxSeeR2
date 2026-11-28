#pragma once
#include <Arduino.h>
#include <FastLED.h>
#include "Math3D.h"

// ============================================================================
// 3D-МОДЕЛИ С SD-КАРТЫ (формат .fle3d, спецификация — ../MODEL_FORMAT.md)
//
// Модель — сетка треугольников с запечённой анимацией. Файл целиком
// читается в PSRAM, разбирается без единого выделения памяти, а для
// поиска пересечения луча строится BVH (плоский массив в PSRAM).
//
// Модель выбирается параметром эффекта: в папке /fle3d/ может лежать
// несколько файлов, на OLED показывается их список.
//
// Если карта не вставлена или файл не читается — эффект молча
// переключается на встроенные фигуры.
// ============================================================================

#define MODEL_MAX_FILES 16      // сколько моделей показываем в списке
#define MODEL_NAME_LEN   12      // длина имени файла (без пути и расширения)
                                // — столько влезает в строку OLED вместе
                                //   с номером и названием параметра

// Результат пересечения луча с моделью
struct ModelHit {
  bool     hit = false;
  float    t = 0.0f;       // расстояние от начала луча
  uint16_t tri = 0;        // номер треугольника
  float    u = 0.0f;       // барицентрические координаты
  float    v = 0.0f;
};

// --- Инициализация ---
// Монтирует SD-карту и собирает список файлов /fle3d/*.fle3d
void model3dInit();

bool model3dCardReady();          // карта смонтирована
uint8_t model3dCount();           // число найденных моделей
const char* model3dName(uint8_t index);   // имя файла без расширения

// --- Работа с моделью ---
bool model3dLoad(uint8_t index);  // прочитать файл, построить BVH
void model3dUnload();
bool model3dLoaded();
uint32_t model3dLoadMillis();     // сколько заняла последняя загрузка
uint8_t  model3dLoadedIndex();    // индекс загруженной модели (0xFF — ничего не загружено)

// Кадр анимации: время в секундах от начала (цикл зациклен)
void model3dSetTime(float seconds);

// Геометрия текущего кадра (в единицах модели)
uint16_t model3dVertCount();
uint16_t model3dTriCount();
float    model3dRadius();        // радиус модели в единицах
Vec3     model3dVertex(uint16_t i);
CRGB     model3dVertexColor(uint16_t i);
void     model3dTriangle(uint16_t t, uint16_t& i0, uint16_t& i1, uint16_t& i2);

// Пересечение луча с моделью
ModelHit model3dCast(const Vec3& origin, const Vec3& dir);
