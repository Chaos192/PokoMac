// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Utils/Math/Matrix2x2.cs (C# record struct: value equality is kept as operator==).
#pragma once
#include "../../audren_common.h"

namespace nxrt::audren {

struct Matrix2x2 {
    float M11 = 0.0f;
    float M12 = 0.0f;
    float M21 = 0.0f;
    float M22 = 0.0f;

    Matrix2x2() = default;

    Matrix2x2(float m11, float m12, float m21, float m22) : M11(m11), M12(m12), M21(m21), M22(m22) {}

    bool operator==(const Matrix2x2& other) const = default;
};

inline Matrix2x2 operator+(const Matrix2x2& value1, const Matrix2x2& value2) {
    return Matrix2x2(value1.M11 + value2.M11, value1.M12 + value2.M12, value1.M21 + value2.M21, value1.M22 + value2.M22);
}

inline Matrix2x2 operator-(const Matrix2x2& value1, float value2) {
    return Matrix2x2(value1.M11 - value2, value1.M12 - value2, value1.M21 - value2, value1.M22 - value2);
}

inline Matrix2x2 operator*(const Matrix2x2& value1, float value2) {
    return Matrix2x2(value1.M11 * value2, value1.M12 * value2, value1.M21 * value2, value1.M22 * value2);
}

inline Matrix2x2 operator*(const Matrix2x2& value1, const Matrix2x2& value2) {
    // First row
    float m11 = value1.M11 * value2.M11 + value1.M12 * value2.M21;
    float m12 = value1.M11 * value2.M12 + value1.M12 * value2.M22;

    // Second row
    float m21 = value1.M21 * value2.M11 + value1.M22 * value2.M21;
    float m22 = value1.M21 * value2.M12 + value1.M22 * value2.M22;

    return Matrix2x2(m11, m12, m21, m22);
}

}  // namespace nxrt::audren
