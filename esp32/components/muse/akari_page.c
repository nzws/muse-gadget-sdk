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

/*
 * Akari dynamic page: see akari_page.h for the contract. The endpoint
 * URLs come from the gitignored akari_tts_secrets.h (template:
 * akari_tts_secrets.example.h), the Bearer token is the TTS endpoint's,
 * and nothing in this file logs either. Fetching, parsing and action
 * POSTs all run on one PSRAM task; the UI and the input task only ever
 * touch the parsed model, the bindings and the toast through a mutex.
 */

#include "akari_page.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "muse_state.h"
#include "muse_voice.h"

#if __has_include("akari_tts_secrets.h")
#include "akari_tts_secrets.h"
#endif
#ifndef AKARI_PAGE_URL
#define AKARI_PAGE_URL ""
#endif
#ifndef AKARI_HOOK_URL
#define AKARI_HOOK_URL ""
#endif
#ifndef AKARI_TTS_TOKEN
#define AKARI_TTS_TOKEN ""
#endif

static const char *TAG = "akari_page";

/* A page (or an action answer carrying one), capped against a broken
 * server; real pages are a few KB. */
#define PAGE_MAX (32 * 1024)
/* While the page tile stays visible, refetch this often. */
#define REFETCH_US (120 * 1000000LL)
#define TOAST_US (3500 * 1000LL)
#define TOAST_MAX 120   /* bytes, cut on a whole UTF-8 character */

typedef struct {
    char id[AKARI_PAGE_ID_MAX];
    char source[12];   /* "tap" / "key_talk" / "key_fn" */
} page_job_t;

static akari_page_model_t *s_model;   /* the parsed page, in PSRAM */
static char s_bind[2][AKARI_PAGE_ID_MAX];   /* bindings by AKARI_PAGE_KEY_* */
static char s_toast[TOAST_MAX + 1];
static int64_t s_toast_until_us;
static volatile unsigned s_version;   /* bumped on page / binding / toast change */
static volatile bool s_active;        /* the page tile is visible */
static volatile bool s_have_page;     /* a page has loaded at least once */
static volatile bool s_failed;        /* the last fetch failed, with no page yet */
static volatile bool s_fetch_wanted;  /* the task fetches at its next loop */
static int64_t s_last_fetch_us;
static QueueHandle_t s_jobs;
static SemaphoreHandle_t s_lock;
static bool s_ready;
static bool s_init_started;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;

bool akari_page_configured(void)
{
    return AKARI_PAGE_URL[0] && AKARI_TTS_TOKEN[0];
}

static void *psram_malloc(size_t n)
{
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

/* ---- Small helpers ---- */

/* `src` into `dst`, capped at cap-1 bytes, backing off to the start of
 * a UTF-8 character rather than cutting one in half. */
static void utf8_copy(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && (src[n] & 0xC0) == 0x80) {
            n--;   /* a continuation byte: its character starts earlier */
        }
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void set_toast(const char *text)
{
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    utf8_copy(s_toast, sizeof(s_toast), text);
    s_toast_until_us = esp_timer_get_time() + TOAST_US;
    s_version++;
    xSemaphoreGive(s_lock);
}

/* ---- HTTP (page task only) ---- */

/* A response body collected in PSRAM by the event handler. */
typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
    bool overflow;
} body_t;

static body_t *s_collect;

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA || !evt->data_len || !s_collect) {
        return ESP_OK;
    }
    body_t *b = s_collect;
    if (b->len + (size_t)evt->data_len > PAGE_MAX - 1) {   /* one byte spare for the NUL */
        b->overflow = true;
        return ESP_FAIL;
    }
    if (b->len + (size_t)evt->data_len > b->cap) {
        size_t cap = (b->len + (size_t)evt->data_len) * 2 + 1;
        uint8_t *grown = heap_caps_realloc(b->data, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!grown) {
            return ESP_FAIL;
        }
        b->data = grown;
        b->cap = cap;
    }
    memcpy(b->data + b->len, evt->data, (size_t)evt->data_len);
    b->len += (size_t)evt->data_len;
    return ESP_OK;
}

/*
 * One request on a fresh client (the TTS module's keep-alive dance is
 * for streaming; a page fetch is one short exchange). Returns the HTTP
 * status once the exchange completed, or -1 on a transport failure.
 * The whole response lands in `body` (caller frees body->data), NUL
 * terminated. Only the status is ever logged: the URLs and the
 * Authorization header stay out of the logs.
 */
