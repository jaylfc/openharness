// The WiFi half of the cable vocabulary: what `wifi.set` may carry and what `wifi.status` says.
//
// Pure message logic over cJSON, no radio and no NVS, so the length limits and the field rules are
// checked on a host (test/test_lan_wifi_msg.c). The contract with the daemon is WIFI_PROTOCOL.md in the
// waveshare-175c worktree; wifi_sta.c is what acts on it.
//
// THE PSK NEVER LEAVES THIS PATH ON THE WIRE OR IN A LOG. It is copied out of a `wifi.set` into a
// caller-owned buffer, handed to the driver and to NVS, and that is all: no status carries it, no
// function here formats it, and nothing in the LAN files passes a psk to a logging macro.
#pragma once

#include <stdbool.h>

#include "cJSON.h"

#define LAN_SSID_MAX 32   // bytes, per 802.11
#define LAN_PSK_MAX  63   // bytes; empty means an open network

typedef enum {
    LAN_WIFI_OFF = 0,
    LAN_WIFI_CONNECTING,
    LAN_WIFI_CONNECTED,
    LAN_WIFI_FAILED,
} lan_wifi_state_t;

typedef struct {
    lan_wifi_state_t state;
    char ssid[LAN_SSID_MAX + 1];   // "" when unknown
    bool has_rssi;
    int rssi;
    char ip[16];                   // "" unless connected
    char reason[12];               // "" unless failed
} lan_wifi_status_t;

typedef enum {
    LAN_WIFI_SET_OK = 0,
    LAN_WIFI_SET_BAD_SSID,   // missing, not a string, empty, or over 32 bytes
    LAN_WIFI_SET_BAD_PSK,    // present but not a string, or over 63 bytes
} lan_wifi_set_result_t;

// Validate a `wifi.set` body and copy ssid/psk out. A missing or null `psk` is an open network. On any
// failure both outputs are left empty.
lan_wifi_set_result_t lan_wifi_parse_set(const cJSON *body, char ssid[LAN_SSID_MAX + 1], char psk[LAN_PSK_MAX + 1]);

const char *lan_wifi_state_name(lan_wifi_state_t state);

// Add the status fields (everything but `t`) to `obj`: state always; ssid/rssi/ip/reason only when
// known, and reason only on `failed`. False if an allocation failed.
bool lan_wifi_status_fill(cJSON *obj, const lan_wifi_status_t *s);
