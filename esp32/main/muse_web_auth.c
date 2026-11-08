/*
 * Copyright (c) 2026 Dani Martinez - dan1t0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// The web session's auth leg (CONFIG_HOMEHUB_WEB_SESSION), the port of the
// simulator's verified chain (sim_muse_auth.c → muse_web_auth.h's map).
// The wire constants — the hatch_sess cookie name, muse.ai, /api/hatch/token,
// the shared gateway — are server-fixed names the naming rule exempts.
//
// Nothing here ever prints a secret: statuses, ids and token lengths only,
// and the cookie as an 8-character hint, exactly like muse-login.sh.

#include "muse_web_auth.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config_store.h"
#include "muse_settings.h"
#include "muse_web_noise.h"
#include "noise_control.h"

static const char *TAG = "link.web_auth";

/* Server-fixed wire constants. */
#define WEB_SESSION_URL "https://muse.ai/api/session"
#define WEB_TOKEN_URL   "https://muse.ai/api/hatch/token"
#define COOKIE_KEY      "web_cookie"

#define RESPONSE_MAX (16 * 1024)
#define TOKEN_TTL_US (10LL * 60 * 1000000) /* "minutes to hours": ask again in 10 */
#define MAX_ATTEMPTS 4
#define INITIAL_BACKOFF_MS 500  /* the first DNS query after Wi-Fi joins often
                                 * loses to the DHCP race; see vm_api.c */

// ---- cookie ---------------------------------------------------------------

bool muse_web_cookie_valid(const char *value) {
    if (!value) return false;
    size_t len = strlen(value);
    if (len < 16 || len > MUSE_WEB_COOKIE_MAX) return false;
    for (const char *c = value; *c; c++) {
        unsigned char ch = (unsigned char)*c;
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')
              || (ch >= '0' && ch <= '9')
              || strchr("_-.~%=+/", ch))) {
            return false;
        }
    }
    return true;
}

bool muse_web_set_cookie(const char *value) {
    return muse_web_cookie_valid(value) && config_set_str(COOKIE_KEY, value);
}

bool muse_web_cookie_get(char *out, size_t cap) {
    return config_get_str(COOKIE_KEY, out, cap) && out[0];
}

bool muse_web_cookie_present(void) {
    char value[MUSE_WEB_COOKIE_MAX + 1];
    if (!muse_web_cookie_get(value, sizeof(value))) return false;
    bool present = muse_web_cookie_valid(value);
    memset(value, 0, sizeof(value));
    return present;
}

// ---- the in-RAM token cache ------------------------------------------------

static muse_web_auth_t s_cache;
static bool s_cached;
static int64_t s_fetched_us;

void muse_web_auth_invalidate(void) {
    memset(&s_cache, 0, sizeof(s_cache));
    s_cached = false;
}

const char *muse_web_auth_result_name(muse_web_auth_result_t result) {
    switch (result) {
    case MUSE_WEB_AUTH_OK: return "ok";
    case MUSE_WEB_AUTH_NO_COOKIE: return "no web cookie";
    case MUSE_WEB_AUTH_AUTH: return "auth";
    case MUSE_WEB_AUTH_FAILED: return "failed";
    }
    return "?";
}

// ---- HTTP plumbing (the vm_api.c pattern) ----------------------------------

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} web_resp_t;

static esp_err_t web_event(esp_http_client_event_t *evt) {
    web_resp_t *r = (web_resp_t *)evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || !r || !evt->data_len) {
        return ESP_OK;
    }
    if (r->len + evt->data_len > RESPONSE_MAX) {
        return ESP_OK;  // drop the tail, like vm_api.c
    }
    if (r->len + evt->data_len > r->cap) {
        size_t new_cap = r->cap ? r->cap * 2 : 1024;
        while (new_cap < r->len + (size_t)evt->data_len) new_cap *= 2;
        if (new_cap > RESPONSE_MAX) new_cap = RESPONSE_MAX;
        char *nb = realloc(r->buf, new_cap);
        if (!nb) return ESP_FAIL;
        r->buf = nb;
        r->cap = new_cap;
    }
    memcpy(r->buf + r->len, evt->data, (size_t)evt->data_len);
    r->len += (size_t)evt->data_len;
    return ESP_OK;
}

