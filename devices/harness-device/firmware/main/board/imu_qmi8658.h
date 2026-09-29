// QMI8658 6-axis IMU on the shared I2C bus (Waveshare ESP32-S3-Touch-AMOLED-1.75C). Accelerometer only: the
// gyroscope is left off. Fork only; it exists to feed the double-shake detector (shake_task.c).
//
// The chip is optional. Its address, its presence on a given unit and every register below are marked in
// imu_qmi8658.c as either "datasheet" or "VERIFY ON HARDWARE"; this file was written without a board to
// read them back from.
#pragma once

#include <stdbool.h>

// Probe 0x6B then 0x6A, check WHO_AM_I, and configure +-4 g at about 125 Hz, accelerometer only. False (and
// nothing left allocated) when nothing answers or the id is wrong. Quiet: one log line, decided by the
// caller. Safe from any task; the shared bus serialises the transactions itself.
bool imu_qmi8658_init(void);

// Latest acceleration in milli-g. False on a bus error. Never blocks longer than one 50 ms transaction.
bool imu_qmi8658_read_mg(int *x, int *y, int *z);

// The 7-bit address that answered (0 before a successful init), for the one log line.
int imu_qmi8658_address(void);
