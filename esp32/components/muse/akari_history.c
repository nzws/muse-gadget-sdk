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

#include "akari_history.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "akari_history";

#define HISTORY_ENTRIES 12
#define HISTORY_TEXT_MAX 600   /* bytes of an entry's text, cut on a whole character */

typedef struct {
    bool is_user;
    char text[HISTORY_TEXT_MAX + 1];
} history_entry_t;

static history_entry_t *s_entries;   /* the ring, in PSRAM */
static int s_head;                   /* the slot the next add overwrites */
static int s_count;                  /* entries held, up to HISTORY_ENTRIES */
static volatile unsigned s_version;  /* bumped per add; read lock-free by the UI's poll */
static SemaphoreHandle_t s_lock;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;

static void *psram_malloc(size_t n)
{
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

/* Allocates the ring and the mutex on first use, from whichever task
 * gets there first. */
static bool history_init(void)
{
    if (s_entries && s_lock) {
        return true;
    }
    taskENTER_CRITICAL(&s_init_mux);
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (!s_entries) {
        s_entries = psram_malloc(sizeof(history_entry_t) * HISTORY_ENTRIES);
        if (!s_entries) {
            ESP_LOGW(TAG, "no memory for the history ring");
        }
    }
    taskEXIT_CRITICAL(&s_init_mux);
    return s_entries && s_lock;
}

/* The length of `text` capped at `max` bytes, backing off to the start
 * of a UTF-8 character rather than cutting one in half. */
static size_t utf8_capped_len(const char *text, size_t max)
{
    size_t n = strlen(text);
    if (n <= max) {
        return n;
    }
    size_t cut = max;
    while (cut > 0 && (text[cut] & 0xC0) == 0x80) {
        cut--;   /* a continuation byte: its character starts earlier */
    }
    return cut;
}

void akari_history_add(bool is_user, const char *text)
{
    if (!text || !text[0] || !history_init()) {
        return;
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    history_entry_t *e = &s_entries[s_head];
    e->is_user = is_user;
    size_t n = utf8_capped_len(text, HISTORY_TEXT_MAX);
    memcpy(e->text, text, n);
    e->text[n] = '\0';
    s_head = (s_head + 1) % HISTORY_ENTRIES;
    if (s_count < HISTORY_ENTRIES) {
        s_count++;
    }
    s_version++;
    xSemaphoreGive(s_lock);
}

unsigned akari_history_version(void)
{
    return s_version;
}

char *akari_history_render(void)
{
    static const char USER_HEAD[] = "自分：";
    static const char AKARI_HEAD[] = "朱莉：";
    if (!history_init()) {
        return NULL;
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return NULL;
    }
    int oldest = (s_head + HISTORY_ENTRIES - s_count) % HISTORY_ENTRIES;
    size_t total = 1;   /* the NUL */
    for (int k = 0; k < s_count; k++) {
        const history_entry_t *e = &s_entries[(oldest + k) % HISTORY_ENTRIES];
        total += strlen(e->is_user ? USER_HEAD : AKARI_HEAD) + strlen(e->text) + 2;   /* "\n\n" */
    }
    char *out = psram_malloc(total);
    if (out) {
        char *w = out;
        for (int k = 0; k < s_count; k++) {
            const history_entry_t *e = &s_entries[(oldest + k) % HISTORY_ENTRIES];
            if (k) {
                *w++ = '\n';
                *w++ = '\n';
            }
            const char *head = e->is_user ? USER_HEAD : AKARI_HEAD;
            size_t hn = strlen(head);
            memcpy(w, head, hn);
            w += hn;
            size_t tn = strlen(e->text);
            memcpy(w, e->text, tn);
            w += tn;
        }
        *w = '\0';
    }
    xSemaphoreGive(s_lock);
    return out;
}
