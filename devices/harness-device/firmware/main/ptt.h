// The physical keys (see ptt.c). BOOT (GPIO0) is back / stop the running turn / abort a recording on the
// press edge, and a hold of about 0.8 s also toggles the screen; the PWR key (where there is a PMIC)
// toggles the screen on a tap. Neither key starts or finishes a recording: voice is a touch gesture.
// (GPIO0 doubles as factory-reset, but ONLY when held at power-on.)
#pragma once

// Start the button poll task. Call after ui_set_voice().
void ptt_start(void);
