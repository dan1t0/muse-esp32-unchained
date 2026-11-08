/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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
// Modified by Dani Martinez - dan1t0 (2026): adds the >web_cookie= console command.

#include "sdk_token_console.h"

#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

#include "identity.h"
#if CONFIG_HOMEHUB_WEB_SESSION
#include "muse_web_auth.h"
#endif

// The chip's own USB port, where it has one and the log goes to it.
#define USJ_CONSOLE (SOC_USB_SERIAL_JTAG_SUPPORTED && \
                     (CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG || CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG))
#if USJ_CONSOLE
#include "driver/usb_serial_jtag.h"
#endif

static const char *TAG = "link.sdk_token";

#define COMMAND "sdk_token="
#if CONFIG_HOMEHUB_WEB_SESSION
#define WEB_COOKIE_COMMAND "web_cookie="
#endif
// A command line: '>', the command and an SDK token or a web session
// cookie (hatch_sess runs a few hundred characters), with room to spare.
#define COMMAND_LINE_MAX 1024
// The reader tasks drain one byte at a time; the driver must hold a whole
// line's burst or the tail is dropped before the reader sees it.
#define READER_RX_BUFFER 2048

bool sdk_token_console_command(const char *line) {
    if (strncmp(line, COMMAND, strlen(COMMAND)) != 0) {
#if CONFIG_HOMEHUB_WEB_SESSION
        // The web session's cookie (muse_web_auth.h): same delivery as the
        // SDK token, same answer protocol. It's serial-only, so it can't be
        // set over BLE.
        if (strncmp(line, WEB_COOKIE_COMMAND,
                    strlen(WEB_COOKIE_COMMAND)) == 0) {
            const char *value = line + strlen(WEB_COOKIE_COMMAND);
            if (!muse_web_cookie_valid(value)) {
                ESP_LOGW(TAG, "not a web session cookie");
                printf("@web_cookie error invalid\n");
                fflush(stdout);
                return true;
            }
            if (!muse_web_set_cookie(value)) {
                ESP_LOGE(TAG, "couldn't store the web session cookie");
                printf("@web_cookie error storage\n");
                fflush(stdout);
                return true;
            }
            ESP_LOGI(TAG, "stored web session cookie %.8s…, restarting to use it",
                     value);
            printf("@web_cookie ok\n");
            fflush(stdout);
            vTaskDelay(pdMS_TO_TICKS(500));   // let the answer reach the host
            esp_restart();
            return true;
        }
#endif
        return false;
    }
    const char *token = line + strlen(COMMAND);
    if (!identity_sdk_token_valid(token)) {
        ESP_LOGW(TAG, "not an SDK token");
        printf("@sdk_token error invalid\n");
        fflush(stdout);
        return true;
    }
    if (!identity_set_sdk_token(token)) {
        ESP_LOGE(TAG, "couldn't store the SDK token");
        printf("@sdk_token error storage\n");
        fflush(stdout);
        return true;
    }
    ESP_LOGI(TAG, "stored SDK token %.12s, restarting to use it", token);
    printf("@sdk_token ok\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(500));   // let the answer reach the host
    esp_restart();
    return true;
}

// Collects bytes into a line and runs it on '\n' if it starts with '>'.
// Longer lines are dropped.
typedef struct {
    char buf[COMMAND_LINE_MAX];
    size_t len;
    bool overflow;
} line_reader_t;

static void feed(line_reader_t *r, char c) {
    if (c == '\r') return;
    if (c != '\n') {
        if (r->len < sizeof(r->buf) - 1) {
            r->buf[r->len++] = c;
        } else {
            r->overflow = true;
        }
        return;
    }
    r->buf[r->len] = '\0';
    if (!r->overflow && r->buf[0] == '>') sdk_token_console_command(r->buf + 1);
    r->len = 0;
    r->overflow = false;
}

#if CONFIG_ESP_CONSOLE_UART
static void uart_task(void *arg) {
    (void)arg;
    line_reader_t r = {0};
    for (;;) {
        uint8_t c;
        if (uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, &c, 1, portMAX_DELAY) == 1) feed(&r, (char)c);
    }
}
#endif

#if USJ_CONSOLE
static void usj_task(void *arg) {
    (void)arg;
    line_reader_t r = {0};
    for (;;) {
        uint8_t c;
        if (usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY) == 1) feed(&r, (char)c);
    }
}
#endif

void sdk_token_console_start(void) {
#if CONFIG_ESP_CONSOLE_UART
    // With the driver installed, the log goes through it too, so it can't
    // interleave with what the driver sends.
    if (uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, READER_RX_BUFFER, 0, 0, NULL, 0) == ESP_OK) {
        uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);
        xTaskCreate(uart_task, "sdk_token_uart", 4096, NULL, 3, NULL);
    } else {
        ESP_LOGW(TAG, "no console UART; can't set the SDK token over it");
    }
#endif
#if USJ_CONSOLE
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = READER_RX_BUFFER;
    if (usb_serial_jtag_driver_install(&cfg) == ESP_OK) {
        xTaskCreate(usj_task, "sdk_token_usj", 4096, NULL, 3, NULL);
    } else {
        ESP_LOGW(TAG, "no USB Serial/JTAG driver; can't set the SDK token over it");
    }
#endif
}
