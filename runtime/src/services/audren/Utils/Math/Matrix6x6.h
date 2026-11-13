// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Port of Ryujinx.Audio/Renderer/Utils/Math/Matrix6x6.cs (C# record struct: value equality is kept as operator==).
// Field names M<row><col> are the C# names, 1-based like the C# source.
#pragma once
#include "../../audren_common.h"

namespace nxrt::audren {

struct Matrix6x6 {
    float M11 = 0.0f, M12 = 0.0f, M13 = 0.0f, M14 = 0.0f, M15 = 0.0f, M16 = 0.0f;
    float M21 = 0.0f, M22 = 0.0f, M23 = 0.0f, M24 = 0.0f, M25 = 0.0f, M26 = 0.0f;
    float M31 = 0.0f, M32 = 0.0f, M33 = 0.0f, M34 = 0.0f, M35 = 0.0f, M36 = 0.0f;
    float M41 = 0.0f, M42 = 0.0f, M43 = 0.0f, M44 = 0.0f, M45 = 0.0f, M46 = 0.0f;
    float M51 = 0.0f, M52 = 0.0f, M53 = 0.0f, M54 = 0.0f, M55 = 0.0f, M56 = 0.0f;
    float M61 = 0.0f, M62 = 0.0f, M63 = 0.0f, M64 = 0.0f, M65 = 0.0f, M66 = 0.0f;

    Matrix6x6() = default;

    Matrix6x6(float m11, float m12, float m13, float m14, float m15, float m16,
              float m21, float m22, float m23, float m24, float m25, float m26,
              float m31, float m32, float m33, float m34, float m35, float m36,
              float m41, float m42, float m43, float m44, float m45, float m46,
              float m51, float m52, float m53, float m54, float m55, float m56,
              float m61, float m62, float m63, float m64, float m65, float m66)
        : M11(m11), M12(m12), M13(m13), M14(m14), M15(m15), M16(m16),
          M21(m21), M22(m22), M23(m23), M24(m24), M25(m25), M26(m26),
          M31(m31), M32(m32), M33(m33), M34(m34), M35(m35), M36(m36),
          M41(m41), M42(m42), M43(m43), M44(m44), M45(m45), M46(m46),
          M51(m51), M52(m52), M53(m53), M54(m54), M55(m55), M56(m56),
          M61(m61), M62(m62), M63(m63), M64(m64), M65(m65), M66(m66) {}

    bool operator==(const Matrix6x6& other) const = default;
};

}  // namespace nxrt::audren
