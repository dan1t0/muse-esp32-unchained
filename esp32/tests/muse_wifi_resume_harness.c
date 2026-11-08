/* Copyright (c) 2026 Dani Martinez - dan1t0
 * SPDX-License-Identifier: Apache-2.0 */
// Host fixture for production app_wifi_join_saved/app_wifi_nap/keeper_task.
// All credentials are synthetic. RTOS, radio, storage and UI are fakes.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "app.h"

#define BIT0 1u
#define BIT1 2u
#define BIT2 4u
#define BIT3 8u
#define BIT4 16u
#define pdFALSE 0
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) (ms)
#define WIFI_KNOWN_SSID_MAX 32
#define LED_STATE_WIFI_CONNECTING 1
#define LED_STATE_WS_DISCONNECTED 2
#define ESP_LOGI(...) ((void)0)
#define STACK_MONITOR_INIT 0
typedef int stack_monitor_t;
typedef enum { JOIN_ANY, JOIN_ANY_CACHED, JOIN_FIRST } join_mode_t;
@CONSTANTS@

static bool s_muse_napping, s_muse_resume_vm;
static bool s_nap, s_editing, s_chosen, s_away, s_linked, s_joining;
static uint32_t s_creds_gen;
static char s_ssid[33];
static int s_ready, s_muse_joined;
static bool connected, vm_running, wifi_on = true, sleeping;
static bool paired, gate_available = true, gate_held, ap_available = true;
static bool stale_scan;
static char saved_ssid[33] = "saved-network";
static char last_ssid[33], last_password[65], status_text[40], failure[40];
static int connects, saved_joins, disconnects, vm_disconnects, heartbeat_wakes;
static int channel_saves, gate_attempts, passes;
static join_mode_t last_mode;
static int64_t now_us = 100000000;
static int64_t attempt_times[16];
static uint32_t notifications;
static const char *scenario;
static jmp_buf stopped;

static bool operation_gate_take(int wait, const char *why) {
    (void)why;
    assert(wait == 0 && !gate_held);
    gate_attempts++;
    if (!gate_available) return false;
    gate_held = true;
    return true;
}
static void operation_gate_give(void) { assert(gate_held); gate_held = false; }
static bool config_is_provisioned(void) { return paired; }
static bool config_setup_complete(void) { return paired; }
static void ui_set_status(const char *s) { snprintf(status_text, sizeof(status_text), "%s", s); }
static void ui_set_wifi(const char *s) { assert(!strcmp(s, last_ssid)); }
static void led_status_set_state(int state) { (void)state; }
static void persist_connected_wifi_channel(void) { channel_saves++; }
static void xSemaphoreGive(int semaphore) { (void)semaphore; heartbeat_wakes++; }
static bool wifi_mgr_is_connected(void) { return connected; }
static bool wifi_mgr_connect(const char *ssid, const char *password, int timeout_ms) {
    assert(gate_held && timeout_ms == CONNECT_TIMEOUT_MS);
    assert(connects < (int)(sizeof(attempt_times) / sizeof(attempt_times[0])));
    attempt_times[connects++] = now_us;
    snprintf(last_ssid, sizeof(last_ssid), "%s", ssid);
    snprintf(last_password, sizeof(last_password), "%s", password);
    connected = ap_available;
    return connected;
}
static void wifi_mgr_disconnect(void) { connected = false; disconnects++; }
static bool noise_ctrl_is_running(void) { return vm_running; }
static void disconnect_vm_transports(void) { vm_running = false; vm_disconnects++; }
static app_wifi_join_t join_saved_networks(int timeout, join_mode_t mode, bool led, char *joined) {
    assert(gate_held && led == paired);
    saved_joins++;
    last_mode = mode;
    if (!saved_ssid[0]) return APP_WIFI_NOT_NEARBY;
    if (!wifi_mgr_connect(saved_ssid, "saved-password", timeout)) return APP_WIFI_FAILED;
    snprintf(joined, WIFI_KNOWN_SSID_MAX + 1, "%s", saved_ssid);
    return APP_WIFI_JOINED;
}
static bool load_creds(void) { snprintf(s_ssid, sizeof(s_ssid), "%s", saved_ssid); return true; }
static void reload_creds(uint32_t *gen) { load_creds(); *gen = s_creds_gen; }
static void save_saved(uint32_t pending, int64_t *next) { (void)pending; (void)next; }
static bool muse_settings_ble_on(void) { return false; }
static void muse_settings_set_ble_on(bool on) { (void)on; }
static void app_ble_companion_set_fake(bool on) { (void)on; }
#define app_ble_companion_set app_ble_companion_set_fake
static bool muse_settings_wifi_on(void) { return wifi_on; }
static bool muse_state_asleep(void) { return sleeping; }
static bool none_nearby_lately(void) { return stale_scan; }
static int64_t esp_timer_get_time(void) { return now_us; }
static void set_fail(const char *s) { snprintf(failure, sizeof(failure), "%s", s); }
static void keeper_kick(uint32_t bits) { notifications |= bits; }
static void stack_monitor_poll(stack_monitor_t *s) { (void)s; }
static void xEventGroupWaitBits(int group, uint32_t bits, int clear, int all, uint32_t wait) {
    (void)group; (void)bits; (void)clear; (void)all; (void)wait;
}
static void xTaskNotifyWait(uint32_t clear, uint32_t exit_clear, uint32_t *out, uint32_t wait);

