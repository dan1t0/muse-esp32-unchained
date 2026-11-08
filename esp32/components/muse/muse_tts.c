/* Copyright (c) 2026 Dani Martinez - dan1t0
 * SPDX-License-Identifier: Apache-2.0 */
#include "muse_tts.h"
#if CONFIG_MUSE_TTS_HTTP
#include <string.h>
#include <strings.h>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "muse_mem.h"

#define BUFFER_BYTES (32 * 1024)
#define READ_BYTES 2048
#define MAX_AUDIO_BYTES (4 * 1024 * 1024)
#define REQUEST_US (90 * 1000000LL)
static const char *TAG = "muse_tts";
typedef struct { uint32_t id; char *text; } job_t;
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_jobs;
static uint8_t *s_buffer;
static size_t s_head, s_used;
static uint32_t s_id;
static int s_fd = -1;
static muse_tts_status_t s_status;

static bool active(uint32_t id)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool yes = id == s_id;
    xSemaphoreGive(s_lock);
    return yes;
}

/* Bounded backpressure. The worker owns the HTTP client; the session owns the
 * MP3 decoder. They never touch each other's mutable state. */
static bool push(uint32_t id, const uint8_t *data, size_t len, int64_t deadline)
{
    while (len) {
        if (esp_timer_get_time() >= deadline) return false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (id != s_id) { xSemaphoreGive(s_lock); return false; }
        size_t tail = (s_head + s_used) % BUFFER_BYTES;
        size_t n = BUFFER_BYTES - s_used;
        if (n > BUFFER_BYTES - tail) n = BUFFER_BYTES - tail;
        if (n > len) n = len;
        memcpy(s_buffer + tail, data, n);
        s_used += n;
        xSemaphoreGive(s_lock);
        data += n; len -= n;
        if (!n) vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

static esp_err_t http_event(esp_http_client_event_t *e)
{
    bool *is_mp3 = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_HEADER && e->header_key && e->header_value &&
        !strcasecmp(e->header_key, "Content-Type")) {
        const char *v = e->header_value;
        *is_mp3 = (!strncasecmp(v, "audio/mpeg", 10) && (!v[10] || v[10] == ';')) ||
                  (!strncasecmp(v, "audio/mp3", 9) && (!v[9] || v[9] == ';'));
    }
    return ESP_OK;
}

static bool fetch(job_t job)
{
    bool ok = false, is_mp3 = false;
    size_t total = 0;
    int64_t deadline = esp_timer_get_time() + REQUEST_US;
    cJSON *body = cJSON_CreateObject();
    if (!body) return false;
    bool built = cJSON_AddStringToObject(body, "model", CONFIG_MUSE_TTS_MODEL) &&
                 cJSON_AddStringToObject(body, "input", job.text) &&
                 cJSON_AddStringToObject(body, "voice", CONFIG_MUSE_TTS_VOICE) &&
                 cJSON_AddStringToObject(body, "response_format", "mp3");
    if (built && CONFIG_MUSE_TTS_LANG_CODE[0])
        built = cJSON_AddStringToObject(body, "lang_code", CONFIG_MUSE_TTS_LANG_CODE) != NULL;
#if CONFIG_MUSE_TTS_STREAM
    if (built) built = cJSON_AddBoolToObject(body, "stream", true) != NULL;
#endif
    char *json = built ? cJSON_PrintUnformatted(body) : NULL;
    cJSON_Delete(body);
    if (!json) return false;
    esp_http_client_config_t cfg = {
        .url = CONFIG_MUSE_TTS_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 3000,
        .buffer_size = READ_BYTES,
        .buffer_size_tx = 1024,
        .disable_auto_redirect = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event,
        .user_data = &is_mp3,
    };
    esp_http_client_handle_t http = esp_http_client_init(&cfg);
    if (!http) { cJSON_free(json); return false; }
    if (esp_http_client_set_header(http, "Content-Type", "application/json") != ESP_OK ||
        esp_http_client_set_header(http, "Accept", "audio/mpeg") != ESP_OK ||
        !active(job.id) || esp_http_client_open(http, (int)strlen(json)) != ESP_OK) goto done;

    /* shutdown() interrupts a blocked fetch/read without concurrently changing
     * esp_http_client state. Clear the descriptor under this lock before the
     * owning worker closes it, so cancel cannot hit a recycled socket. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (job.id == s_id) s_fd = esp_http_client_get_socket(http);
    bool current = job.id == s_id;
    xSemaphoreGive(s_lock);
    if (!current) goto done;
    for (size_t off = 0, len = strlen(json); off < len;) {
        if (!active(job.id) || esp_timer_get_time() >= deadline) goto done;
        int n = esp_http_client_write(http, json + off, (int)(len - off));
        if (n <= 0) goto done;
        off += (size_t)n;
    }
    esp_http_client_set_timeout_ms(http, 60000);
    int64_t length = esp_http_client_fetch_headers(http);
    int status = esp_http_client_get_status_code(http);
    if (!active(job.id)) goto done;
    if (length < 0 || status != 200 || !is_mp3 || length > MAX_AUDIO_BYTES) {
        ESP_LOGW(TAG, "speech rejected: HTTP %d, MP3=%d, bytes=%lld", status, is_mp3, (long long)length);
        goto done;
    }
    esp_http_client_set_timeout_ms(http, 1000);
    uint8_t chunk[READ_BYTES];
    while (active(job.id) && esp_timer_get_time() < deadline) {
        int n = esp_http_client_read(http, (char *)chunk, sizeof(chunk));
        if (n == -ESP_ERR_HTTP_EAGAIN) continue;
        if (n < 0) break;
        if (!n) {
            ok = total > 0 && esp_http_client_is_complete_data_received(http);
            break;
        }
        total += (size_t)n;
        if (total > MAX_AUDIO_BYTES || !push(job.id, chunk, (size_t)n, deadline)) break;
    }
done:
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* There is only one worker/socket, including while a newer job is queued. */
    s_fd = -1;
    xSemaphoreGive(s_lock);
    esp_http_client_cleanup(http);
    cJSON_free(json);
    return ok;
}

static void worker(void *unused)
{
    (void)unused;
    job_t job;
    for (;;) {
        if (xQueueReceive(s_jobs, &job, portMAX_DELAY) != pdTRUE) continue;
        bool ok = active(job.id) && fetch(job);
        heap_caps_free(job.text);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (job.id == s_id) {
            s_status = ok ? MUSE_TTS_END : MUSE_TTS_FAILED;
            if (!ok) ESP_LOGW(TAG, "speech unavailable or incomplete");
        }
        xSemaphoreGive(s_lock);
    }
}

bool muse_tts_init(void)
{
    if (s_jobs) return true;
    if (!CONFIG_MUSE_TTS_URL[0]) return false;
    s_buffer = heap_caps_malloc(BUFFER_BYTES, MUSE_BIG_CAPS);
    s_lock = xSemaphoreCreateMutex();
    s_jobs = xQueueCreateWithCaps(1, sizeof(job_t), MUSE_BIG_CAPS);
    if (!s_buffer || !s_lock || !s_jobs ||
        xTaskCreatePinnedToCoreWithCaps(worker, "muse_tts", 8192, NULL, 4, NULL, 0, MUSE_BIG_CAPS) != pdPASS) {
        if (s_jobs) vQueueDeleteWithCaps(s_jobs);
        if (s_lock) vSemaphoreDelete(s_lock);
        heap_caps_free(s_buffer);
        s_jobs = NULL; s_lock = NULL; s_buffer = NULL;
        ESP_LOGE(TAG, "speech worker could not start");
        return false;
    }
    ESP_LOGI(TAG, "HTTP speech ready (voice %s, model %s)", CONFIG_MUSE_TTS_VOICE, CONFIG_MUSE_TTS_MODEL);
    return true;
}

static void cancel_locked(void)
{
    if (++s_id == 0) ++s_id;
    s_head = s_used = 0;
    s_status = MUSE_TTS_FAILED;
    if (s_fd >= 0) shutdown(s_fd, SHUT_RDWR);
    job_t pending;
    if (xQueueReceive(s_jobs, &pending, 0) == pdTRUE) heap_caps_free(pending.text);
}

void muse_tts_cancel(void)
{
    if (!s_jobs) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cancel_locked();
    xSemaphoreGive(s_lock);
}

uint32_t muse_tts_start(const char *text)
{
    if (!s_jobs || !text || !text[0]) return 0;
    size_t len = strlen(text) + 1;
    char *copy = heap_caps_malloc(len, MUSE_BIG_CAPS);
    if (!copy) return 0;
    memcpy(copy, text, len);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cancel_locked();
    job_t job = { .id = s_id, .text = copy };
    s_status = MUSE_TTS_PENDING;
    /* Queue insertion and generation publication are atomic with cancel. */
    bool sent = xQueueSend(s_jobs, &job, 0) == pdTRUE;
    xSemaphoreGive(s_lock);
    if (!sent) { heap_caps_free(copy); return 0; }
    return job.id;
}

size_t muse_tts_read(uint32_t id, uint8_t *out, size_t cap, muse_tts_status_t *status)
{
    *status = MUSE_TTS_FAILED;
    if (!s_jobs || !id) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t n = 0;
    if (id == s_id) {
        n = s_used < cap ? s_used : cap;
        size_t first = n < BUFFER_BYTES - s_head ? n : BUFFER_BYTES - s_head;
        memcpy(out, s_buffer + s_head, first);
        memcpy(out + first, s_buffer, n - first);
        s_head = (s_head + n) % BUFFER_BYTES; s_used -= n;
        *status = s_used ? MUSE_TTS_PENDING : s_status;
    }
    xSemaphoreGive(s_lock);
    return n;
}
#endif
