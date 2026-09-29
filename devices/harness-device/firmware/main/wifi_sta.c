#include "wifi_sta.h"

#include <stdio.h>
#include <string.h>

#include "config_store.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "device_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "wifi_cable.h"

static const char *TAG = "wifi_sta";

static SemaphoreHandle_t s_mu;
static bool s_driver;           // esp_wifi_init() has run (it is never undone; see wifi_sta_forget)
static bool s_started;          // esp_wifi_start() is in effect
static esp_netif_t *s_netif;
static esp_timer_handle_t s_retry_timer;

// Everything below is guarded by s_mu.
static bool s_want;             // a network is saved and should be joined
static lan_wifi_state_t s_state = LAN_WIFI_OFF;
static char s_ssid[LAN_SSID_MAX + 1];
static char s_ip[16];
static char s_reason[12];
static int s_streak;            // consecutive failures since the last address
static int64_t s_ignore_leave_until_us;   // our own disconnect() echoes as ASSOC_LEAVE; see on_disconnected
static uint32_t s_gen;
static bool s_worker;           // the request task is running (see "requests from the cable")

static void lock(void) { xSemaphoreTake(s_mu, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_mu); }

// Caller holds the lock. Any change a listener could see bumps the generation.
static void set_state(lan_wifi_state_t st, const char *reason, const char *ip)
{
    const char *r = reason ? reason : "";
    const char *i = ip ? ip : "";
    if (s_state == st && !strcmp(s_reason, r) && !strcmp(s_ip, i)) return;
    s_state = st;
    snprintf(s_reason, sizeof(s_reason), "%s", r);
    snprintf(s_ip, sizeof(s_ip), "%s", i);
    s_gen++;
}

// 1 s, 2, 5, 10, 20, 40, then a flat minute. The cap matters more than the curve: what is being waited
// on is usually a router coming back, and a dial that has been sulking for an hour should still notice
// within a minute of the AP returning.
static uint32_t backoff_ms(int streak)
{
    static const uint32_t steps[] = { 1000, 2000, 5000, 10000, 20000, 40000, 60000 };
    const int n = (int)(sizeof(steps) / sizeof(steps[0]));
    if (streak < 1) streak = 1;
    return steps[streak > n ? n - 1 : streak - 1];
}

static const char *reason_word(int reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
    case WIFI_REASON_802_1X_AUTH_FAILED:
    case WIFI_REASON_AUTH_EXPIRE:
        return "auth";
    case WIFI_REASON_NO_AP_FOUND:
        return "no-ap";
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_ASSOC_EXPIRE:
        return "assoc";
    case WIFI_REASON_BEACON_TIMEOUT:
    case WIFI_REASON_ASSOC_LEAVE:
    case WIFI_REASON_AP_TSF_RESET:
        return "lost";
    default:
        return "timeout";
    }
}

static void retry_cb(void *arg)
{
    (void)arg;
    lock();
    const bool want = s_want;
    unlock();
    if (!want) return;
    const esp_err_t e = esp_wifi_connect();
    if (e != ESP_OK && e != ESP_ERR_WIFI_CONN) ESP_LOGD(TAG, "connect: %s", esp_err_to_name(e));
}

static void schedule_retry(uint32_t ms)
{
    if (!s_retry_timer) return;
    esp_timer_stop(s_retry_timer);
    esp_timer_start_once(s_retry_timer, (uint64_t)ms * 1000);
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id;
    const ip_event_got_ip_t *e = data;
    char ip[16];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&e->ip_info.ip));
    lock();
    s_streak = 0;
    set_state(LAN_WIFI_CONNECTED, NULL, ip);
    unlock();
    ESP_LOGI(TAG, "connected, ip %s", ip);
    wifi_cable_on_sta_up();
}

