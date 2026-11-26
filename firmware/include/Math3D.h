#pragma once
#include <Arduino.h>

// ============================================================================
// МАТЕМАТИКА 3D — векторы, кватернионы, матрицы
//
// Минимальный набор для эффектов 3D-сцены:
//   - Vec3  — вектор в правой системе координат
//   - Quat  — кватернион вращения (форма Гамильтона)
//   - Mat3  — матрица 3x3 (построчно, m[r*3 + c])
//
// Соглашение о кватернионе в модуле ориентации:
//   v_body = psi * v_cal * psi^(-1)
//   то есть psi переводит вектор из калибровочной (мировой) системы
//   в систему корпуса лампы.
// ============================================================================

// ---------------------------------------------------------------------------
// Вектор
// ---------------------------------------------------------------------------
struct Vec3 {
  float x = 0.0f, y = 0.0f, z = 0.0f;

  Vec3() = default;
  Vec3(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}

  Vec3 operator+(const Vec3& o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
  Vec3 operator-(const Vec3& o) const { return Vec3(x - o.x, y - o.y, z - o.z); }
  Vec3 operator-() const { return Vec3(-x, -y, -z); }
  Vec3 operator*(float s) const { return Vec3(x * s, y * s, z * s); }
  Vec3 operator/(float s) const { return Vec3(x / s, y / s, z / s); }

  Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
  Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
  Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }

  float dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }

  Vec3 cross(const Vec3& o) const {
    return Vec3(y * o.z - z * o.y,
                z * o.x - x * o.z,
                x * o.y - y * o.x);
  }

  float lengthSquared() const { return x * x + y * y + z * z; }
  float length() const { return sqrtf(x * x + y * y + z * z); }

  Vec3 normalized() const {
    float l = length();
    if (l < 1e-9f) return Vec3(0.0f, 0.0f, 0.0f);
    float inv = 1.0f / l;
    return Vec3(x * inv, y * inv, z * inv);
  }

  // Доступ по индексу: 0 = X, 1 = Y, 2 = Z
  float at(uint8_t i) const { return (i == 0) ? x : ((i == 1) ? y : z); }
  float& at(uint8_t i) { return (i == 0) ? x : ((i == 1) ? y : z); }
};

// ---------------------------------------------------------------------------
// Кватернион
// ---------------------------------------------------------------------------
struct Quat {
  float w = 1.0f, x = 0.0f, y = 0.0f, z = 0.0f;

  Quat() = default;
  Quat(float _w, float _x, float _y, float _z) : w(_w), x(_x), y(_y), z(_z) {}

  // Произведение Гамильтона
  Quat operator*(const Quat& o) const {
    return Quat(w * o.w - x * o.x - y * o.y - z * o.z,
                w * o.x + x * o.w + y * o.z - z * o.y,
                w * o.y - x * o.z + y * o.w + z * o.x,
                w * o.z + x * o.y - y * o.x + z * o.w);
  }

  // Сопряжённый (инверсия для единичного кватерниона)
  Quat conjugate() const { return Quat(w, -x, -y, -z); }

  Quat normalized() const {
    float l = sqrtf(w * w + x * x + y * y + z * z);
    if (l < 1e-9f) return Quat();
    float inv = 1.0f / l;
    return Quat(w * inv, x * inv, y * inv, z * inv);
  }

  // Поворот вектора: v' = q * v * q^(-1)
  Vec3 rotate(const Vec3& v) const {
    Vec3 qv(x, y, z);
    Vec3 t = qv.cross(v) * 2.0f;
    return v + t * w + qv.cross(t);
  }

  // Поворот вектора обратным кватернионом: v' = q^(-1) * v * q
  Vec3 rotateInverse(const Vec3& v) const {
    Vec3 qv(-x, -y, -z);
    Vec3 t = qv.cross(v) * 2.0f;
    return v + t * w + qv.cross(t);
  }

  // Кватернион малого поворота вокруг оси на угол angle (рад)
  static Quat fromAxisAngle(const Vec3& axis, float angle) {
    Vec3 a = axis.normalized();
    float s = sinf(angle * 0.5f);
    return Quat(cosf(angle * 0.5f), a.x * s, a.y * s, a.z * s);
  }

  // Угол поворота кватерниона, рад
  float angle() const {
    Quat q = normalized();
    float w = q.w;
    if (w < 0.0f) w = -w;           // берём кратчайшую дугу
    w = constrain(w, -1.0f, 1.0f);
    return 2.0f * acosf(w);
  }

  // Ось поворота (единичная)
  Vec3 axis() const {
    Quat q = normalized();
    float s = sqrtf(1.0f - q.w * q.w);
    if (s < 1e-6f) return Vec3(0.0f, 1.0f, 0.0f);
    return Vec3(q.x / s, q.y / s, q.z / s);
  }
};

// ---------------------------------------------------------------------------
// Матрица 3x3 (построчно: m[r * 3 + c])
// ---------------------------------------------------------------------------
struct Mat3 {
  float m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};

  static Mat3 identity() {
    Mat3 r;
    r.m[0] = 1; r.m[1] = 0; r.m[2] = 0;
    r.m[3] = 0; r.m[4] = 1; r.m[5] = 0;
    r.m[6] = 0; r.m[7] = 0; r.m[8] = 1;
    return r;
  }

  // Матрица из столбцов (оси базиса)
  static Mat3 fromColumns(const Vec3& cx, const Vec3& cy, const Vec3& cz) {
    Mat3 r;
    r.m[0] = cx.x; r.m[1] = cy.x; r.m[2] = cz.x;
    r.m[3] = cx.y; r.m[4] = cy.y; r.m[5] = cz.y;
    r.m[6] = cx.z; r.m[7] = cy.z; r.m[8] = cz.z;
    return r;
  }

  Vec3 operator*(const Vec3& v) const {
    return Vec3(m[0] * v.x + m[1] * v.y + m[2] * v.z,
                m[3] * v.x + m[4] * v.y + m[5] * v.z,
                m[6] * v.x + m[7] * v.y + m[8] * v.z);
  }

  Mat3 operator*(const Mat3& o) const {
    Mat3 r;
    for (uint8_t i = 0; i < 3; i++) {
      for (uint8_t j = 0; j < 3; j++) {
        float s = 0.0f;
        for (uint8_t k = 0; k < 3; k++) s += m[i * 3 + k] * o.m[k * 3 + j];
        r.m[i * 3 + j] = s;
      }
    }
    return r;
  }

  Mat3 transpose() const {
    Mat3 r;
    for (uint8_t i = 0; i < 3; i++)
      for (uint8_t j = 0; j < 3; j++)
        r.m[i * 3 + j] = m[j * 3 + i];
    return r;
  }

  // Определитель (для проверки правильности базиса)
  float determinant() const {
    return m[0] * (m[4] * m[8] - m[5] * m[7])
         - m[1] * (m[3] * m[8] - m[5] * m[6])
         + m[2] * (m[3] * m[7] - m[4] * m[6]);
  }
};