static int http_do(esp_http_client_method_t method, const char *url, const char *payload, body_t *body)
{
    memset(body, 0, sizeof(*body));
    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = http_event,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return -1;
    }
    char auth[32 + sizeof(AKARI_TTS_TOKEN)];
    snprintf(auth, sizeof(auth), "Bearer %s", AKARI_TTS_TOKEN);
    if (payload) {
        /* Post field first: setting it can touch Content-Type, so the
         * explicit headers below must come after it. */
        esp_http_client_set_post_field(client, payload, (int)strlen(payload));
    }
    esp_http_client_set_header(client, "Authorization", auth);
    if (payload) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
    }
    s_collect = body;
    esp_err_t err = esp_http_client_perform(client);
    s_collect = NULL;
    int status = err == ESP_OK ? esp_http_client_get_status_code(client) : -1;
    esp_http_client_cleanup(client);
    if (status < 0) {
        free(body->data);
        body->data = NULL;
        body->len = 0;
        return -1;
    }
    if (body->data) {
        body->data[body->len] = '\0';   /* the collector kept a byte spare */
    }
    if (body->overflow) {
        return -1;
    }
    return status;
}

/* ---- The page JSON ---- */

static bool parse_button(cJSON *obj, akari_button_t *out)
{
    memset(out, 0, sizeof(*out));
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(obj, "id"));
    if (!id || !id[0]) {
        return false;
    }
    utf8_copy(out->id, sizeof(out->id), id);
    const char *label = cJSON_GetStringValue(cJSON_GetObjectItem(obj, "label"));
    utf8_copy(out->label, sizeof(out->label), label && label[0] ? label : id);
    out->confirm = cJSON_IsTrue(cJSON_GetObjectItem(obj, "confirm"));
    out->danger = cJSON_IsTrue(cJSON_GetObjectItem(obj, "danger"));
    return true;
}

/*
 * Parses a page document into `m` and the two bindings. Unknown block
 * types are skipped; a document without a blocks array is not a page.
 */
static bool parse_page(cJSON *root, akari_page_model_t *m, char (*bind)[AKARI_PAGE_ID_MAX])
{
    memset(m, 0, sizeof(*m));
    bind[0][0] = bind[1][0] = '\0';
    const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(root, "title"));
    if (title) {
        utf8_copy(m->title, sizeof(m->title), title);
    }
    cJSON *bindings = cJSON_GetObjectItem(root, "bindings");
    if (cJSON_IsObject(bindings)) {
        const char *talk = cJSON_GetStringValue(cJSON_GetObjectItem(bindings, "talk"));
        if (talk) {
            utf8_copy(bind[AKARI_PAGE_KEY_TALK], AKARI_PAGE_ID_MAX, talk);
        }
        const char *fn = cJSON_GetStringValue(cJSON_GetObjectItem(bindings, "fn"));
        if (fn) {
            utf8_copy(bind[AKARI_PAGE_KEY_FN], AKARI_PAGE_ID_MAX, fn);
        }
    }
    cJSON *blocks = cJSON_GetObjectItem(root, "blocks");
    if (!cJSON_IsArray(blocks)) {
        return false;
    }
    int n = 0;
    cJSON *b;
    cJSON_ArrayForEach(b, blocks) {
        if (n >= AKARI_PAGE_MAX_BLOCKS) {
            break;
        }
        if (!cJSON_IsObject(b)) {
            continue;
        }
        const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(b, "type"));
        if (!type) {
            continue;
        }
        akari_block_t *out = &m->blocks[n];
        memset(out, 0, sizeof(*out));
        if (!strcmp(type, "title") || !strcmp(type, "text") || !strcmp(type, "caption")) {
            const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(b, "text"));
            if (!text) {
                continue;
            }
            out->kind = !strcmp(type, "title") ? AKB_TITLE : !strcmp(type, "text") ? AKB_TEXT : AKB_CAPTION;
            utf8_copy(out->text, sizeof(out->text), text);
        } else if (!strcmp(type, "divider")) {
            out->kind = AKB_DIVIDER;
        } else if (!strcmp(type, "spacer")) {
            out->kind = AKB_SPACER;
            cJSON *h = cJSON_GetObjectItem(b, "h");
            out->spacer_h = cJSON_IsNumber(h) ? (int)h->valuedouble : 12;
            if (out->spacer_h < 0) {
                out->spacer_h = 0;
            }
            if (out->spacer_h > 96) {
                out->spacer_h = 96;
            }
        } else if (!strcmp(type, "button")) {
            if (!parse_button(b, &out->btn[0])) {
                continue;
            }
            out->kind = AKB_BUTTON;
            out->n_btn = 1;
        } else if (!strcmp(type, "buttons")) {
            cJSON *row = cJSON_GetObjectItem(b, "buttons");
            if (!cJSON_IsArray(row)) {
                continue;
            }
            int got = 0;
            cJSON *one;
            cJSON_ArrayForEach(one, row) {
                if (got >= 2) {
                    break;
                }
                if (cJSON_IsObject(one) && parse_button(one, &out->btn[got])) {
                    got++;
                }
            }
            if (!got) {
                continue;
            }
            out->kind = AKB_BUTTON_ROW;
            out->n_btn = got;
        } else {
            continue;   /* an unknown block type: skip it */
        }
        n++;
    }
    m->n_blocks = n;
    return true;
}

