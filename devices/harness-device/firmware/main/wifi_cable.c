#include "wifi_cable.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cable_link.h"
#include "device_mac.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mdns.h"

static const char *TAG = "wifi_cable";

static cable_frame_cb       s_on_frame;
static wifi_cable_closed_cb s_on_closed;

static int s_listen = -1;
static int s_client = -1;
static int64_t s_client_since_us;
static bool s_had_client;            // a client existed since the last on_closed
static bool s_mdns_up;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_sock_mu;
static volatile bool s_want_up;
static cable_decoder_t *s_decoder;   // PSRAM: 8 KB that only the LAN task touches

static void sock_lock(void) { xSemaphoreTake(s_sock_mu, portMAX_DELAY); }
static void sock_unlock(void) { xSemaphoreGive(s_sock_mu); }

static void sock_close(int *fd)
{
    sock_lock();
    if (*fd >= 0) {
        shutdown(*fd, SHUT_RDWR);
        close(*fd);
        *fd = -1;
    }
    sock_unlock();
}

bool wifi_cable_client(void) { return s_client >= 0; }

void wifi_cable_drop(void) { if (s_sock_mu) sock_close(&s_client); }

bool wifi_cable_write(const uint8_t *data, size_t n)
{
    if (!data || !n || !s_sock_mu) return false;
    sock_lock();
    const int fd = s_client;
    size_t sent = 0;
    int err = 0;
    while (fd >= 0 && sent < n) {
        // The socket has a send timeout (see adopt), so a peer that stops reading costs one bounded
        // wait and a closed connection, never a task parked on the transmit lock.
        const int r = send(fd, data + sent, n - sent, 0);
        if (r <= 0) { err = errno; break; }
        sent += (size_t)r;
    }
    const bool ok = fd >= 0 && sent == n;
    if (fd >= 0 && !ok) {
        shutdown(fd, SHUT_RDWR);
        close(fd);
        s_client = -1;
    }
    sock_unlock();
    // Logged after the unlock. Logs go to the USB sink, which takes the transmit lock — the lock our
    // caller already holds — so this cannot deadlock, but there is no reason to hold the socket lock
    // across a log line either.
    if (fd >= 0 && !ok) ESP_LOGW(TAG, "tcp send failed (errno %d)", err);
    return ok;
}

// ── mDNS ────────────────────────────────────────────────────────────────────────────────────────────

static void mdns_start(void)
{
    char mac[DEVICE_MAC_STR_LEN];
    if (!device_mac_str(mac, sizeof(mac))) return;   // no identity, nothing honest to advertise
    // harness-<last 4 hex of the MAC>: the last two octets of AA:BB:CC:DD:EE:FF.
    char host[24];
    snprintf(host, sizeof(host), "harness-%c%c%c%c", mac[12], mac[13], mac[15], mac[16]);
    esp_err_t err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "mdns_init: %s", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set(host);
    mdns_instance_name_set("Harness dial");
    mdns_txt_item_t txt[] = { { .key = "mac", .value = mac } };
    err = mdns_service_add(NULL, "_harness-dial", "_tcp", WIFI_CABLE_PORT, txt, 1);
    if (err != ESP_OK) ESP_LOGW(TAG, "mdns_service_add: %s", esp_err_to_name(err));
    s_mdns_up = true;
    ESP_LOGI(TAG, "mdns %s.local _harness-dial._tcp:%d", host, WIFI_CABLE_PORT);
}

static void mdns_stop(void)
{
    mdns_free();
    s_mdns_up = false;
}

// ── sockets ─────────────────────────────────────────────────────────────────────────────────────────

static bool listen_up(void)
{
    if (s_listen >= 0) return true;
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return false;
    const int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(WIFI_CABLE_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 1) != 0) {
        ESP_LOGW(TAG, "tcp bind/listen failed (errno %d)", errno);
        close(fd);
        return false;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    s_listen = fd;
    ESP_LOGI(TAG, "tcp listen :%d (one client; bound computer only)", WIFI_CABLE_PORT);
    return true;
}

