#pragma once
#include <Arduino.h>
#include "Types.h"

// ============================================================================
// РЕЕСТР ПАРАМЕТРОВ ЭФФЕКТОВ
//
// Параметры хранятся в таблицах: для каждого эффекта — список ParamSpec
// (имя, диапазон, тип, флаги) и массив текущих значений.
// Это избавляет от «лестницы» switch-кейсов в Controls и Display.
// ============================================================================

// Максимум параметров, задекларированных для одного эффекта
#define PARAM_TABLE_MAX 14

// ---------------------------------------------------------------------------
// Таблицы параметров (по ТЗ, раздел 5)
// ---------------------------------------------------------------------------
extern const ParamSpec PARAMS_STATIC[];
extern const ParamSpec PARAMS_RAINBOW_Z[];
extern const ParamSpec PARAMS_RAINBOW_CIRCLE[];
extern const ParamSpec PARAMS_FLUID[];
extern const ParamSpec PARAMS_SPIRAL[];
extern const ParamSpec PARAMS_BALLS[];
extern const ParamSpec PARAMS_LAVA[];
extern const ParamSpec PARAMS_SCENE_GRAV[];
extern const ParamSpec PARAMS_SCENE_BODY[];

// ---------------------------------------------------------------------------
// Экспоненциальная шкала «процент силы притяжения G» (ТЗ:
// шаг 10% в диапазоне 0..100%, далее по экспоненте)
// Индекс в таблице = значение параметра, элемент = процент.
// ---------------------------------------------------------------------------
extern const uint16_t G_POWER_TABLE[];
extern const uint8_t  G_POWER_TABLE_SIZE;

// Процент силы G по индексу параметра
int16_t paramsGPowerPercent(int16_t index);

// ---------------------------------------------------------------------------
// Инициализация значений по умолчанию
// ---------------------------------------------------------------------------
void paramsInit();

// Количество параметров эффекта
uint8_t paramsCount(uint8_t effect);

// Ссылка на таблицу параметров эффекта
const ParamSpec* paramsSpec(uint8_t effect);

// Текущее значение параметра (сырое, в единицах параметра)
int16_t paramsValue(uint8_t effect, uint8_t param);

// Записать значение параметра с проверкой диапазона
void paramsSetValue(uint8_t effect, uint8_t param, int16_t value);

// Изменить значение параметра на дельту (с учётом PF_WRAP)
void paramsAdjust(uint8_t effect, uint8_t param, int32_t delta);

// Значение «как видит пользователь» (для OLED): для G — процент, для MODE — индекс
int16_t paramsDisplayValue(uint8_t effect, uint8_t param);

// Текстовая подпись значения (только PK_MODE возвращает строку)
const char* paramsDisplayText(uint8_t effect, uint8_t param);

// Инженерное нормированное значение (яркость -> 0..255, цвет -> Hue 0..255)
int16_t paramsRealValue(uint8_t effect, uint8_t param);

// Доля шкалы параметра 0..255 (для ползунка на OLED)
uint8_t paramsSliderFraction(uint8_t effect, uint8_t param);

// Найти параметр заданного типа в эффекте (например, PK_BRIGHTNESS).
// Возвращает 0xFF, если такого параметра нет.
uint8_t paramsFindByKind(uint8_t effect, uint8_t kind);