static void on_disconnected(const wifi_event_sta_disconnected_t *e)
{
    const int reason = e ? e->reason : 0;
    lock();
    if (!s_want) { unlock(); return; }
    // Our own esp_wifi_disconnect() (a new network, or a forget racing a retry) comes back as
    // ASSOC_LEAVE. Counting it as a failure would schedule a second, redundant attempt. Only inside a
    // short window after we asked for it: an AP that really sends a leave still gets the retry.
    if (reason == WIFI_REASON_ASSOC_LEAVE && esp_timer_get_time() < s_ignore_leave_until_us) {
        unlock();
        return;
    }
    s_streak++;
    const char *word = reason_word(reason);
    const bool auth = strcmp(word, "auth") == 0;
    // A wrong password is not going to fix itself, and a router rebooting is: both wait, but only the
    // first says `failed` from the start. A transient drop reads `connecting` for a couple of tries.
    uint32_t delay = backoff_ms(s_streak);
    if (auth && delay < 5000) delay = 5000;
    if (auth || s_streak >= 3) set_state(LAN_WIFI_FAILED, word, NULL);
    else set_state(LAN_WIFI_CONNECTING, NULL, NULL);
    const int streak = s_streak;
    unlock();
    wifi_cable_on_sta_down();
    ESP_LOGW(TAG, "disconnected (reason %d, %s), attempt %d, retry in %u ms", reason, word, streak, (unsigned)delay);
    schedule_retry(delay);
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    if (id == WIFI_EVENT_STA_START) {
        lock();
        const bool want = s_want;
        unlock();
        if (want) esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        on_disconnected(data);
    }
}

// Bring the driver up once. Buffers are sized in sdkconfig.defaults (few static RX buffers, everything
// else preferring PSRAM); this only wires the events and turns the radio into a bare station.
static bool ensure_driver(void)
{
    if (s_driver) return true;
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) { ESP_LOGE(TAG, "netif_init: %s", esp_err_to_name(err)); return false; }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(err)); return false; }
    s_netif = esp_netif_create_default_wifi_sta();
    if (!s_netif) { ESP_LOGE(TAG, "no netif"); return false; }

    char mac[DEVICE_MAC_STR_LEN];
    if (device_mac_str(mac, sizeof(mac))) {
        char host[24];
        snprintf(host, sizeof(host), "harness-%c%c%c%c", mac[12], mac[13], mac[15], mac[16]);
        esp_netif_set_hostname(s_netif, host);
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) { ESP_LOGE(TAG, "wifi_init: %s", esp_err_to_name(err)); return false; }
    // Credentials live in config_store's "lan" namespace and nowhere else, so a factory reset finds them
    // all. The driver must not keep its own copy in NVS.
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_got_ip, NULL);
    esp_wifi_set_mode(WIFI_MODE_STA);

    const esp_timer_create_args_t targs = { .callback = retry_cb, .name = "wifi_retry" };
    if (esp_timer_create(&targs, &s_retry_timer) != ESP_OK) s_retry_timer = NULL;
    s_driver = true;
    return true;
}

static bool apply_and_start(const char *ssid, const char *psk)
{
    wifi_config_t wcfg = { 0 };
    memcpy(wcfg.sta.ssid, ssid, strlen(ssid));   // lengths were validated: <= 32 and <= 63
    memcpy(wcfg.sta.password, psk, strlen(psk));
    wcfg.sta.threshold.authmode = psk[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wcfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    wcfg.sta.pmf_cfg.capable = true;
    const esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wcfg);
    memset(&wcfg, 0, sizeof(wcfg));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_config: %s", esp_err_to_name(err));   // no psk in this line, ever
        return false;
    }
    return true;
}

static void mark_failed(const char *reason)
{
    lock();
    set_state(LAN_WIFI_FAILED, reason, NULL);
    unlock();
}

// Join what is in s_ssid/psk. The radio is started if it was stopped, otherwise a connect is scheduled
// shortly after the disconnect so the old association's leave has been seen first.
static bool join(const char *ssid, const char *psk)
{
    if (!ensure_driver()) { mark_failed("nomem"); return false; }
    if (!apply_and_start(ssid, psk)) { mark_failed("config"); return false; }
    lock();
    s_want = true;
    s_streak = 0;
    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);   // (also set by wifi_sta_set, ahead of this task)
    set_state(LAN_WIFI_CONNECTING, NULL, NULL);
    unlock();
    if (!s_started) {
        const esp_err_t err = esp_wifi_start();   // STA_START then connects
        if (err != ESP_OK) { ESP_LOGE(TAG, "wifi_start: %s", esp_err_to_name(err)); mark_failed("nomem"); return false; }
        s_started = true;
    } else {
        lock();
        s_ignore_leave_until_us = esp_timer_get_time() + 2000000;
        unlock();
        wifi_cable_on_sta_down();
        esp_wifi_disconnect();
        schedule_retry(500);
    }
    ESP_LOGI(TAG, "joining '%s'", ssid);
    return true;
}

void wifi_sta_prepare(void)
{
    if (!s_mu) s_mu = xSemaphoreCreateMutex();
}

