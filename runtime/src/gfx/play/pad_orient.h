// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Play mode: the pad's orientation from its gyro and accelerometer. The console's sensor fusion hands the game, next to the raw readings, the pad's turn since it was
// reset (angle) and the pad's axes in the world frame (direction); the game's pointer control scheme reads the direction. SDL gives only the raw readings, so this
// integrates them: the gyro turns the axes, and the accelerometer pulls the tilt back toward gravity slowly (the gyro drifts). Pure code, no SDL.
#pragma once

namespace nxrt::play {

struct PadOrient {
    // rows are the pad's x, y and z axes in the world frame (the layout of nn::hid::DirectionState); identity = lying still, face up, y toward the front
    float direction[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    float angle[3] = {0, 0, 0};      // the turn about each axis since reset, in revolutions

    void reset();
    // gyro: revolutions per second about the pad's own axes. accel: in G, the console's sign (0, 0, -1 lying still face up). dt: seconds since the last step.
    // tilt_gain: how fast gravity corrects the tilt (per second, 0 = not at all); only used while |accel| is within 15 percent of 1 G.
    void step(const float gyro[3], const float accel[3], double dt, double tilt_gain = 0.5);
};

}  // namespace nxrt::play