/* Installs a parsed page (and its bindings) as the current one. */
static void install_page(akari_page_model_t *m, char (*bind)[AKARI_PAGE_ID_MAX])
{
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        free(m);
        return;
    }
    free(s_model);
    s_model = m;
    memcpy(s_bind, bind, sizeof(s_bind));
    s_have_page = true;
    s_failed = false;
    s_version++;
    xSemaphoreGive(s_lock);
}

static bool adopt_json(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return false;
    }
    akari_page_model_t *m = psram_malloc(sizeof(*m));
    char bind[2][AKARI_PAGE_ID_MAX];
    bool ok = m && parse_page(root, m, bind);
    cJSON_Delete(root);
    if (!ok) {
        free(m);
        return false;
    }
    install_page(m, bind);
    return true;
}

/* ---- The page task ---- */

static void do_fetch(void)
{
    s_last_fetch_us = esp_timer_get_time();
    body_t body;
    int status = http_do(HTTP_METHOD_GET, AKARI_PAGE_URL, NULL, &body);
    bool ok = status == 200 && body.data && adopt_json((const char *)body.data);
    free(body.data);
    if (ok) {
        ESP_LOGI(TAG, "page loaded");
    } else {
        ESP_LOGW(TAG, "page fetch failed (status %d)", status);
        if (!s_have_page && !s_failed) {
            s_failed = true;   /* the UI can stop saying "loading" */
            s_version++;
        }
    }
}

/* Escapes `src` into `dst` for a JSON string value (ids and sources
 * come from the hosted page, so don't trust them to be tame). */
static void json_escape(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    for (const char *p = src; *p && o + 2 < cap; p++) {
        if (*p == '"' || *p == '\\') {
            dst[o++] = '\\';
        }
        dst[o++] = *p;
    }
    dst[o] = '\0';
}

static void do_action(const page_job_t *job)
{
    char id[AKARI_PAGE_ID_MAX * 2], source[24], payload[256];
    json_escape(id, sizeof(id), job->id);
    json_escape(source, sizeof(source), job->source);
    snprintf(payload, sizeof(payload), "{\"action_id\":\"%s\",\"source\":\"%s\"}", id, source);
    body_t body = { 0 };   /* http_do fills it, but not when the hook URL is empty */
    int status = AKARI_HOOK_URL[0] ? http_do(HTTP_METHOD_POST, AKARI_HOOK_URL, payload, &body) : -1;
    if (status != 200) {
        ESP_LOGW(TAG, "action %s failed (status %d)", job->id, status);
        free(body.data);
        set_toast("送信に失敗しました");
        return;
    }
    if (body.data && body.data[0]) {
        cJSON *root = cJSON_Parse((const char *)body.data);
        if (root) {
            const char *toast = cJSON_GetStringValue(cJSON_GetObjectItem(root, "toast"));
            if (toast && toast[0]) {
                set_toast(toast);
            }
            cJSON *page = cJSON_GetObjectItem(root, "page");
            if (cJSON_IsObject(page)) {
                akari_page_model_t *m = psram_malloc(sizeof(*m));
                char bind[2][AKARI_PAGE_ID_MAX];
                if (m && parse_page(page, m, bind)) {
                    install_page(m, bind);
                } else {
                    free(m);
                }
            }
            if (cJSON_IsTrue(cJSON_GetObjectItem(root, "reload"))) {
                s_fetch_wanted = true;
            }
            cJSON_Delete(root);
        }
    }
    free(body.data);
}