@PRODUCTION@

static void finish(void) { assert(!gate_held); longjmp(stopped, 1); }
static void expect_credentials(void) {
    assert(!strcmp(last_ssid, WIFI_OVERRIDE ? "build-network" : "saved-network"));
    assert(!strcmp(last_password, WIFI_OVERRIDE ? "build-password" : "saved-password"));
}
static void expect_joined(int count) {
    assert(connected && connects == count && !s_muse_napping);
    assert(heartbeat_wakes == count);
    assert(!strcmp(status_text, "wifi_connected") && !failure[0]);
    expect_credentials();
}
static void xTaskNotifyWait(uint32_t clear, uint32_t exit_clear, uint32_t *out, uint32_t wait) {
    (void)clear; (void)exit_clear;
    assert(!gate_held);
    assert(passes < 20);  // No hanging tests if the production loop regresses.
    int step = passes++;
    if (!strcmp(scenario, "cycles")) {
        int cycle = step / 3, phase = step % 3;
        if (phase < 2) {
            assert(!connected && s_muse_napping && s_muse_resume_vm);
            assert(connects == cycle && vm_disconnects == cycle + 1);
            assert(wait == portMAX_DELAY);
            if (phase == 1) { sleeping = false; op_wifi_nap(false); }
        } else {
            expect_joined(cycle + 1);
            if (cycle == 2) finish();
            sleeping = true;
            vm_running = true;
            op_wifi_nap(true);
        }
    } else if (!strcmp(scenario, "router")) {
        if (step == 0) {
            assert(!connected && connects == 0);
            sleeping = false;
            op_wifi_nap(false);
        } else if (step == 1) {
            assert(connects == 1 && !connected && heartbeat_wakes == 0);
            assert(!strcmp(status_text, "wifi_failed") && failure[0]);
            now_us = attempt_times[0] + RETRY_MIN_US - 1;
        } else if (step == 2) {
            assert(connects == 1);  // The retry deadline has not arrived.
            now_us++;
            ap_available = true;
        } else {
            assert(connects == 2 && heartbeat_wakes == 1 && connected && !failure[0]);
            expect_credentials();
            finish();
        }
    } else if (!strcmp(scenario, "backoff")) {
        assert(connects == step + 1 && !connected && !heartbeat_wakes);
        if (step) {
            int64_t expected = RETRY_MIN_US << (step - 1);
            if (expected > RETRY_AWAKE_MAX_US) expected = RETRY_AWAKE_MAX_US;
            assert(attempt_times[step] - attempt_times[step - 1] == expected);
        }
        if (step == 5) finish();
        int64_t delay = RETRY_MIN_US << step;
        if (delay > RETRY_AWAKE_MAX_US) delay = RETRY_AWAKE_MAX_US;
        now_us += delay;
    } else if (!strcmp(scenario, "off")) {
        if (step < 2) {
            assert(!connects && !connected);
            if (step == 0) { sleeping = false; op_wifi_nap(false); }
            else { wifi_on = true; keeper_kick(KEEP_WIFI); }
        } else { expect_joined(1); finish(); }
    } else if (!strcmp(scenario, "busy")) {
        if (step == 0) {
            assert(!connects && !vm_disconnects);  // Nap cannot steal the gate.
            gate_available = true;
        } else if (step == 1) {
            assert(!connected && vm_disconnects == 1);
            sleeping = false;
            gate_available = false;
            op_wifi_nap(false);
        } else if (step == 2) {
            assert(!connects && s_muse_napping);
            gate_available = true;
            now_us += 1000000;
        } else { expect_joined(1); finish(); }
    } else if (!strcmp(scenario, "editing")) {
        if (step == 0) {
            assert(!connects && !gate_attempts);
            s_editing = false;
            keeper_kick(KEEP_WIFI);
        } else { expect_joined(1); finish(); }
    } else if (!strcmp(scenario, "no_saved")) {
        if (WIFI_OVERRIDE) {
            expect_joined(1);
            assert(!saved_joins && !channel_saves);
        } else {
            assert(!connects && !saved_joins);  // No network configured.
        }
        finish();
    } else { assert(!"unknown scenario"); }
    *out = notifications;
    notifications = 0;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    scenario = argv[1];
    // A cookie-only override has no device token; normal mode is paired.
    paired = !WIFI_OVERRIDE;
    if (!strcmp(scenario, "credentials")) {
        for (int first = 0; first < 2; first++) {
            assert(app_wifi_join_saved(CONNECT_TIMEOUT_MS, first) == APP_WIFI_JOINED);
            expect_credentials();
            assert(heartbeat_wakes == first + 1);
            assert(channel_saves == (WIFI_OVERRIDE ? 0 : first + 1));
            if (!WIFI_OVERRIDE) assert(last_mode == (first ? JOIN_FIRST : JOIN_ANY));
        }
        assert(saved_joins == (WIFI_OVERRIDE ? 0 : 2));
        assert(!strcmp(saved_ssid, "saved-network"));  // No storage rewrite.
        return 0;
    }
    if (!strcmp(scenario, "gate")) {
        s_muse_napping = true;
        gate_available = false;
        assert(app_wifi_join_saved(CONNECT_TIMEOUT_MS, false) == APP_WIFI_BUSY);
        assert(!connects && s_muse_napping && !heartbeat_wakes);
        gate_available = true;
        ap_available = false;
        assert(app_wifi_join_saved(CONNECT_TIMEOUT_MS, false) == APP_WIFI_FAILED);
        assert(!connected && !s_muse_napping && !gate_held && !heartbeat_wakes);
        assert(!strcmp(status_text, "wifi_failed") && !channel_saves);
        expect_credentials();
        return 0;
    }
    if (!strcmp(scenario, "cycles") || !strcmp(scenario, "router") || !strcmp(scenario, "busy")) {
        connected = vm_running = sleeping = s_nap = true;
    }
    if (!strcmp(scenario, "router") || !strcmp(scenario, "backoff")) ap_available = false;
    if (!strcmp(scenario, "off")) { wifi_on = false; sleeping = s_nap = s_muse_napping = true; }
    if (!strcmp(scenario, "busy")) gate_available = false;
    if (!strcmp(scenario, "editing")) s_editing = true;
    if (!strcmp(scenario, "no_saved")) { saved_ssid[0] = '\0'; s_away = stale_scan = true; }
    if (setjmp(stopped) == 0) keeper_task(NULL);
    return 0;
}
