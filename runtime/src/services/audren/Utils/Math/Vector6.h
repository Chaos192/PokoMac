// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Utils/Math/Vector6.cs (C# record struct: value equality is kept as operator==).
#pragma once
#include "../../audren_common.h"

namespace nxrt::audren {

struct Vector6 {
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    float W = 0.0f;
    float V = 0.0f;
    float U = 0.0f;

    Vector6() = default;

    // C# Vector6(float value): all six components set to value.
    explicit Vector6(float value) : Vector6(value, value, value, value, value, value) {}

    Vector6(float x, float y, float z, float w, float v, float u) : X(x), Y(y), Z(z), W(w), V(v), U(u) {}

    bool operator==(const Vector6& other) const = default;
};

inline Vector6 operator+(const Vector6& left, const Vector6& right) {
    return Vector6(left.X + right.X, left.Y + right.Y, left.Z + right.Z, left.W + right.W, left.V + right.V, left.U + right.U);
}

inline Vector6 operator*(const Vector6& left, const Vector6& right) {
    return Vector6(left.X * right.X, left.Y * right.Y, left.Z * right.Z, left.W * right.W, left.V * right.V, left.U * right.U);
}

inline Vector6 operator*(const Vector6& left, float right) { return left * Vector6(right); }

}  // namespace nxrt::audren
