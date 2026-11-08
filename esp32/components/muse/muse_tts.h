/* Copyright (c) 2026 Dani Martinez - dan1t0
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "sdkconfig.h"

/* One HTTP speech worker. Only the session task starts/reads requests; cancel
 * may also run on the voice task. Every read is scoped to its request ID. */
typedef enum { MUSE_TTS_PENDING, MUSE_TTS_END, MUSE_TTS_FAILED } muse_tts_status_t;
#if CONFIG_MUSE_TTS_HTTP
bool muse_tts_init(void);
uint32_t muse_tts_start(const char *text);
void muse_tts_cancel(void);
size_t muse_tts_read(uint32_t id, uint8_t *out, size_t cap, muse_tts_status_t *status);
#else
static inline bool muse_tts_init(void) { return false; }
static inline uint32_t muse_tts_start(const char *text) { (void)text; return 0; }
static inline void muse_tts_cancel(void) {}
static inline size_t muse_tts_read(uint32_t id, uint8_t *out, size_t cap, muse_tts_status_t *status)
{ (void)id; (void)out; (void)cap; *status = MUSE_TTS_FAILED; return 0; }
#endif
