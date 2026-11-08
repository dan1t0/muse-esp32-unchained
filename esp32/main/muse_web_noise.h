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

// Construction for the web session's WebSocket upgrade — the web-auth
// recipe a logged-in browser uses, split out pure like noise_upgrade.h so
// host tests can run it without the transport.
//
// The pairing path's upgrade (noise_upgrade.h) authenticates with an
// Authorization bearer header and only vm_id in the query. The web client's
// dial — what a logged-in browser tab makes — instead puts every credential
// percent-encoded in the
// query string, on the shared gateway host, and that is the shape the edge
// has answered 101 to live:
//
//   GET /v1/noise?vm_id=..&auth_token=..&notary_token=..&app_id=..&request_id=..
//
// Above the upgrade sits the same Noise-XX handshake and HTTP framing the
// device path already runs (noise_control.cpp owns it; nothing here changes
// it). Server-fixed wire constants keep their names — the naming rule
// exempts existing ones — so the host and the app id below stay as the
// server spells them.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "noise_upgrade.h"  // the query escaper, and its cap arithmetic

// The shared load balancer every web dial goes through. The per-VM hosts
// are not resolvable publicly; the web bundle dials exactly this host.
#define MUSE_WEB_GATEWAY_HOST "hatch.metaaivm.com"
// The app id the web client presents in the query.
#define MUSE_WEB_APP_ID "hatch-web"

// Worst-case output size for the whole request, including the NUL: the fixed
// head (request line with the parameter names, the headers) plus every
// credential escaped, plus the request id.
static inline size_t muse_web_upgrade_request_cap(const char *auth_token,
                                                   const char *notary_token) {
    size_t tok_len = auth_token ? strlen(auth_token) : 0;
    size_t not_len = notary_token ? strlen(notary_token) : 0;
    return 512
           + NOISE_UPGRADE_ESCAPED_CAP(128)  /* vm_id */
           + NOISE_UPGRADE_ESCAPED_CAP(tok_len)
           + NOISE_UPGRADE_ESCAPED_CAP(not_len)
           + NOISE_UPGRADE_ESCAPED_CAP(40)   /* request_id */
           + strlen(MUSE_WEB_APP_ID) + 32;
}

// A bounded string builder cursor: bad latches once anything does not fit,
// and later appends become no-ops, so the caller only checks the flag once.
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    bool bad;
} muse_web_str_t;

static inline bool muse_web_append(muse_web_str_t *s, const char *text) {
    if (s->bad) return false;
    size_t n = strlen(text);
    if (s->len + n + 1 > s->cap) {
        s->bad = true;
        return false;
    }
    memcpy(s->buf + s->len, text, n);
    s->len += n;
    s->buf[s->len] = '\0';
    return true;
}

// Escapes value into esc (one scratch buffer per credential, reused in
// turn; esc_cap at least NOISE_UPGRADE_ESCAPED_CAP of the longest input)
// and appends it. The escape set is noise_upgrade.h's — exactly what
// JavaScript's encodeURIComponent() leaves unescaped, which is what the
// web bundle's URL builder produces.
static inline bool muse_web_append_escaped(muse_web_str_t *s, const char *value,
                                           char *esc, size_t esc_cap) {
    if (s->bad) return false;
    if (noise_upgrade_query_escape(value, esc, esc_cap) < 0) {
        s->bad = true;
        return false;
    }
    return muse_web_append(s, esc);
}

// Builds the web session's upgrade request into out (cap from
// muse_web_upgrade_request_cap()).
//
// Every input must be a non-empty string without CR/LF — vm_id and
// request_id feed the request target the edge routes on, the tokens the
// query, and a server-supplied value must not be able to inject headers.
//
// Returns the request length, or -1 if anything is missing, carries CR/LF,
// or did not fit; out then holds a partial prefix the caller must not
// send. The result embeds the credentials — never log it.
static inline int muse_web_upgrade_build_request(
    const char *path, const char *vm_id, const char *host,
    const char *auth_token, const char *notary_token,
    const char *request_id, char *esc, size_t esc_cap,
    char *out, size_t out_cap) {
    if (!path || !vm_id || !host || !auth_token || !notary_token
        || !request_id || !esc || !out) {
        return -1;
    }
    if (!*path || !*vm_id || !*host || !*auth_token || !*notary_token
        || !*request_id) {
        return -1;
    }
    if (strpbrk(path, "\r\n") || strpbrk(vm_id, "\r\n") || strpbrk(host, "\r\n")
        || strpbrk(auth_token, "\r\n") || strpbrk(notary_token, "\r\n")
        || strpbrk(request_id, "\r\n")) {
        return -1;
    }

    // The same fixed key the device path's upgrade sends (the sample
    // nonce); the server echoes its accept, it does not judge randomness.
    muse_web_str_t s = { .buf = out, .len = 0, .cap = out_cap, .bad = false };
    if (!muse_web_append(&s, "GET ")
        || !muse_web_append(&s, path)
        || !muse_web_append(&s, "?vm_id=")
        || !muse_web_append_escaped(&s, vm_id, esc, esc_cap)
        || !muse_web_append(&s, "&auth_token=")
        || !muse_web_append_escaped(&s, auth_token, esc, esc_cap)
        || !muse_web_append(&s, "&notary_token=")
        || !muse_web_append_escaped(&s, notary_token, esc, esc_cap)
        || !muse_web_append(&s, "&app_id=" MUSE_WEB_APP_ID "&request_id=")
        || !muse_web_append_escaped(&s, request_id, esc, esc_cap)
        || !muse_web_append(&s, " HTTP/1.1\r\n"
                                "Host: ")
        || !muse_web_append(&s, host)
        || !muse_web_append(&s, "\r\n"
                                "Upgrade: websocket\r\n"
                                "Connection: Upgrade\r\n"
                                "Sec-WebSocket-Version: 13\r\n"
                                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                "\r\n")) {
        return -1;
    }
    return (int)s.len;
}
