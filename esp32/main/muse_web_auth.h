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

// The web session's auth leg for the gadget (CONFIG_HOMEHUB_WEB_SESSION):
// the web-auth recipe a logged-in browser uses, running on the device itself.
//
// One pasted cookie value, the browser session's hatch_sess, stored in NVS
// as "web_cookie" (">web_cookie=…" on the console; never in the build,
// never in a log — lengths and 8-character hints only), mints the gateway
// tokens exactly the way a logged-in web client does:
//
//   cookie ─► GET  muse.ai/api/session       → vm_id, endpoint_url
//           ─► POST muse.ai/api/hatch/token   → gateway auth + notary tokens
//           ─► wss://hatch.metaaivm.com/v1/noise?…  (muse_web_noise.h)
//
// Above the upgrade the gadget runs the ordinary Noise session — register,
// commands, tunnel — with no SDK token, no device token and no BLE pairing.
// hatch_sess alone is enough: the site hands the other cookies back on the
// way, so only that one value is kept, and the tokens are re-derived every
// boot and cached in RAM for ten minutes.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define MUSE_WEB_VM_ID_MAX 128
#define MUSE_WEB_ENDPOINT_MAX 256 /* wss://<uuid>.metaaivm.com/ */
#define MUSE_WEB_TOKEN_MAX 1200   /* the gateway auth token (907 chars live) */
#define MUSE_WEB_NOTARY_MAX 720   /* the notary token (575 chars live) */
#define MUSE_WEB_COOKIE_MAX 900   /* the hatch_sess value */

typedef struct {
    char vm_id[MUSE_WEB_VM_ID_MAX];
    char endpoint[MUSE_WEB_ENDPOINT_MAX];
    char token[MUSE_WEB_TOKEN_MAX];   /* secret: the upgrade's auth_token */
    char notary[MUSE_WEB_NOTARY_MAX]; /* secret: the upgrade's notary */
} muse_web_auth_t;

typedef enum {
    MUSE_WEB_AUTH_OK = 0,
    MUSE_WEB_AUTH_NO_COOKIE, /* no web_cookie in NVS (send >web_cookie=…) */
    MUSE_WEB_AUTH_AUTH,       /* muse.ai refused the cookie: expired */
    MUSE_WEB_AUTH_FAILED,     /* transport or shape trouble */
} muse_web_auth_result_t;

const char *muse_web_auth_result_name(muse_web_auth_result_t result);

/* The cookie value's shape: a plausible hatch_sess (length and character
 * set only — only muse.ai can say whether it is real). Bounds the console
 * line and the Cookie header. */
bool muse_web_cookie_valid(const char *value);

/* Stores/reads the cookie in NVS ("web_cookie", the homehub namespace). */
bool muse_web_set_cookie(const char *value);
bool muse_web_cookie_get(char *out, size_t cap);
bool muse_web_cookie_present(void);

/* Drops the cached tokens so the next load re-runs the whole chain. */
void muse_web_auth_invalidate(void);

/* Runs the chain — cookie → GET /api/session → POST /api/hatch/token —
 * with a ten-minute in-RAM token cache. err (optional) gets one
 * secret-free line. Only the app task calls this: muse_web_boot_connect()
 * runs under the operation gate, and the heartbeat's reconnect path runs
 * on the same task. */
muse_web_auth_result_t muse_web_auth_load(muse_web_auth_t *out, char *err,
                                          size_t err_cap);

/* The seam's entry: the chain plus the web dial (noise_ctrl_connect_web),
 * marking setup complete on the first success so BLE advertising stops
 * like after any setup. network_ssid is as in noise_ctrl_connect(). False
 * leaves one secret-free line in err. */
bool muse_web_boot_connect(const char *network_ssid, char *err, size_t err_cap);
