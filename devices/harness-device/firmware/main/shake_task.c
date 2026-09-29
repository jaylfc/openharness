#include "shake_task.h"
#include "shake_detect.h"
#include "board/imu_qmi8658.h"
#include "ui/display.h"
#include "ui/ui_screens.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "shake";

#define SAMPLE_MS        10     // about 100 Hz, the rate the detector's thresholds are tuned for
#define ASLEEP_POLL_MS   250    // screen off: no bus traffic at all, just a check for waking
#define PROBE_TRIES      4      // the sensor's rail may come up late; try a few times, then give up quietly
#define PROBE_RETRY_MS   1500
#define READ_FAIL_LIMIT  50     // half a second of NACKs in a row: the bus or the chip is gone

static void shake_task(void *arg)
{
    (void)arg;
    bool up = false;
    for (int i = 0; i < PROBE_TRIES && !(up = imu_qmi8658_init()); i++) vTaskDelay(pdMS_TO_TICKS(PROBE_RETRY_MS));
    if (!up) {
        ESP_LOGI(TAG, "no IMU answered at 0x6B/0x6A: shake to open the menu is off");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "QMI8658 at 0x%02X, accel +-4 g: shake to open the menu is on", imu_qmi8658_address());

    ht_shake_t det;
    ht_shake_reset(&det);
    int fails = 0;
    while (1) {
        if (display_is_asleep()) {
            // A shake while the screen is off does nothing; the detector is told so it forgets the moment.
            ht_shake_feed(&det, (uint32_t)(esp_timer_get_time() / 1000), 0, 0, 1000, true);
            vTaskDelay(pdMS_TO_TICKS(ASLEEP_POLL_MS));
            continue;
        }
        int x, y, z;
        if (imu_qmi8658_read_mg(&x, &y, &z)) {
            fails = 0;
            if (ht_shake_feed(&det, (uint32_t)(esp_timer_get_time() / 1000), x, y, z, false)) ui_shake();
        } else if (++fails >= READ_FAIL_LIMIT) {
            ESP_LOGW(TAG, "IMU stopped answering: shake to open the menu is off");
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));
    }
}

void shake_start(void)
{
    // Priority 2, under the button task (3) and the UI (4): sampling never preempts either. 3 KB of stack
    // is generous for a loop of one bus read; it is returned if the IMU is absent.
    xTaskCreate(shake_task, "shake", 3072, NULL, 2, NULL);
}
