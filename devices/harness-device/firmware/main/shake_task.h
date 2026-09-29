// The shake-to-open-menu task (fork only): reads the IMU at low priority, feeds the pure detector in
// shake_detect.c, and calls ui_shake() on a double shake.
#pragma once

// Create the task. Returns at once: the IMU is probed on the task itself, so an absent one costs the boot
// nothing. With no IMU the task logs one line and deletes itself, returning its stack.
void shake_start(void);