// One call. cookie_header is "hatch_sess=<value>" (heap, wiped by the
// caller); fetch_shape is true for the POSTs the edge refuses without the
// browser's Sec-Fetch-* headers (muse-web-auth.md: the empty 403 was that).
static muse_web_auth_result_t web_http(const char *url,
                                        esp_http_client_method_t method,
                                        const char *cookie_header,
                                        bool fetch_shape, const char *body,
                                        web_resp_t *resp, int *status_out) {
    *status_out = 0;
    resp->buf = NULL;
    resp->len = 0;
    resp->cap = 0;

    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .event_handler = web_event,
        .user_data = resp,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return MUSE_WEB_AUTH_FAILED;
    }

    esp_http_client_set_header(client, "Cookie", cookie_header);
    if (fetch_shape) {
        // Exactly the sim's FETCH_SHAPE_HEADERS: the browser fetch shape
        // plus the usual Accept, without which the POST is 403'd empty.
        esp_http_client_set_header(client, "Accept", "*/*");
        esp_http_client_set_header(client, "Origin", "https://muse.ai");
        esp_http_client_set_header(client, "Referer", "https://muse.ai/");
        esp_http_client_set_header(client, "Sec-Fetch-Site", "same-origin");
        esp_http_client_set_header(client, "Sec-Fetch-Mode", "cors");
        esp_http_client_set_header(client, "Sec-Fetch-Dest", "empty");
    }
    if (body) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body, (int)strlen(body));
    }

    esp_err_t err = esp_http_client_perform(client);
    *status_out = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (*status_out == 401 || *status_out == 403) {
        ESP_LOGW(TAG, "%s answered HTTP %d (cookie expired?)",
                 url, *status_out);
        free(resp->buf);
        resp->buf = NULL;
        return MUSE_WEB_AUTH_AUTH;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s failed: %s", url, esp_err_to_name(err));
        free(resp->buf);
        resp->buf = NULL;
        *status_out = 0;
        return MUSE_WEB_AUTH_FAILED;
    }
    if (*status_out < 200 || *status_out >= 300) {
        ESP_LOGW(TAG, "%s answered HTTP %d", url, *status_out);
        free(resp->buf);
        resp->buf = NULL;
        return MUSE_WEB_AUTH_FAILED;
    }
    return MUSE_WEB_AUTH_OK;
}

// Transport-level retries (DNS race, TLS hiccup): auth refusals are final.
static muse_web_auth_result_t web_http_with_retries(
    const char *url, esp_http_client_method_t method,
    const char *cookie_header, bool fetch_shape, const char *body,
    web_resp_t *resp) {
    int backoff_ms = INITIAL_BACKOFF_MS;
    int status = 0;
    for (int attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
        muse_web_auth_result_t rc =
            web_http(url, method, cookie_header, fetch_shape, body, resp, &status);
        if (rc == MUSE_WEB_AUTH_OK) {
            ESP_LOGI(TAG, "%s: HTTP %d, %u bytes (attempt %d)",
                     url, status, (unsigned)resp->len, attempt);
            return rc;
        }
        if (rc == MUSE_WEB_AUTH_AUTH) return rc;
        if (attempt < MAX_ATTEMPTS) {
            ESP_LOGW(TAG, "%s: attempt %d failed (status=%d), retry in %d ms",
                     url, attempt, status, backoff_ms);
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            backoff_ms *= 2;
        } else {
            ESP_LOGE(TAG, "%s: all %d attempts failed (last status=%d)",
                     url, MAX_ATTEMPTS, status);
        }
    }
    return MUSE_WEB_AUTH_FAILED;
}

// ---- the two bodies' parsers (cJSON, faithful to the sim's fields) --------