static void page_task(void *arg)
{
    (void)arg;
    for (;;) {
        page_job_t job;
        if (xQueueReceive(s_jobs, &job, pdMS_TO_TICKS(250)) == pdTRUE) {
            do_action(&job);
        }
        if (s_fetch_wanted) {
            s_fetch_wanted = false;
            do_fetch();
        } else if (s_active && s_last_fetch_us &&
                   esp_timer_get_time() - s_last_fetch_us >= REFETCH_US) {
            do_fetch();   /* still on the page two minutes later */
        }
    }
}

/* Creates the mutex, queue and task, once, from whichever task gets
 * there first; a caller that loses the claim race just retries later. */
static bool page_spawn(void)
{
    if (s_ready) {
        return true;
    }
    taskENTER_CRITICAL(&s_init_mux);
    bool mine = !s_init_started;
    s_init_started = true;
    taskEXIT_CRITICAL(&s_init_mux);
    if (!mine) {
        return s_ready;   /* another task is bringing it up */
    }
    s_lock = xSemaphoreCreateMutex();
    s_jobs = xQueueCreate(6, sizeof(page_job_t));
    if (!s_lock || !s_jobs ||
        xTaskCreateWithCaps(page_task, "akari_page", 32768, NULL, 5, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGW(TAG, "no memory for the page task");
        return false;
    }
    s_ready = true;
    return true;
}

/* ---- Public API ---- */

unsigned akari_page_version(void)
{
    return s_version;
}

akari_page_model_t *akari_page_get(void)
{
    if (!s_ready || !s_lock) {
        return NULL;
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return NULL;
    }
    akari_page_model_t *copy = NULL;
    if (s_model) {
        copy = psram_malloc(sizeof(*copy));
        if (copy) {
            memcpy(copy, s_model, sizeof(*copy));
        }
    }
    xSemaphoreGive(s_lock);
    return copy;
}

bool akari_page_failed(void)
{
    return !akari_page_configured() || s_failed;
}

void akari_page_set_active(bool active)
{
    bool was = s_active;
    s_active = active;
    if (active && !was && akari_page_configured() && page_spawn()) {
        s_fetch_wanted = true;
    }
}

void akari_page_action(const char *id, const char *source)
{
    if (!id || !id[0] || !page_spawn()) {
        return;
    }
    page_job_t job;
    memset(&job, 0, sizeof(job));
    utf8_copy(job.id, sizeof(job.id), id);
    utf8_copy(job.source, sizeof(job.source), source && source[0] ? source : "tap");
    if (xQueueSend(s_jobs, &job, 0) != pdTRUE) {
        ESP_LOGW(TAG, "action queue full; dropping %s", job.id);
    }
}

bool akari_page_toast(char *out, size_t cap)
{
    if (!s_ready || !s_lock || !cap) {
        return false;
    }
    bool live = false;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        live = s_toast[0] && esp_timer_get_time() < s_toast_until_us;
        if (live) {
            strlcpy(out, s_toast, cap);
        }
        xSemaphoreGive(s_lock);
    }
    return live;
}

static bool binding_for(int key, char *id_out)
{
    if (key < 0 || key > 1 || !s_ready || !s_lock) {
        return false;
    }
    bool bound = false;
    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        bound = s_bind[key][0] != '\0';
        if (bound && id_out) {
            strlcpy(id_out, s_bind[key], AKARI_PAGE_ID_MAX);
        }
        xSemaphoreGive(s_lock);
    }
    return bound;
}

bool akari_page_key_bound(int key)
{
    return s_active && binding_for(key, NULL);
}

bool akari_page_key(int key)
{
    if (!s_active || !akari_page_configured()) {
        return false;
    }
    /* A press mid-recording starts (or ends) a turn; a press during a
     * call belongs to the call. Neither is the page's to take. */
    if (muse_state_mode(NULL) == MUSE_MODE_LISTENING || muse_voice_in_call()) {
        return false;
    }
    char id[AKARI_PAGE_ID_MAX];
    if (!binding_for(key, id)) {
        return false;
    }
    akari_page_action(id, key == AKARI_PAGE_KEY_TALK ? "key_talk" : "key_fn");
    muse_state_poke();   /* an action fired: keep the screen up for its toast */
    return true;
}
