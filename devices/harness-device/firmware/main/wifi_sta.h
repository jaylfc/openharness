// The station half of the optional LAN transport: join ONE saved network and stay on it.
//
// No scan, no picker, no SoftAP, no on-device UI. The network arrives over the cable as `wifi.set`
// (cable_client.c), is stored in NVS, and is joined here — at boot if one is saved, immediately when it
// is set. Without a saved network the WiFi driver is never even initialised, so an unconfigured dial
// pays nothing for this file.
//
// An access point that refuses does not get hammered: every disconnect schedules the next attempt on a
// capped backoff (1 s, 2, 5, 10, 20, 40, then a flat minute), and a wrong password backs off from the
// first failure. The psk is never logged.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lan_wifi_msg.h"

// Create the state lock. Cheap and allocation-free apart from that, so it runs with the cable link, well
// before the radio: a `wifi.set` can arrive the moment the link is up.
void wifi_sta_prepare(void);

// Boot: if a network is saved, bring the driver up and join it. Otherwise does nothing. Call after the
// display and the cable are up (see app_main.c) — the driver takes tens of KB of internal RAM.
void wifi_sta_init(void);

// Store, then join — done on a short-lived task, because the caller is the cable's reader. The state
// reads `connecting` (or, if no task could be made, `failed`/`nomem`) as soon as this returns; whatever
// the join then does shows up as a state change. `ssid`/`psk` have already been length-checked
// (lan_wifi_parse_set). False only when the request could not be queued.
bool wifi_sta_set(const char *ssid, const char *psk);

// Erase the saved network, disconnect, stop the radio (also on that task). The state reads `off` at once.
void wifi_sta_forget(void);

// A snapshot of the state, for wifi.status and hello.wifi.
void wifi_sta_status(lan_wifi_status_t *out);

// Increments on every change of anything wifi_sta_status reports except the RSSI. The session task polls
// it to send a `wifi.status` on a state change without a callback into the cable's send path.
uint32_t wifi_sta_generation(void);
