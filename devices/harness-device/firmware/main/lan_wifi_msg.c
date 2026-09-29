#include "lan_wifi_msg.h"

#include <string.h>

lan_wifi_set_result_t lan_wifi_parse_set(const cJSON *body, char ssid[LAN_SSID_MAX + 1], char psk[LAN_PSK_MAX + 1])
{
    ssid[0] = '\0';
    psk[0] = '\0';
    if (!cJSON_IsObject(body)) return LAN_WIFI_SET_BAD_SSID;

    const cJSON *s = cJSON_GetObjectItemCaseSensitive(body, "ssid");
    if (!cJSON_IsString(s) || !s->valuestring) return LAN_WIFI_SET_BAD_SSID;
    const size_t sl = strlen(s->valuestring);
    if (sl < 1 || sl > LAN_SSID_MAX) return LAN_WIFI_SET_BAD_SSID;

    const cJSON *k = cJSON_GetObjectItemCaseSensitive(body, "psk");
    size_t kl = 0;
    if (k && !cJSON_IsNull(k)) {
        if (!cJSON_IsString(k) || !k->valuestring) return LAN_WIFI_SET_BAD_PSK;
        kl = strlen(k->valuestring);
        if (kl > LAN_PSK_MAX) return LAN_WIFI_SET_BAD_PSK;
    }

    memcpy(ssid, s->valuestring, sl + 1);
    if (kl) memcpy(psk, k->valuestring, kl + 1);
    return LAN_WIFI_SET_OK;
}

const char *lan_wifi_state_name(lan_wifi_state_t state)
{
    switch (state) {
    case LAN_WIFI_CONNECTING: return "connecting";
    case LAN_WIFI_CONNECTED:  return "connected";
    case LAN_WIFI_FAILED:     return "failed";
    case LAN_WIFI_OFF:
    default:                  return "off";
    }
}

bool lan_wifi_status_fill(cJSON *obj, const lan_wifi_status_t *s)
{
    if (!obj || !s) return false;
    if (!cJSON_AddStringToObject(obj, "state", lan_wifi_state_name(s->state))) return false;
    if (s->ssid[0] && !cJSON_AddStringToObject(obj, "ssid", s->ssid)) return false;
    if (s->has_rssi && s->state == LAN_WIFI_CONNECTED && !cJSON_AddNumberToObject(obj, "rssi", s->rssi)) return false;
    if (s->ip[0] && s->state == LAN_WIFI_CONNECTED && !cJSON_AddStringToObject(obj, "ip", s->ip)) return false;
    if (s->reason[0] && s->state == LAN_WIFI_FAILED && !cJSON_AddStringToObject(obj, "reason", s->reason)) return false;
    return true;
}
