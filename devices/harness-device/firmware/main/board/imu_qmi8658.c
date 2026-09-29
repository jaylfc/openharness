#include "imu_qmi8658.h"
#include "board_i2c.h"
#include "board_pins.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"

// Written from the QMI8658C register map as remembered, with no board on the desk and no way to fetch the
// datasheet. Each line says which. Anything marked VERIFY is a guess to be checked with a read-back on the
// glass before the shake feature is trusted; the WHO_AM_I check below refuses to go on if the chip is not
// what we think it is, which is the one safeguard that does not depend on the rest being right.
#define QMI_ADDR_HI       0x6B   // VERIFY: SA0 high. Waveshare's docs page does not state the address.
#define QMI_ADDR_LO       0x6A   // VERIFY: SA0 low
#define QMI_REG_WHO_AM_I  0x00   // datasheet: reads 0x05
#define QMI_WHO_AM_I      0x05
#define QMI_REG_CTRL1     0x02   // VERIFY: bit6 ADDR_AI (register auto-increment), bit5 BE (0 = little endian)
#define QMI_CTRL1_AI      0x40
#define QMI_REG_CTRL2     0x03   // VERIFY: accel: bits6:4 full scale, bits3:0 output data rate
#define QMI_CTRL2_4G_125HZ 0x16  // VERIFY: 001 = +-4 g, 0110 = 125 Hz (normal-power code; low-power codes differ)
#define QMI_REG_CTRL7     0x08   // VERIFY: bit0 accel enable, bit1 gyro enable
#define QMI_CTRL7_ACCEL   0x01
#define QMI_REG_AX_L      0x35   // VERIFY: AX_L, AX_H, AY_L, AY_H, AZ_L, AZ_H, little endian, signed 16 bit
#define QMI_LSB_PER_G_4G  8192   // VERIFY: 32768 / 4 g

static i2c_master_dev_handle_t s_dev;
static int s_addr;

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_dev, buf, sizeof buf, pdMS_TO_TICKS(50));
}

static esp_err_t reg_read(uint8_t reg, uint8_t *out, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, out, n, pdMS_TO_TICKS(50));
}

bool imu_qmi8658_init(void)
{
    if (s_dev) return true;
    i2c_master_bus_handle_t bus = board_i2c_get();
    if (!bus) return false;
    static const uint8_t candidates[] = {QMI_ADDR_HI, QMI_ADDR_LO};
    for (unsigned i = 0; i < sizeof candidates; i++) {
        if (i2c_master_probe(bus, candidates[i], 20) != ESP_OK) continue;   // NACK: nothing there
        i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = candidates[i],
            .scl_speed_hz = BSP_I2C_FREQ_HZ,
        };
        if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) { s_dev = NULL; continue; }
        uint8_t id = 0;
        if (reg_read(QMI_REG_WHO_AM_I, &id, 1) == ESP_OK && id == QMI_WHO_AM_I &&
            reg_write(QMI_REG_CTRL1, QMI_CTRL1_AI) == ESP_OK &&
            reg_write(QMI_REG_CTRL2, QMI_CTRL2_4G_125HZ) == ESP_OK &&
            reg_write(QMI_REG_CTRL7, QMI_CTRL7_ACCEL) == ESP_OK) {
            s_addr = candidates[i];
            return true;
        }
        i2c_master_bus_rm_device(s_dev);   // something else lives here, or it would not configure
        s_dev = NULL;
    }
    return false;
}

bool imu_qmi8658_read_mg(int *x, int *y, int *z)
{
    uint8_t raw[6];
    if (!s_dev || reg_read(QMI_REG_AX_L, raw, sizeof raw) != ESP_OK) return false;
    int16_t v[3];
    for (int i = 0; i < 3; i++) v[i] = (int16_t)((uint16_t)raw[2 * i] | ((uint16_t)raw[2 * i + 1] << 8));
    *x = v[0] * 1000 / QMI_LSB_PER_G_4G;
    *y = v[1] * 1000 / QMI_LSB_PER_G_4G;
    *z = v[2] * 1000 / QMI_LSB_PER_G_4G;
    return true;
}

int imu_qmi8658_address(void) { return s_addr; }
