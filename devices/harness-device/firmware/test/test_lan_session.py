"""Drive the LAN half of cable_client.c: who may bind, who may set a network, and what is said back.

The functions under test are cut out of the production source and compiled against the real cJSON, the
real lan_bind.c and lan_wifi_msg.c; only the radio, NVS, the sockets and the session bookkeeping are
mocked. Needs IDF_PATH for cJSON (like test_cable_json_parse.py).
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

main = Path(__file__).resolve().parent / '../main'
json_dir = Path(os.environ['IDF_PATH']) / 'components/json/cJSON'
source = (main / 'cable_client.c').read_text()

def function(name):
    m = re.search(r'^static [^\n]*\b' + name + r'\([^;]*?\)\n\{.*?^\}', source, re.M | re.S)
    assert m, name
    return m.group(0) + '\n'

code = r'''
#include "cJSON.h"
#include "lan_bind.h"
#include "lan_wifi_msg.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef enum { CABLE_XPORT_NONE = 0, CABLE_XPORT_USB, CABLE_XPORT_TCP } cable_xport_t;
typedef enum { ESP_OK = 0 } esp_err_t;
static atomic_int s_xport;
static atomic_uint s_wifi_sent_gen;
static char s_bind[LAN_BIND_LEN + 1];

// Every log line, so "never logs the psk or the token" can be checked over the whole run.
static char logs[65536];
static void log_line(const char *fmt, ...) {
    va_list a; va_start(a, fmt);
    size_t n = strlen(logs);
    vsnprintf(logs + n, sizeof logs - n - 2, fmt, a);
    va_end(a);
    strcat(logs, "\n");
}
#define ESP_LOGI(tag, ...) log_line(__VA_ARGS__)
#define ESP_LOGW(tag, ...) log_line(__VA_ARGS__)
#define ESP_LOGD(tag, ...) log_line(__VA_ARGS__)
#define ESP_LOGE(tag, ...) log_line(__VA_ARGS__)
static const char *TAG = "t";

// ── mocks ──
static unsigned bind_saves, drops, downs, ups, set_calls, forget_calls;
static cable_xport_t up_via;
static char joined_ssid[64], joined_psk[80];
static lan_wifi_status_t live_status;
static uint32_t live_gen = 5;
static char sent[8][1024];
static cable_xport_t sent_to[8];
static unsigned sent_n;
static bool config_save_bind(const char *b) { (void)b; bind_saves++; return true; }
static void wifi_cable_drop(void) { drops++; }
static void session_up(const cJSON *p, cable_xport_t via) { (void)p; ups++; up_via = via; atomic_store(&s_xport, (int)via); }
static void session_down(const char *why) { (void)why; downs++; atomic_store(&s_xport, (int)CABLE_XPORT_NONE); }
static bool wifi_sta_set(const char *ssid, const char *psk) {
    set_calls++; snprintf(joined_ssid, sizeof joined_ssid, "%s", ssid); snprintf(joined_psk, sizeof joined_psk, "%s", psk);
    live_status.state = LAN_WIFI_CONNECTING; snprintf(live_status.ssid, sizeof live_status.ssid, "%s", ssid); live_gen++;
    return true;
}
static void wifi_sta_forget(void) { forget_calls++; memset(&live_status, 0, sizeof live_status); live_gen++; }
static void wifi_sta_status(lan_wifi_status_t *o) { *o = live_status; }
static uint32_t wifi_sta_generation(void) { return live_gen; }
static bool cable_link_send_to(cable_xport_t x, uint8_t type, const uint8_t *p, size_t n) {
    (void)type; assert(sent_n < 8 && n < sizeof sent[0]);
    memcpy(sent[sent_n], p, n); sent[sent_n][n] = 0; sent_to[sent_n++] = x; return true;
}
#define CABLE_TYPE_JSON 1
'''
code += function('str_of') + function('msg') + function('send_json_to').replace('static bool send_json_to', 'static bool send_json_to')
code += function('send_wifi_status') + function('handle_welcome') + function('handle_wifi_set') + function('handle_wifi_forget')
code += r'''
static const char *BIND_A = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char *BIND_B = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static const char *SECRET_PSK = "hunter2-hunter2";

static void welcome(cable_xport_t via, const char *bind) {
    cJSON *o = cJSON_CreateObject(); cJSON_AddStringToObject(o, "t", "welcome");
    if (bind) cJSON_AddStringToObject(o, "bind", bind);
    handle_welcome(o, via); cJSON_Delete(o);
}
static void wifi_set(cable_xport_t via, const char *ssid, const char *psk) {
    cJSON *o = cJSON_CreateObject(); cJSON_AddStringToObject(o, "t", "wifi.set");
    if (ssid) cJSON_AddStringToObject(o, "ssid", ssid);
    if (psk) cJSON_AddStringToObject(o, "psk", psk);
    handle_wifi_set(o, via); cJSON_Delete(o);
}
static cJSON *last_sent(cable_xport_t *to) {
    assert(sent_n); if (to) *to = sent_to[sent_n - 1]; return cJSON_Parse(sent[sent_n - 1]);
}

int main(void) {
    // ── bind: stored on USB (only on change), verified on TCP ──
    welcome(CABLE_XPORT_USB, NULL);                       assert(!bind_saves && ups == 1 && up_via == CABLE_XPORT_USB);
    welcome(CABLE_XPORT_USB, "not-a-token");              assert(!bind_saves && !s_bind[0]);
    welcome(CABLE_XPORT_USB, BIND_A);                     assert(bind_saves == 1 && !strcmp(s_bind, BIND_A));
    welcome(CABLE_XPORT_USB, BIND_A);                     assert(bind_saves == 1);   // welcome repeats: no rewrite
    welcome(CABLE_XPORT_USB, BIND_B);                     assert(bind_saves == 2 && !strcmp(s_bind, BIND_B));   // last host wins
    atomic_store(&s_xport, CABLE_XPORT_NONE);

    unsigned u = ups, d = drops;
    welcome(CABLE_XPORT_TCP, BIND_A);                     assert(ups == u && drops == d + 1);   // wrong token: dropped
    welcome(CABLE_XPORT_TCP, NULL);                       assert(ups == u && drops == d + 2);   // no token: dropped
    welcome(CABLE_XPORT_TCP, "");                         assert(ups == u && drops == d + 3);
    welcome(CABLE_XPORT_TCP, "B123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
                                                          assert(ups == u && drops == d + 4);   // upper case is not the token
    welcome(CABLE_XPORT_TCP, BIND_B);                     assert(ups == u + 1 && up_via == CABLE_XPORT_TCP && drops == d + 4);
    assert(bind_saves == 2);                              // a TCP welcome never writes a bind
    welcome(CABLE_XPORT_TCP, BIND_B);                     assert(ups == u + 2);   // repeats are fine on the same wire

    // USB wins: the cable's welcome takes a LAN session over, and closes the socket.
    d = drops; unsigned dn = downs;
    welcome(CABLE_XPORT_USB, BIND_B);
    assert(downs == dn + 1 && drops == d + 1 && up_via == CABLE_XPORT_USB);
    // ...and a LAN welcome against a live USB session is refused whatever it carries.
    u = ups; d = drops;
    welcome(CABLE_XPORT_TCP, BIND_B);                     assert(ups == u && drops == d + 1);

    // With nothing stored, no LAN session is ever accepted.
    atomic_store(&s_xport, CABLE_XPORT_NONE); s_bind[0] = 0; u = ups;
    welcome(CABLE_XPORT_TCP, BIND_B);                     assert(ups == u);
    welcome(CABLE_XPORT_TCP, NULL);                       assert(ups == u);
    strcpy(s_bind, BIND_B);

    // ── wifi.set: cable only ──
    wifi_set(CABLE_XPORT_TCP, "net", SECRET_PSK);         assert(!set_calls && !sent_n);
    { cJSON *o = cJSON_Parse("{\"t\":\"wifi.forget\"}"); handle_wifi_forget(CABLE_XPORT_TCP); cJSON_Delete(o); }
    assert(!forget_calls && !sent_n);
    assert(strstr(logs, "wifi.set ignored") && strstr(logs, "wifi.forget ignored"));

    cable_xport_t to;
    wifi_set(CABLE_XPORT_USB, "net", SECRET_PSK);
    assert(set_calls == 1 && !strcmp(joined_ssid, "net") && !strcmp(joined_psk, SECRET_PSK));
    { cJSON *r = last_sent(&to); assert(to == CABLE_XPORT_USB);
      assert(!strcmp(cJSON_GetObjectItem(r, "t")->valuestring, "wifi.status"));
      assert(!strcmp(cJSON_GetObjectItem(r, "state")->valuestring, "connecting"));
      assert(!strcmp(cJSON_GetObjectItem(r, "ssid")->valuestring, "net"));
      assert(!cJSON_GetObjectItem(r, "psk") && !cJSON_GetObjectItem(r, "reason"));
      cJSON_Delete(r); }
    assert(atomic_load(&s_wifi_sent_gen) == live_gen);   // the session task will not repeat it

    // The reply goes back on the wire the request came in on, not on the active one.
    atomic_store(&s_xport, CABLE_XPORT_TCP);
    wifi_set(CABLE_XPORT_USB, "net2", "");
    { cJSON *r = last_sent(&to); assert(to == CABLE_XPORT_USB); cJSON_Delete(r); }
    assert(!strcmp(joined_ssid, "net2") && !joined_psk[0]);   // empty psk = open network
    atomic_store(&s_xport, CABLE_XPORT_NONE);

    // Bad input: nothing is stored or joined, and the answer is `failed`/`invalid` with no ssid.
    unsigned sets = set_calls;
    char long_ssid[40], long_psk[80]; memset(long_ssid, 's', 33); long_ssid[33] = 0; memset(long_psk, 'p', 64); long_psk[64] = 0;
    wifi_set(CABLE_XPORT_USB, NULL, "x");       wifi_set(CABLE_XPORT_USB, "", "x");
    wifi_set(CABLE_XPORT_USB, long_ssid, "x");  wifi_set(CABLE_XPORT_USB, "net", long_psk);
    assert(set_calls == sets);
    { cJSON *r = last_sent(&to);
      assert(!strcmp(cJSON_GetObjectItem(r, "state")->valuestring, "failed"));
      assert(!strcmp(cJSON_GetObjectItem(r, "reason")->valuestring, "invalid"));
      assert(!cJSON_GetObjectItem(r, "ssid")); cJSON_Delete(r); }
    memset(long_ssid, 's', 32); long_ssid[32] = 0; memset(long_psk, 'p', 63); long_psk[63] = 0;
    wifi_set(CABLE_XPORT_USB, long_ssid, long_psk);       assert(set_calls == sets + 1);   // the limits themselves are fine

    // ── wifi.forget ──
    handle_wifi_forget(CABLE_XPORT_USB);
    assert(forget_calls == 1);
    { cJSON *r = last_sent(&to); assert(!strcmp(cJSON_GetObjectItem(r, "state")->valuestring, "off"));
      assert(!cJSON_GetObjectItem(r, "ssid")); cJSON_Delete(r); }

    // ── nothing sensitive was ever logged or sent ──
    assert(!strstr(logs, SECRET_PSK) && !strstr(logs, BIND_A) && !strstr(logs, BIND_B));
    for (unsigned i = 0; i < sent_n; i++) assert(!strstr(sent[i], SECRET_PSK) && !strstr(sent[i], "psk"));
    puts("LAN session: bind stored on USB only and only on change, TCP accepted only with the stored bind, USB takes over, wifi.set/forget cable-only, replies on the requesting wire, no psk or token in logs or statuses PASS");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='harness-lan-session-') as folder:
    out = Path(folder)
    (out / 'lan.c').write_text(code)
    flags = ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
             '-fsanitize=' + os.environ.get('SANITIZERS', 'undefined,bounds')]
    subprocess.run(flags + ['-Wno-deprecated-declarations', '-Wno-unused-function', '-Wno-unused-variable', '-Wno-unused-parameter',
                            '-I', str(main), '-I', str(json_dir), str(out / 'lan.c'), str(main / 'lan_bind.c'),
                            str(main / 'lan_wifi_msg.c'), str(json_dir / 'cJSON.c'), '-o', str(out / 'lan')], check=True)
    subprocess.run([str(out / 'lan')], check=True)