static bool copy_bounded(const cJSON *root, const char *key, char *out,
                         size_t cap, const char *what) {
    const cJSON *item = cJSON_GetObjectItem(root, key);
    if (!cJSON_IsString(item) || !item->valuestring || !item->valuestring[0]) {
        ESP_LOGW(TAG, "no %s in the %s answer", key, what);
        return false;
    }
    if (strlen(item->valuestring) + 1 > cap) {
        ESP_LOGW(TAG, "%s in the %s answer is too long", key, what);
        return false;
    }
    snprintf(out, cap, "%s", item->valuestring);
    return true;
}

static bool parse_session_body(const char *body, size_t len,
                               muse_web_auth_t *out) {
    cJSON *root = cJSON_ParseWithLength(body, len);
    if (!cJSON_IsObject(root)) {
        ESP_LOGW(TAG, "the session answer is not an object");
        cJSON_Delete(root);
        return false;
    }
    bool ok = copy_bounded(root, "vm_id", out->vm_id, sizeof(out->vm_id),
                           "session")
              && copy_bounded(root, "endpoint_url", out->endpoint,
                              sizeof(out->endpoint), "session");
    cJSON_Delete(root);
    return ok;
}

static bool parse_token_body(const char *body, size_t len,
                             muse_web_auth_t *out) {
    cJSON *root = cJSON_ParseWithLength(body, len);
    if (!cJSON_IsObject(root)) {
        ESP_LOGW(TAG, "the token answer is not an object");
        cJSON_Delete(root);
        return false;
    }
    bool ok = copy_bounded(root, "token", out->token, sizeof(out->token),
                           "token")
              && copy_bounded(root, "notary_token", out->notary,
                              sizeof(out->notary), "token");
    cJSON_Delete(root);
    return ok;
}

// ---- the chain ---------------------------------------------------------------

muse_web_auth_result_t muse_web_auth_load(muse_web_auth_t *out, char *err,
                                          size_t err_cap) {
    if (err && err_cap) err[0] = '\0';
    memset(out, 0, sizeof(*out));

    if (s_cached
        && esp_timer_get_time() - s_fetched_us < TOKEN_TTL_US) {
        *out = s_cache;
        return MUSE_WEB_AUTH_OK;
    }

    char *cookie = malloc(MUSE_WEB_COOKIE_MAX + 1);
    if (!cookie) {
        snprintf(err, err_cap, "out of memory");
        return MUSE_WEB_AUTH_FAILED;
    }
    if (!muse_web_cookie_get(cookie, MUSE_WEB_COOKIE_MAX + 1)) {
        snprintf(err, err_cap, "no web cookie; send >web_cookie=… on the console");
        free(cookie);
        return MUSE_WEB_AUTH_NO_COOKIE;
    }
    if (!muse_web_cookie_valid(cookie)) {
        snprintf(err, err_cap, "the stored web cookie has a bad shape; re-send it");
        free(cookie);
        return MUSE_WEB_AUTH_NO_COOKIE;
    }
    ESP_LOGI(TAG, "web cookie %.8s… (%u chars); running the web chain",
             cookie, (unsigned)strlen(cookie));

    // The one Cookie header both calls send: hatch_sess alone opens
    // /api/session, which hands the other cookies back on the way.
    char *cookie_header = malloc(MUSE_WEB_COOKIE_MAX + 32);
    muse_web_auth_result_t rc = MUSE_WEB_AUTH_FAILED;
    if (!cookie_header) {
        snprintf(err, err_cap, "out of memory");
        free(cookie);
        return rc;
    }
    snprintf(cookie_header, MUSE_WEB_COOKIE_MAX + 32, "hatch_sess=%s", cookie);
    memset(cookie, 0, MUSE_WEB_COOKIE_MAX + 1);
    free(cookie);

    // 1. GET /api/session → vm_id, endpoint_url. GETs need no fetch shape.
    web_resp_t resp = {0};
    rc = web_http_with_retries(WEB_SESSION_URL, HTTP_METHOD_GET,
                               cookie_header, false, NULL, &resp);
    if (rc != MUSE_WEB_AUTH_OK) {
        snprintf(err, err_cap, "%s (%s)", WEB_SESSION_URL,
                 muse_web_auth_result_name(rc));
        goto done;
    }
    if (!parse_session_body(resp.buf, resp.len, out)) {
        snprintf(err, err_cap, "the session answer carried no vm_id/endpoint_url");
        rc = MUSE_WEB_AUTH_FAILED;
        goto done;
    }
    free(resp.buf);
    resp.buf = NULL;
    resp.len = 0;
    resp.cap = 0;

    // 2. POST /api/hatch/token → the gateway's auth + notary tokens. The
    // body is the session's own answer: vmAddress = endpoint_url,
    // vmName = vm_id (the sim's exact shape).
    char *request = malloc(512);
    if (!request) {
        snprintf(err, err_cap, "out of memory");
        rc = MUSE_WEB_AUTH_FAILED;
        goto done;
    }
    snprintf(request, 512, "{\"vmAddress\":\"%.200s\",\"vmName\":\"%.100s\"}",
             out->endpoint, out->vm_id);
    rc = web_http_with_retries(WEB_TOKEN_URL, HTTP_METHOD_POST,
                               cookie_header, true, request, &resp);
    memset(request, 0, 512);
    free(request);
    if (rc != MUSE_WEB_AUTH_OK) {
        snprintf(err, err_cap, "%s (%s)", WEB_TOKEN_URL,
                 muse_web_auth_result_name(rc));
        goto done;
    }
    if (!parse_token_body(resp.buf, resp.len, out)) {
        snprintf(err, err_cap, "the token answer carried no token/notary_token");
        rc = MUSE_WEB_AUTH_FAILED;
        goto done;
    }

    s_cache = *out;
    s_fetched_us = esp_timer_get_time();
    s_cached = true;
    ESP_LOGI(TAG, "web chain ok: VM %.36s endpoint %.120s, auth token %u chars,"
             " notary %u chars",
             out->vm_id, out->endpoint, (unsigned)strlen(out->token),
             (unsigned)strlen(out->notary));

done:
    free(resp.buf);
    memset(cookie_header, 0, MUSE_WEB_COOKIE_MAX + 32);
    free(cookie_header);
    if (rc != MUSE_WEB_AUTH_OK) {
        muse_web_auth_invalidate();
    }
    return rc;
}

