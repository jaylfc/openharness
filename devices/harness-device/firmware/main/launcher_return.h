#pragma once
// Fork only. The way back to the taOS Orb launcher when this firmware is flashed as a guest app (ota_2/ota_3)
// beside it. The Orb's bootloader boots its factory app (the launcher) when the RTC boot word says LAUNCHER.
#include <stdbool.h>
bool launcher_present(void);   // the partition table has a factory app, so a launcher is there to return to
void launcher_return(void);    // write the LAUNCHER word and restart; does not return