void wifi_sta_init(void)
{
    if (!s_mu) return;
    lock();
    const bool busy = s_want || s_worker;   // a wifi.set got there first: it already joined the newer network
    unlock();
    if (busy) return;
    char ssid[CFG_WIFI_SSID_MAX], psk[CFG_WIFI_PSK_MAX];
    if (config_load_wifi(ssid, sizeof(ssid), psk, sizeof(psk))) join(ssid, psk);
    else ESP_LOGI(TAG, "no saved network, wifi stays off");
    memset(psk, 0, sizeof(psk));
}

// ── requests from the cable ─────────────────────────────────────────────────────────────────────────
// wifi.set and wifi.forget arrive on the USB reader, whose stack is sized for the message layer and not
// for esp_wifi_init() + a flash write. So the caller only records the intent (and the visible state, so
// the immediate wifi.status reply is right) and a short-lived task does the work, one request at a time;
// a request that arrives while one is running replaces the pending one.
typedef struct {
    enum { REQ_NONE = 0, REQ_SET, REQ_FORGET } op;
    char ssid[LAN_SSID_MAX + 1];
    char psk[LAN_PSK_MAX + 1];
} request_t;
static request_t s_req;

static void do_forget(void)
{
    config_clear_wifi();
    lock();
    s_want = false;
    s_streak = 0;
    unlock();
    wifi_cable_on_sta_down();
    if (s_retry_timer) esp_timer_stop(s_retry_timer);
    if (s_started) {
        esp_wifi_disconnect();
        esp_wifi_stop();   // the driver stays initialised: tearing it down and back up is where leaks live
        s_started = false;
    }
    ESP_LOGI(TAG, "forgot the saved network");
}

static void do_set(const char *ssid, const char *psk)
{
    // Join first: a network the driver refuses outright (a psk it cannot use) must not be kept in flash
    // to fail the same way on every boot.
    if (!join(ssid, psk)) return;
    if (!config_save_wifi(ssid, psk)) ESP_LOGW(TAG, "could not persist the network; it will not survive a reboot");
}

static void worker_task(void *arg)
{
    (void)arg;
    while (1) {
        lock();
        request_t req = s_req;
        memset(&s_req, 0, sizeof(s_req));
        if (req.op == REQ_NONE) { s_worker = false; unlock(); break; }
        unlock();
        if (req.op == REQ_SET) do_set(req.ssid, req.psk);
        else do_forget();
        memset(&req, 0, sizeof(req));
    }
    vTaskDelete(NULL);
}

static bool submit(const request_t *r)
{
    lock();
    s_req = *r;
    const bool spawn = !s_worker;
    if (spawn) s_worker = true;
    unlock();
    if (spawn && xTaskCreate(worker_task, "wifi_cfg", 6144, NULL, 3, NULL) != pdPASS) {
        lock();
        s_worker = false;
        memset(&s_req, 0, sizeof(s_req));
        unlock();
        return false;
    }
    return true;
}

bool wifi_sta_set(const char *ssid, const char *psk)
{
    if (!s_mu) return false;   // wifi_sta_prepare() has not run: no LAN transport in this image
    request_t r = { .op = REQ_SET };
    snprintf(r.ssid, sizeof(r.ssid), "%s", ssid);
    snprintf(r.psk, sizeof(r.psk), "%s", psk);
    lock();
    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    set_state(LAN_WIFI_CONNECTING, NULL, NULL);
    unlock();
    const bool ok = submit(&r);
    memset(&r, 0, sizeof(r));
    if (!ok) mark_failed("nomem");
    return ok;
}

void wifi_sta_forget(void)
{
    if (!s_mu) return;
    request_t r = { .op = REQ_FORGET };
    lock();
    s_ssid[0] = '\0';
    set_state(LAN_WIFI_OFF, NULL, NULL);
    unlock();
    submit(&r);
}

void wifi_sta_status(lan_wifi_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!s_mu) return;   // never initialised: off
    lock();
    out->state = s_state;
    snprintf(out->ssid, sizeof(out->ssid), "%s", s_ssid);
    snprintf(out->ip, sizeof(out->ip), "%s", s_ip);
    snprintf(out->reason, sizeof(out->reason), "%s", s_reason);
    unlock();
    if (out->state == LAN_WIFI_CONNECTED) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) { out->has_rssi = true; out->rssi = ap.rssi; }
    }
}

uint32_t wifi_sta_generation(void)
{
    if (!s_mu) return 0;
    lock();
    const uint32_t g = s_gen;
    unlock();
    return g;
}
