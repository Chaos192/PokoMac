// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Play mode pad orientation, see pad_orient.h.
#include "pad_orient.h"

#include <cmath>

namespace nxrt::play {
namespace {

constexpr double kTwoPi = 6.283185307179586;

// m is the pad-to-world rotation, row-major, whose COLUMNS are the pad's axes in the world; direction stores the axes as rows of three, so direction[3 * a + w] = m[w][a].
void to_matrix(const float d[9], double m[3][3]) {
    for (int a = 0; a < 3; ++a)
        for (int w = 0; w < 3; ++w) m[w][a] = d[3 * a + w];
}
void from_matrix(const double m[3][3], float d[9]) {
    for (int a = 0; a < 3; ++a)
        for (int w = 0; w < 3; ++w) d[3 * a + w] = static_cast<float>(m[w][a]);
}

// Rotation by the vector v (axis * angle in radians), Rodrigues.
void rot_vec(const double v[3], double r[3][3]) {
    const double th = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (th < 1e-12) {
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) r[i][j] = i == j ? 1.0 : 0.0;
        return;
    }
    const double x = v[0] / th, y = v[1] / th, z = v[2] / th, c = std::cos(th), s = std::sin(th), t = 1.0 - c;
    r[0][0] = c + x * x * t;     r[0][1] = x * y * t - z * s; r[0][2] = x * z * t + y * s;
    r[1][0] = y * x * t + z * s; r[1][1] = c + y * y * t;     r[1][2] = y * z * t - x * s;
    r[2][0] = z * x * t - y * s; r[2][1] = z * y * t + x * s; r[2][2] = c + z * z * t;
}

void mul(const double a[3][3], const double b[3][3], double out[3][3]) {
    double t[3][3];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) out[i][j] = t[i][j];
}

// Keeps the matrix a rotation (the products drift): Gram-Schmidt on the columns.
void orthonormalize(double m[3][3]) {
    double c[3][3];
    for (int w = 0; w < 3; ++w) {
        c[0][w] = m[w][0];
        c[1][w] = m[w][1];
    }
    auto norm = [](double* v) {
        const double l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (l > 1e-12)
            for (int i = 0; i < 3; ++i) v[i] /= l;
    };
    norm(c[0]);
    const double d = c[0][0] * c[1][0] + c[0][1] * c[1][1] + c[0][2] * c[1][2];
    for (int i = 0; i < 3; ++i) c[1][i] -= d * c[0][i];
    norm(c[1]);
    c[2][0] = c[0][1] * c[1][2] - c[0][2] * c[1][1];
    c[2][1] = c[0][2] * c[1][0] - c[0][0] * c[1][2];
    c[2][2] = c[0][0] * c[1][1] - c[0][1] * c[1][0];
    for (int a = 0; a < 3; ++a)
        for (int w = 0; w < 3; ++w) m[w][a] = c[a][w];
}

}  // namespace

void PadOrient::reset() {
    for (int i = 0; i < 9; ++i) direction[i] = (i % 4 == 0) ? 1.f : 0.f;
    for (float& a : angle) a = 0.f;
}

void PadOrient::step(const float gyro[3], const float accel[3], double dt, double tilt_gain) {
    if (!(dt > 0.0) || dt > 0.5) return;
    double m[3][3], r[3][3];
    to_matrix(direction, m);
    // the gyro turns the pad about its own axes
    const double v[3] = {gyro[0] * kTwoPi * dt, gyro[1] * kTwoPi * dt, gyro[2] * kTwoPi * dt};
    rot_vec(v, r);
    mul(m, r, m);
    for (int i = 0; i < 3; ++i) angle[i] += static_cast<float>(gyro[i] * dt);
    // gravity: the accelerometer reads, in the pad's frame, the direction opposite to the pull (0, 0, -1 G lying face up, so the world "down" is the world z axis negated).
    // The tilt error is the cross product of where that points in the world and where it should (0, 0, -1).
    const double an = std::sqrt(static_cast<double>(accel[0]) * accel[0] + static_cast<double>(accel[1]) * accel[1] + static_cast<double>(accel[2]) * accel[2]);
    if (tilt_gain > 0.0 && an > 0.85 && an < 1.15) {
        double aw[3];
        for (int w = 0; w < 3; ++w) aw[w] = (m[w][0] * accel[0] + m[w][1] * accel[1] + m[w][2] * accel[2]) / an;      // accel in the world frame
        const double want[3] = {0, 0, -1};
        // rotate by (aw x want) * gain * dt in the world frame: the world-frame correction is applied on the left
        const double e[3] = {aw[1] * want[2] - aw[2] * want[1], aw[2] * want[0] - aw[0] * want[2], aw[0] * want[1] - aw[1] * want[0]};
        const double k = tilt_gain * dt;
        const double cv[3] = {e[0] * k, e[1] * k, e[2] * k};
        rot_vec(cv, r);
        mul(r, m, m);
    }
    orthonormalize(m);
    from_matrix(m, direction);
}

}  // namespace nxrt::play
