// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Play mode camera look maths, see play_camlook.h.
#include "play_camlook.h"

#include <algorithm>
#include <cmath>

namespace nxrt::play {

void LookOffsets::add(double dx, double dy, const LookParams& p) {
    yaw_ += dx * p.yaw_rad * (p.invert_x ? 1.0 : -1.0);
    pitch_ += dy * p.pitch_rad * (p.invert_y ? 1.0 : -1.0);
    pitch_ = std::max(-p.pitch_limit_rad, std::min(p.pitch_limit_rad, pitch_));
    const double kTwoPi = 6.28318530717958647692;
    if (std::fabs(yaw_) > kTwoPi) yaw_ = std::remainder(yaw_, kTwoPi);
}

void LookOffsets::add_angles(double dyaw, double dpitch, const LookParams& p) {
    yaw_ += dyaw;
    pitch_ = std::max(-p.pitch_limit_rad, std::min(p.pitch_limit_rad, pitch_ + dpitch));
    const double kTwoPi = 6.28318530717958647692;
    if (std::fabs(yaw_) > kTwoPi) yaw_ = std::remainder(yaw_, kTwoPi);
}

namespace {
// Rodrigues: rotation matrix (row-major 3x3) about the unit axis a by angle t.
void axis_angle(const double a[3], double t, double r[9]) {
    const double c = std::cos(t), s = std::sin(t), k = 1.0 - c;
    r[0] = c + a[0] * a[0] * k;        r[1] = a[0] * a[1] * k - a[2] * s; r[2] = a[0] * a[2] * k + a[1] * s;
    r[3] = a[1] * a[0] * k + a[2] * s; r[4] = c + a[1] * a[1] * k;        r[5] = a[1] * a[2] * k - a[0] * s;
    r[6] = a[2] * a[0] * k - a[1] * s; r[7] = a[2] * a[1] * k + a[0] * s; r[8] = c + a[2] * a[2] * k;
}
void mul3(const double a[9], const double b[9], double out[9]) {
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) out[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
}
void mulv(const double a[9], const double v[3], double out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = a[i * 3] * v[0] + a[i * 3 + 1] * v[1] + a[i * 3 + 2] * v[2];
}
}  // namespace

void orbit_transform(const float in[12], const float pivot[3], double yaw, double pitch, float out[12]) {
    double m[9] = {in[0], in[1], in[2], in[4], in[5], in[6], in[8], in[9], in[10]};
    double p[3] = {in[3] - pivot[0], in[7] - pivot[1], in[11] - pivot[2]};
    // 1. yaw about world up (Y)
    const double up[3] = {0, 1, 0};
    double ry[9], m1[9], p1[3];
    axis_angle(up, yaw, ry);
    mul3(ry, m, m1);
    mulv(ry, p, p1);
    // 2. pitch about the camera's right axis after the yaw (column 0 of the rotation)
    const double right[3] = {m1[0], m1[3], m1[6]};
    const double len = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    double m2[9], p2[3];
    if (len > 1e-9 && pitch != 0.0) {
        const double a[3] = {right[0] / len, right[1] / len, right[2] / len};
        double rp[9];
        axis_angle(a, -pitch, rp);      // forward is +Z in the game: a positive pitch (looking up) turns about the right axis the other way
        mul3(rp, m1, m2);
        mulv(rp, p1, p2);
    } else {
        for (int i = 0; i < 9; ++i) m2[i] = m1[i];
        for (int i = 0; i < 3; ++i) p2[i] = p1[i];
    }
    out[0] = static_cast<float>(m2[0]); out[1] = static_cast<float>(m2[1]); out[2] = static_cast<float>(m2[2]);
    out[4] = static_cast<float>(m2[3]); out[5] = static_cast<float>(m2[4]); out[6] = static_cast<float>(m2[5]);
    out[8] = static_cast<float>(m2[6]); out[9] = static_cast<float>(m2[7]); out[10] = static_cast<float>(m2[8]);
    out[3] = static_cast<float>(pivot[0] + p2[0]);
    out[7] = static_cast<float>(pivot[1] + p2[1]);
    out[11] = static_cast<float>(pivot[2] + p2[2]);
}

}  // namespace nxrt::play