// ---- the seam's entry --------------------------------------------------------

bool muse_web_boot_connect(const char *network_ssid, char *err, size_t err_cap) {
    if (err && err_cap) err[0] = '\0';

    muse_web_auth_t auth;
    muse_web_auth_result_t rc = muse_web_auth_load(&auth, err, err_cap);
    if (rc != MUSE_WEB_AUTH_OK) {
        return false;
    }

    // The web dial always goes through the shared gateway host; say so
    // explicitly so a pairing-era noise_host can never leak in.
    noise_ctrl_set_host(MUSE_WEB_GATEWAY_HOST);
    ESP_LOGI(TAG, "dialing the shared gateway as VM %.36s", auth.vm_id);

    // Seed the muse UI's voice chat (its own dialer) with the web session's
    // credentials: it hits the same shared gateway and accepts the web auth
    // token as the upgrade's Authorization bearer (probe: HTTP 101,
    // 2026-10-25), so resolve_vm()'s direct fallback runs on the stored
    // (vm_id, token) — no SDK token, no device chain. Re-seeded on every
    // chain run, so the voice session's stored token never outlives the one
    // the gateway minted.
    muse_settings_set_hatch_host(MUSE_WEB_GATEWAY_HOST);
    muse_settings_set_hatch_vm(auth.vm_id);
    esp_err_t tok_err = muse_settings_set_hatch_token(auth.token, false);
    if (tok_err != ESP_OK) {
        ESP_LOGW(TAG, "voice chat seed failed: %s", esp_err_to_name(tok_err));
    }

    bool ok = noise_ctrl_connect_web(auth.vm_id, auth.token, auth.notary,
                                     network_ssid);
    // noise_ctrl keeps its own copies of the credentials; wipe these.
    memset(&auth, 0, sizeof(auth));
    if (!ok) {
        snprintf(err, err_cap, "the noise session did not start");
        return false;
    }
    // The device is set up now: BLE advertising stops, as after any setup.
    config_mark_setup_complete();
    return true;
}
