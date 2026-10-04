#include "launcher_return.h"
#include <stdint.h>
#include "sdkconfig.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "bootloader_common.h"

// The word's layout and values are the Orb's (taos-orb components/orb_boot/include/orb_bootword.h): 16 bytes in
// rtc_retain_mem_t.custom, valid only when magic matches and check == f(value), so random RTC contents after a
// power-on never select anything. The address agrees only when both builds reserve the same 16 bytes.
#if !CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC || CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_IN_CRC
#error "launcher_return needs CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC=y and CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_IN_CRC=n"
#endif
_Static_assert(CONFIG_BOOTLOADER_CUSTOM_RESERVE_RTC_SIZE == 16, "the Orb reserves exactly 16 bytes (0x10)");

#define BW_MAGIC 0x4F524231u      // "ORB1"
#define BW_LAUNCHER 0x4C41554Eu   // "LAUN": boot the factory app
#define BW_CHECK_XOR 0xA5C3961Eu

typedef struct {
    uint32_t magic, value, check, spare;
} bootword_t;

bool launcher_present(void)
{
    static int present = -1;   // the table does not change while running
    if (present < 0)
        present = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL) != NULL;
    return present;
}

void launcher_return(void)
{
    volatile bootword_t *w = (volatile bootword_t *)bootloader_common_get_rtc_retain_mem()->custom;
    // Magic last: a torn write is "no word", which boots this app again rather than anything else.
    w->magic = 0;
    w->value = BW_LAUNCHER;
    w->check = (BW_LAUNCHER * 0x9E3779B1u) ^ BW_CHECK_XOR;
    w->spare = 0;
    w->magic = BW_MAGIC;
    esp_restart();
}
