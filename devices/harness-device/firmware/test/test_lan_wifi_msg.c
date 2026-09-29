// wifi.set validation and wifi.status building (main/lan_wifi_msg.c), against ESP-IDF's cJSON.
#include "lan_wifi_msg.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lan_wifi_set_result_t parse(const char *json, char *ssid, char *psk)
{
    cJSON *o = cJSON_Parse(json);
    assert(o);
    lan_wifi_set_result_t r = lan_wifi_parse_set(o, ssid, psk);
    cJSON_Delete(o);
    return r;
}

static char *status_json(const lan_wifi_status_t *st)
{
    cJSON *o = cJSON_CreateObject();
    assert(o && cJSON_AddStringToObject(o, "t", "wifi.status") && lan_wifi_status_fill(o, st));
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

int main(void)
{
    char ssid[LAN_SSID_MAX + 1], psk[LAN_PSK_MAX + 1], big[200], json[400];

    // ── wifi.set ──
    assert(parse("{\"ssid\":\"net\",\"psk\":\"secret-pass\"}", ssid, psk) == LAN_WIFI_SET_OK);
    assert(!strcmp(ssid, "net") && !strcmp(psk, "secret-pass"));
    assert(parse("{\"t\":\"wifi.set\",\"ssid\":\"open net\",\"psk\":\"\"}", ssid, psk) == LAN_WIFI_SET_OK && !psk[0]);
    assert(parse("{\"ssid\":\"n\"}", ssid, psk) == LAN_WIFI_SET_OK && !psk[0]);            // missing psk = open
    assert(parse("{\"ssid\":\"n\",\"psk\":null}", ssid, psk) == LAN_WIFI_SET_OK && !psk[0]);
    assert(parse("{\"ssid\":\"caf\\u00e9\",\"psk\":\"\\u00e9\"}", ssid, psk) == LAN_WIFI_SET_OK);   // bytes, not characters

    // The limits, both sides: ssid 1..32 bytes, psk 0..63 bytes.
    for (int n = 0; n <= 40; n++) {
        memset(big, 's', (size_t)n); big[n] = 0;
        snprintf(json, sizeof json, "{\"ssid\":\"%s\",\"psk\":\"\"}", big);
        const lan_wifi_set_result_t r = parse(json, ssid, psk);
        if (n >= 1 && n <= LAN_SSID_MAX) { assert(r == LAN_WIFI_SET_OK && strlen(ssid) == (size_t)n); }
        else { assert(r == LAN_WIFI_SET_BAD_SSID && !ssid[0] && !psk[0]); }
    }
    for (int n = 0; n <= 70; n++) {
        memset(big, 'p', (size_t)n); big[n] = 0;
        snprintf(json, sizeof json, "{\"ssid\":\"net\",\"psk\":\"%s\"}", big);
        const lan_wifi_set_result_t r = parse(json, ssid, psk);
        if (n <= LAN_PSK_MAX) { assert(r == LAN_WIFI_SET_OK && strlen(psk) == (size_t)n); }
        else { assert(r == LAN_WIFI_SET_BAD_PSK && !ssid[0] && !psk[0]); }   // nothing partial is left behind
    }
    // 33 bytes of ssid that is 11 characters must still be refused: the limit is in bytes.
    assert(parse("{\"ssid\":\"\\u20ac\\u20ac\\u20ac\\u20ac\\u20ac\\u20ac\\u20ac\\u20ac\\u20ac\\u20ac\\u20ac\",\"psk\":\"\"}", ssid, psk)
           == LAN_WIFI_SET_BAD_SSID);

    assert(parse("{}", ssid, psk) == LAN_WIFI_SET_BAD_SSID);
    assert(parse("{\"ssid\":5}", ssid, psk) == LAN_WIFI_SET_BAD_SSID);
    assert(parse("{\"ssid\":null}", ssid, psk) == LAN_WIFI_SET_BAD_SSID);
    assert(parse("{\"ssid\":[\"a\"]}", ssid, psk) == LAN_WIFI_SET_BAD_SSID);
    assert(parse("{\"ssid\":\"n\",\"psk\":7}", ssid, psk) == LAN_WIFI_SET_BAD_PSK);
    assert(parse("{\"ssid\":\"n\",\"psk\":true}", ssid, psk) == LAN_WIFI_SET_BAD_PSK);
    assert(parse("{\"SSID\":\"n\"}", ssid, psk) == LAN_WIFI_SET_BAD_SSID);                  // case-sensitive keys
    assert(lan_wifi_parse_set(NULL, ssid, psk) == LAN_WIFI_SET_BAD_SSID);
    cJSON *arr = cJSON_Parse("[]");
    assert(lan_wifi_parse_set(arr, ssid, psk) == LAN_WIFI_SET_BAD_SSID);
    cJSON_Delete(arr);

    // ── wifi.status ──
    lan_wifi_status_t st;
    memset(&st, 0, sizeof st);
    char *s = status_json(&st);
    assert(!strcmp(s, "{\"t\":\"wifi.status\",\"state\":\"off\"}"));
    free(s);

    st.state = LAN_WIFI_CONNECTING; strcpy(st.ssid, "net");
    s = status_json(&st);
    assert(!strcmp(s, "{\"t\":\"wifi.status\",\"state\":\"connecting\",\"ssid\":\"net\"}"));
    free(s);

    st.state = LAN_WIFI_CONNECTED; st.has_rssi = true; st.rssi = -52; strcpy(st.ip, "192.0.2.7");
    s = status_json(&st);
    assert(!strcmp(s, "{\"t\":\"wifi.status\",\"state\":\"connected\",\"ssid\":\"net\",\"rssi\":-52,\"ip\":\"192.0.2.7\"}"));
    free(s);

    // reason only on failed; ip/rssi only when connected, even if stale values are still in the struct.
    st.state = LAN_WIFI_FAILED; strcpy(st.reason, "auth");
    s = status_json(&st);
    assert(!strcmp(s, "{\"t\":\"wifi.status\",\"state\":\"failed\",\"ssid\":\"net\",\"reason\":\"auth\"}"));
    free(s);
    st.state = LAN_WIFI_CONNECTING;
    s = status_json(&st);
    assert(!strstr(s, "reason") && !strstr(s, "ip") && !strstr(s, "rssi"));
    free(s);

    // A hostile ssid is escaped by cJSON, never spliced into the frame.
    memset(&st, 0, sizeof st); st.state = LAN_WIFI_CONNECTING; strcpy(st.ssid, "a\"b\\c\n");
    s = status_json(&st);
    cJSON *back = cJSON_Parse(s);
    assert(back && !strcmp(cJSON_GetObjectItem(back, "ssid")->valuestring, "a\"b\\c\n"));
    cJSON_Delete(back); free(s);

    assert(!strcmp(lan_wifi_state_name(LAN_WIFI_OFF), "off") && !strcmp(lan_wifi_state_name(LAN_WIFI_CONNECTING), "connecting") &&
           !strcmp(lan_wifi_state_name(LAN_WIFI_CONNECTED), "connected") && !strcmp(lan_wifi_state_name(LAN_WIFI_FAILED), "failed"));
    assert(!lan_wifi_status_fill(NULL, &st));

    puts("lan_wifi_msg: wifi.set limits in bytes on both sides, type errors, status fields per state, escaping PASS");
    return 0;
}
