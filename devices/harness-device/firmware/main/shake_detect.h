// Deliberate double-shake detector. Pure: no ESP-IDF, no clock of its own, no allocation. The caller
// feeds accelerometer samples with a millisecond timestamp and the display's asleep flag, and gets back
// true exactly when a double shake has just been recognised. Host-tested in test/test_shake_detect.c.
//
// WHAT COUNTS. A "jolt" is a sample whose |a| differs from 1 g by at least SHAKE_JOLT_MG, entered from
// below SHAKE_RELEASE_MG (an edge, not a level) and at least SHAKE_MIN_GAP_MS after the previous jolt,
// so one knock's ringing is one jolt. Tilting the dial keeps |a| at 1 g however slowly or quickly the
// gravity vector turns, so a tilt never makes a jolt at all.
//
// WHAT FIRES. A group of jolts fires once SHAKE_SETTLE_MS has passed after its last jolt, when:
//   - it holds at least two jolts, each no more than SHAKE_PAIR_GAP_MS after the one before,
//   - it began after SHAKE_QUIET_MS without any strong sample (a shake out of stillness),
//   - it ended within SHAKE_GROUP_MAX_MS of its first jolt.
// Waiting for the settle is what tells a shake from walking: a stride train keeps producing jolts about
// every 500 ms, so its group never settles and runs past SHAKE_GROUP_MAX_MS, which discards it and
// demands a fresh stillness before anything can start again. A single knock is a group of one and expires.
//
// AFTER A FIRE. SHAKE_COOLDOWN_MS of nothing, and because the stillness rule reads the last strong
// sample, the tail of the same shake cannot start a new group either.
//
// ASLEEP. While the screen is off nothing is counted and nothing fires; the state is cleared and the
// stillness clock restarted, so a shake that wakes nothing cannot arm a menu the moment the screen
// comes back. Only BOOT and PWR wake the dial.
//
// TIME. uint32_t milliseconds compared as (int32_t)(a - b), so a wrap of the caller's clock is harmless.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// ---- thresholds: compile-time, all in milliseconds or milli-g -----------------------------------------
// TUNING: lower SHAKE_JOLT_MG to make it more sensitive (1500 = |a| beyond 2.5 g or 0 g-ish; the sensor
// is set to +-4 g, so the useful ceiling is about 3000). Raise SHAKE_QUIET_MS / SHAKE_SETTLE_MS to be
// stricter about walking. Override any of them with -D at build time.
#ifndef SHAKE_JOLT_MG
#define SHAKE_JOLT_MG      1500   // |  |a| - 1000 | at or above this is a jolt
#endif
#ifndef SHAKE_RELEASE_MG
#define SHAKE_RELEASE_MG   600    // the deviation must fall below this before the next jolt can count
#endif
#ifndef SHAKE_MIN_GAP_MS
#define SHAKE_MIN_GAP_MS   120    // jolt start to next jolt start
#endif
#ifndef SHAKE_PAIR_GAP_MS
#define SHAKE_PAIR_GAP_MS  1200   // longest pause inside one shake
#endif
#ifndef SHAKE_SETTLE_MS
#define SHAKE_SETTLE_MS    600    // stillness after the last jolt before the shake is accepted
#endif
#ifndef SHAKE_GROUP_MAX_MS
#define SHAKE_GROUP_MAX_MS 2400   // first jolt to last jolt; longer is a vibration, not a gesture
#endif
#ifndef SHAKE_QUIET_MS
#define SHAKE_QUIET_MS     1500   // no strong sample this long before the first jolt
#endif
#ifndef SHAKE_COOLDOWN_MS
#define SHAKE_COOLDOWN_MS  2000   // after a fire, nothing fires
#endif
#define SHAKE_STALE_MS     (1u << 28)   // an old timestamp is forgotten before a clock wrap can alias it

typedef struct {
    bool has_strong, started, in_jolt, in_group, cooling;
    uint32_t last_strong, last_start, group_first, group_last, cool_until;
    uint8_t jolts;
} ht_shake_t;

void ht_shake_reset(ht_shake_t *s);
// |a| in milli-g, from the three axes in milli-g. Integer only.
uint32_t ht_shake_magnitude_mg(int ax, int ay, int az);
// One sample. Returns true once per recognised double shake. `asleep` is display_is_asleep().
bool ht_shake_feed(ht_shake_t *s, uint32_t now_ms, int ax_mg, int ay_mg, int az_mg, bool asleep);
