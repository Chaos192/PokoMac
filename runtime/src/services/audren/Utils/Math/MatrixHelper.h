// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Utils/Math/MatrixHelper.cs (C# class Ryujinx.Audio.Renderer.Dsp.MatrixHelper).
// The Vector4/Matrix4x4/Vector2 overloads use System.Numerics types in C#. No C++ equivalent exists in the port yet, so the minimal
// stand-ins below (same field names) are defined in nxrt::audren::Numerics. Other ports that need System.Numerics should use them.
#pragma once
#include "../../audren_common.h"
#include "Matrix2x2.h"
#include "Matrix6x6.h"
#include "Vector6.h"

namespace nxrt::audren {

namespace Numerics {

// System.Numerics.Vector2 stand-in.
struct Vector2 {
    float X = 0.0f;
    float Y = 0.0f;

    Vector2() = default;
    Vector2(float x, float y) : X(x), Y(y) {}

    bool operator==(const Vector2& other) const = default;
};

// System.Numerics.Vector4 stand-in.
struct Vector4 {
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    float W = 0.0f;

    Vector4() = default;
    Vector4(float x, float y, float z, float w) : X(x), Y(y), Z(z), W(w) {}

    bool operator==(const Vector4& other) const = default;
};

// System.Numerics.Matrix4x4 stand-in (row-major field names M<row><col>, 1-based, as in C#).
struct Matrix4x4 {
    float M11 = 0.0f, M12 = 0.0f, M13 = 0.0f, M14 = 0.0f;
    float M21 = 0.0f, M22 = 0.0f, M23 = 0.0f, M24 = 0.0f;
    float M31 = 0.0f, M32 = 0.0f, M33 = 0.0f, M34 = 0.0f;
    float M41 = 0.0f, M42 = 0.0f, M43 = 0.0f, M44 = 0.0f;

    Matrix4x4() = default;

    bool operator==(const Matrix4x4& other) const = default;
};

}  // namespace Numerics

// Static class MatrixHelper: row-vector times matrix.
struct MatrixHelper {
    static Vector6 Transform(const Vector6& value1, const Matrix6x6& value2) {
        return Vector6(
            value2.M11 * value1.X + value2.M12 * value1.Y + value2.M13 * value1.Z + value2.M14 * value1.W + value2.M15 * value1.V + value2.M16 * value1.U,
            value2.M21 * value1.X + value2.M22 * value1.Y + value2.M23 * value1.Z + value2.M24 * value1.W + value2.M25 * value1.V + value2.M26 * value1.U,
            value2.M31 * value1.X + value2.M32 * value1.Y + value2.M33 * value1.Z + value2.M34 * value1.W + value2.M35 * value1.V + value2.M36 * value1.U,
            value2.M41 * value1.X + value2.M42 * value1.Y + value2.M43 * value1.Z + value2.M44 * value1.W + value2.M45 * value1.V + value2.M46 * value1.U,
            value2.M51 * value1.X + value2.M52 * value1.Y + value2.M53 * value1.Z + value2.M54 * value1.W + value2.M55 * value1.V + value2.M56 * value1.U,
            value2.M61 * value1.X + value2.M62 * value1.Y + value2.M63 * value1.Z + value2.M64 * value1.W + value2.M65 * value1.V + value2.M66 * value1.U);
    }

    static Numerics::Vector4 Transform(const Numerics::Vector4& value1, const Numerics::Matrix4x4& value2) {
        return Numerics::Vector4(
            value2.M11 * value1.X + value2.M12 * value1.Y + value2.M13 * value1.Z + value2.M14 * value1.W,
            value2.M21 * value1.X + value2.M22 * value1.Y + value2.M23 * value1.Z + value2.M24 * value1.W,
            value2.M31 * value1.X + value2.M32 * value1.Y + value2.M33 * value1.Z + value2.M34 * value1.W,
            value2.M41 * value1.X + value2.M42 * value1.Y + value2.M43 * value1.Z + value2.M44 * value1.W);
    }

    static Numerics::Vector2 Transform(const Numerics::Vector2& value1, const Matrix2x2& value2) {
        return Numerics::Vector2(value2.M11 * value1.X + value2.M12 * value1.Y, value2.M21 * value1.X + value2.M22 * value1.Y);
    }
};

}  // namespace nxrt::audren