static void adopt(int c)
{
    const int yes = 1;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    // A peer that vanishes without a FIN would otherwise hold the only client slot until the session
    // layer's silence timer noticed; keepalive is the transport-level backstop.
    setsockopt(c, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
    int idle = 10, intvl = 5, cnt = 3;
    setsockopt(c, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(c, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(c, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
    struct timeval snd = { .tv_sec = 0, .tv_usec = 500000 };   // see wifi_cable_write
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));
    fcntl(c, F_SETFL, fcntl(c, F_GETFL, 0) & ~O_NONBLOCK);
    cable_decoder_reset(s_decoder);
    s_client_since_us = esp_timer_get_time();
    s_had_client = true;
    sock_lock();
    s_client = c;
    sock_unlock();
}

static void accept_one(void)
{
    struct sockaddr_in from;
    socklen_t fl = sizeof(from);
    const int c = accept(s_listen, (struct sockaddr *)&from, &fl);
    if (c < 0) return;
    // One client, and never while a USB session is up. The extra connection is closed on the spot.
    if (s_client >= 0 || cable_link_active() == CABLE_XPORT_USB) {
        ESP_LOGI(TAG, "tcp connect refused (%s)", s_client >= 0 ? "a client is already connected" : "usb session up");
        close(c);
        return;
    }
    adopt(c);
    ESP_LOGI(TAG, "tcp client connected, waiting for a welcome that carries the bind");
}

static void lan_task(void *arg)
{
    (void)arg;
    static uint8_t chunk[1024];   // BSS, not the 4 KiB task stack
    while (1) {
        if (s_had_client && s_client < 0) {
            s_had_client = false;
            if (s_on_closed) s_on_closed();
        }
        if (!s_want_up) {
            if (s_mdns_up) mdns_stop();
            sock_close(&s_client);
            sock_close(&s_listen);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (!s_mdns_up) mdns_start();
        if (!listen_up()) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }

        // USB wins: a cable session ends any LAN client. The daemon that holds both reopens the LAN
        // side once the cable session is over.
        if (s_client >= 0 && cable_link_active() == CABLE_XPORT_USB) {
            ESP_LOGI(TAG, "usb session up: dropping tcp client");
            sock_close(&s_client);
            continue;
        }
        // A connection that never presents an accepted welcome is not a session, it is a squatter on
        // the only client slot.
        if (s_client >= 0 && cable_link_active() != CABLE_XPORT_TCP &&
            esp_timer_get_time() - s_client_since_us > (int64_t)WIFI_CABLE_AUTH_MS * 1000) {
            ESP_LOGW(TAG, "tcp welcome timeout: dropping client");
            sock_close(&s_client);
            continue;
        }

        const int cfd = s_client;
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(s_listen, &rfds);
        int nf = s_listen;
        if (cfd >= 0) { FD_SET(cfd, &rfds); if (cfd > nf) nf = cfd; }
        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
        if (select(nf + 1, &rfds, NULL, NULL, &tv) <= 0) continue;
        if (FD_ISSET(s_listen, &rfds)) accept_one();
        if (cfd < 0 || !FD_ISSET(cfd, &rfds)) continue;

        const int n = recv(cfd, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            if (s_client == cfd) ESP_LOGI(TAG, "tcp client gone");
            sock_close(&s_client);
            continue;
        }
        if (s_client == cfd) cable_decoder_feed(s_decoder, chunk, (size_t)n, s_on_frame, cable_xport_ctx(CABLE_XPORT_TCP));
    }
}

void wifi_cable_init(cable_frame_cb on_frame, wifi_cable_closed_cb on_closed)
{
    s_on_frame = on_frame;
    s_on_closed = on_closed;
}

void wifi_cable_on_sta_up(void)
{
    if (!s_on_frame) return;
    if (!s_task) {
        s_sock_mu = xSemaphoreCreateMutex();
        s_decoder = heap_caps_calloc(1, sizeof(*s_decoder), MALLOC_CAP_SPIRAM);
        if (!s_decoder) s_decoder = calloc(1, sizeof(*s_decoder));
        if (!s_sock_mu || !s_decoder) { ESP_LOGE(TAG, "no memory for the lan transport"); return; }
        cable_decoder_init(s_decoder);
        s_want_up = true;
        if (xTaskCreate(lan_task, "wifi_cable", 4096, NULL, 4, &s_task) != pdPASS) {
            s_want_up = false;
            s_task = NULL;
            ESP_LOGE(TAG, "lan task create failed");
        }
        return;
    }
    s_want_up = true;
}

void wifi_cable_on_sta_down(void)
{
    s_want_up = false;
    wifi_cable_drop();   // the task closes the listener and mDNS on its next pass
}
