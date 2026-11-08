/* Copyright (c) 2026 Dani Martinez - dan1t0
 * SPDX-License-Identifier: Apache-2.0 */
#ifndef MUSE_TEST_TTS_HOST_H
#define MUSE_TEST_TTS_HOST_H
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) (ms)
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_HTTP_EAGAIN 0x7007
#define HTTP_METHOD_POST 1
#define HTTP_EVENT_ON_HEADER 2
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
typedef int esp_err_t;
typedef pthread_mutex_t *SemaphoreHandle_t;
typedef struct queue *QueueHandle_t;
typedef struct http *esp_http_client_handle_t;
typedef struct { int event_id; void *user_data; const char *header_key, *header_value; } esp_http_client_event_t;
typedef struct {
    const char *url; int method, timeout_ms, buffer_size, buffer_size_tx;
    bool disable_auto_redirect; void *crt_bundle_attach;
    esp_err_t (*event_handler)(esp_http_client_event_t *); void *user_data;
} esp_http_client_config_t;
#define esp_crt_bundle_attach NULL
void *heap_caps_malloc(size_t size, int caps);
void heap_caps_free(void *p);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t lock, uint32_t wait);
void xSemaphoreGive(SemaphoreHandle_t lock);
void vSemaphoreDelete(SemaphoreHandle_t lock);
QueueHandle_t xQueueCreateWithCaps(int count, size_t size, int caps);
int xQueueReceive(QueueHandle_t q, void *out, uint32_t wait);
int xQueueSend(QueueHandle_t q, const void *item, uint32_t wait);
void vQueueDeleteWithCaps(QueueHandle_t q);
int xTaskCreatePinnedToCoreWithCaps(void (*fn)(void *), const char *name, int stack,
                                  void *arg, int priority, void *handle, int core, int caps);
void vTaskDelay(uint32_t ms);
int64_t esp_timer_get_time(void);
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg);
int esp_http_client_set_header(esp_http_client_handle_t h, const char *key, const char *value);
int esp_http_client_open(esp_http_client_handle_t h, int len);
int esp_http_client_get_socket(esp_http_client_handle_t h);
int esp_http_client_write(esp_http_client_handle_t h, const char *buf, int len);
int esp_http_client_set_timeout_ms(esp_http_client_handle_t h, int ms);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t h);
int esp_http_client_get_status_code(esp_http_client_handle_t h);
int esp_http_client_read(esp_http_client_handle_t h, char *buf, int cap);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t h);
int esp_http_client_cleanup(esp_http_client_handle_t h);
#endif
