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
 * Akari voice: VOICEVOX TTS for the chat session's replies. See
 * akari_tts.h for the contract and docs/akari-voicevox-tts.md for the
 * design. The endpoint URL and Bearer token come from the gitignored
 * akari_tts_secrets.h (template: akari_tts_secrets.example.h); nothing in
 * this file logs either of them.
 */

#include "akari_tts.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#if __has_include("akari_tts_secrets.h")
#include "akari_tts_secrets.h"
#endif
#ifndef AKARI_TTS_URL
#define AKARI_TTS_URL ""
#endif
#ifndef AKARI_TTS_TOKEN
#define AKARI_TTS_TOKEN ""
#endif

static const char *TAG = "akari_tts";

/* The decided voice: VOICEVOX style id and speaking rate. */
#define AKARI_SPEAKER_ID 17
#define AKARI_SPEED_SCALE 1.15
#define AKARI_RATE 16000   /* what we ask the engine for; the reply rate */

/* Synthesized speech waiting for the session: ~6 s of 16 kHz mono. */
#define PCM_BUF_BYTES (AKARI_RATE * 2 * 6)
/* One sentence's audio_query answer / WAV, caps against a broken server. */
#define QUERY_MAX (256 * 1024)
#define WAV_MAX (4 * 1024 * 1024)
/* A sentence longer than this is cut at a nearby punctuation mark, so the
 * first audio of a long reply is not held back by its tail. */
#define SENTENCE_MAX 120

typedef enum { AK_IDLE, AK_RUNNING, AK_DONE, AK_ERROR } ak_state_t;

typedef struct {
    char *text;      /* cleaned reply text, owned by the job */
    uint32_t gen;
} ak_job_t;

static StreamBufferHandle_t s_pcm;
static QueueHandle_t s_jobs;
static volatile ak_state_t s_state = AK_IDLE;
static volatile uint32_t s_gen;
static bool s_ready;

bool akari_tts_configured(void)
{
    return AKARI_TTS_URL[0] && AKARI_TTS_TOKEN[0];
}

/* ---- Small helpers ---- */

static void *psram_malloc(size_t n)
{
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

/* Percent-encode UTF-8 text for a query value. PSRAM; caller frees. */
static char *url_encode(const char *s)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t cap = strlen(s) * 3 + 1;
    char *out = psram_malloc(cap);
    if (!out) {
        return NULL;
    }
    char *o = out;
    for (const uint8_t *p = (const uint8_t *)s; *p; p++) {
        uint8_t c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            *o++ = (char)c;
        } else {
            *o++ = '%';
            *o++ = hex[c >> 4];
            *o++ = hex[c & 15];
        }
    }
    *o = '\0';
    return out;
}

/* The endpoint base URL with any trailing slashes removed. */
static void base_url(char *out, size_t cap)
{
    size_t n = strlen(AKARI_TTS_URL);
    while (n && AKARI_TTS_URL[n - 1] == '/') {
        n--;
    }
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(out, AKARI_TTS_URL, n);
    out[n] = '\0';
}

/* ---- HTTP ---- */

/* A response body collected in PSRAM by the event handler. */
typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
    size_t max;
    bool overflow;
} body_t;

static esp_err_t body_event(esp_http_client_event_t *evt)
{
    body_t *b = evt->user_data;
    if (evt->event_id != HTTP_EVENT_ON_DATA || !evt->data_len) {
        return ESP_OK;
    }
    if (b->len + evt->data_len > b->max) {
        b->overflow = true;
        return ESP_FAIL;
    }
    if (b->len + evt->data_len > b->cap) {
        size_t cap = (b->len + evt->data_len) * 2;
        uint8_t *grown = heap_caps_realloc(b->data, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!grown) {
            return ESP_FAIL;
        }
        b->data = grown;
        b->cap = cap;
    }
    memcpy(b->data + b->len, evt->data, evt->data_len);
    b->len += evt->data_len;
    return ESP_OK;
}

/*
 * One POST to the TTS endpoint. `path` carries path + query. The whole
 * response lands in `body` (caller frees body->data). Only the HTTP
 * status is ever logged: the URL and the Authorization header stay out
 * of the logs.
 */
static bool http_post(const char *path, const char *payload, size_t payload_len, body_t *body)
{
    char base[192];
    base_url(base, sizeof(base));
    char *url = psram_malloc(strlen(base) + strlen(path) + 1);
    if (!url) {
        return false;
    }
    strcpy(url, base);
    strcat(url, path);

    char auth[32 + sizeof(AKARI_TTS_TOKEN)];
    snprintf(auth, sizeof(auth), "Bearer %s", AKARI_TTS_TOKEN);

    /* Reset the collection, keeping the caller's size cap. */
    body->data = NULL;
    body->len = 0;
    body->cap = 0;
    body->overflow = false;
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 20000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = body_event,
        .user_data = body,
        .buffer_size = 4096,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    bool ok = false;
    if (client) {
        esp_http_client_set_header(client, "Authorization", auth);
        if (payload) {
            esp_http_client_set_header(client, "Content-Type", "application/json");
            esp_http_client_set_post_field(client, payload, payload_len);
        }
        esp_err_t err = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        ok = err == ESP_OK && status == 200 && !body->overflow && body->data;
        if (!ok) {
            ESP_LOGW(TAG, "TTS request failed (status %d)", status);
        }
        esp_http_client_cleanup(client);
    }
    free(url);
    return ok;
}

/* ---- WAV ---- */

/* Linear resampler, in_rate -> AKARI_RATE, as in the chat session. */
typedef struct {
    uint32_t step;
    uint32_t pos;
    int16_t prev;
    bool started;
} resampler_t;

static void resampler_init(resampler_t *r, uint32_t in_rate)
{
    r->step = (uint32_t)(((uint64_t)in_rate << 16) / AKARI_RATE);
    r->pos = 0;
    r->prev = 0;
    r->started = false;
}

static size_t resample(resampler_t *r, const int16_t *in, size_t n, int16_t *out)
{
    if (!r->started) {
        if (!n) {
            return 0;
        }
        r->prev = in[0];
        r->started = true;
    }
    size_t o = 0;
    while ((r->pos >> 16) < n) {
        uint32_t idx = r->pos >> 16;
        int16_t a = idx == 0 ? r->prev : in[idx - 1];
        int16_t b = in[idx];
        out[o++] = (int16_t)(a + (int32_t)(((int64_t)(b - a) * (int64_t)(r->pos & 0xffff)) >> 16));
        r->pos += r->step;
    }
    if (n) {
        r->pos -= (uint32_t)n << 16;
        r->prev = in[n - 1];
    }
    return o;
}

/*
 * Pushes PCM into the session's buffer in chunks, pacing on a full
 * buffer (playback drains it in real time). False when the generation
 * moved on (aborted or superseded) or the drain stalled for good.
 */
static bool push_pcm(uint32_t gen, const int16_t *pcm, size_t frames)
{
    size_t sent = 0;
    int stalled_ms = 0;
    while (sent < frames) {
        if (gen != s_gen) {
            return false;
        }
        size_t chunk = frames - sent;
        if (chunk > 1024) {
            chunk = 1024;
        }
        size_t n = xStreamBufferSend(s_pcm, pcm + sent, chunk * sizeof(int16_t),
                                     pdMS_TO_TICKS(200)) / sizeof(int16_t);
        if (n) {
            sent += n;
            stalled_ms = 0;
        } else {
            stalled_ms += 200;
            if (stalled_ms > 10 * 60 * 1000) {
                return false;
            }
        }
    }
    return true;
}

/*
 * Finds the WAV data chunk in a complete /synthesis response, converts
 * to 16 kHz mono and queues it for the session. False on a malformed
 * file or an abort.
 */
static bool play_wav(uint32_t gen, const uint8_t *wav, size_t len)
{
    if (len < 44 || memcmp(wav, "RIFF", 4) || memcmp(wav + 8, "WAVE", 4)) {
        ESP_LOGW(TAG, "TTS response is not WAV");
        return false;
    }
    int channels = 1;
    uint32_t rate = 0;
    int bits = 16;
    size_t off = 12;
    const uint8_t *data = NULL;
    size_t data_len = 0;
    while (off + 8 <= len) {
        uint32_t size = (uint32_t)wav[off + 4] | ((uint32_t)wav[off + 5] << 8) |
                        ((uint32_t)wav[off + 6] << 16) | ((uint32_t)wav[off + 7] << 24);
        if (!memcmp(wav + off, "fmt ", 4) && size >= 16 && off + 8 + 16 <= len) {
            const uint8_t *f = wav + off + 8;
            int format = f[0] | (f[1] << 8);
            channels = f[2] | (f[3] << 8);
            rate = (uint32_t)f[4] | ((uint32_t)f[5] << 8) | ((uint32_t)f[6] << 16) |
                   ((uint32_t)f[7] << 24);
            bits = f[14] | (f[15] << 8);
            if (format != 1 || bits != 16 || (channels != 1 && channels != 2) || !rate) {
                ESP_LOGW(TAG, "TTS WAV is not PCM16 mono/stereo");
                return false;
            }
        } else if (!memcmp(wav + off, "data", 4)) {
            data = wav + off + 8;
            data_len = len - (off + 8);
            if (size && size < data_len) {
                data_len = size;
            }
            break;
        }
        off += 8 + size + (size & 1);
    }
    if (!data) {
        ESP_LOGW(TAG, "TTS WAV has no data chunk");
        return false;
    }

    /* Byte-wise walk: the response is whole, so alignment is free, but
     * the stereo pairing and the resampler carry across nothing else. */
    resampler_t rs;
    resampler_init(&rs, rate);
    int16_t stage[1024];
    size_t staged = 0;
    int16_t first_channel = 0;
    bool have_first = false;
    size_t samples = data_len / 2;
    const int16_t *pcm = (const int16_t *)data;
    for (size_t i = 0; i < samples; i++) {
        int16_t v = pcm[i];
        if (channels == 2) {
            if (!have_first) {
                first_channel = v;
                have_first = true;
                continue;
            }
            v = (int16_t)(((int32_t)first_channel + v) / 2);
            have_first = false;
        }
        if (rate == AKARI_RATE) {
            stage[staged++] = v;
            if (staged == sizeof(stage) / sizeof(stage[0])) {
                if (!push_pcm(gen, stage, staged)) {
                    return false;
                }
                staged = 0;
            }
        } else {
            int16_t one = v;
            int16_t out[4];
            size_t n = resample(&rs, &one, 1, out);
            for (size_t k = 0; k < n; k++) {
                stage[staged++] = out[k];
                if (staged == sizeof(stage) / sizeof(stage[0])) {
                    if (!push_pcm(gen, stage, staged)) {
                        return false;
                    }
                    staged = 0;
                }
            }
        }
    }
    return !staged || push_pcm(gen, stage, staged);
}

/* ---- VOICEVOX ---- */

/* Synthesizes one sentence and queues its speech. */
static bool speak_sentence(uint32_t gen, const char *sentence)
{
    char *enc = url_encode(sentence);
    if (!enc) {
        return false;
    }
    char *path = psram_malloc(strlen(enc) + 48);
    if (!path) {
        free(enc);
        return false;
    }
    snprintf(path, strlen(enc) + 48, "/audio_query?speaker=%d&text=%s", AKARI_SPEAKER_ID, enc);
    free(enc);

    body_t query = { .max = QUERY_MAX };
    bool ok = http_post(path, NULL, 0, &query);
    free(path);
    if (!ok) {
        free(query.data);
        return false;
    }
    if (query.len >= query.max) {   /* keep room for the NUL cJSON wants */
        free(query.data);
        return false;
    }
    query.data[query.len] = '\0';   /* body_event's growth leaves room via cap */
    cJSON *root = cJSON_Parse((const char *)query.data);
    free(query.data);
    if (!root) {
        ESP_LOGW(TAG, "TTS query answer is not JSON");
        return false;
    }
    cJSON *speed = cJSON_GetObjectItem(root, "speedScale");
    if (speed) {
        cJSON_SetNumberValue(speed, AKARI_SPEED_SCALE);
    } else {
        cJSON_AddNumberToObject(root, "speedScale", AKARI_SPEED_SCALE);
    }
    cJSON *rate = cJSON_GetObjectItem(root, "outputSamplingRate");
    if (rate) {
        cJSON_SetNumberValue(rate, AKARI_RATE);
    } else {
        cJSON_AddNumberToObject(root, "outputSamplingRate", AKARI_RATE);
    }
    cJSON_DeleteItemFromObject(root, "outputStereo");
    cJSON_AddBoolToObject(root, "outputStereo", false);
    char *tuned = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!tuned) {
        return false;
    }

    char synth_path[40];
    snprintf(synth_path, sizeof(synth_path), "/synthesis?speaker=%d", AKARI_SPEAKER_ID);
    body_t wav = { .max = WAV_MAX };
    ok = http_post(synth_path, tuned, strlen(tuned), &wav);
    cJSON_free(tuned);
    if (!ok) {
        free(wav.data);
        return false;
    }
    ok = play_wav(gen, wav.data, wav.len);
    free(wav.data);
    return ok;
}

/* ---- Sentences ---- */

static bool is_space_byte(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

/* Length of the UTF-8 sequence starting at p (1 for ASCII/invalid). */
static size_t utf8_len(const uint8_t *p)
{
    if (*p < 0x80) return 1;
    if ((*p & 0xe0) == 0xc0) return 2;
    if ((*p & 0xf0) == 0xe0) return 3;
    if ((*p & 0xf8) == 0xf0) return 4;
    return 1;
}

/*
 * The next sentence in `text` at or after `cursor`: a pointer into the
 * text and its byte length (not NUL-terminated), or NULL at the end.
 * Splits after 。！？ and newlines, after ASCII . ! ? when followed by a
 * space or the end, and near SENTENCE_MAX at a 、or a space so a long
 * unpunctuated run still starts speaking early.
 */
static const char *next_sentence(const char *cursor, size_t *out_len)
{
    const uint8_t *p = (const uint8_t *)cursor;
    while (*p && (is_space_byte((char)*p) || *p == '\n')) {
        p++;
    }
    if (!*p) {
        return NULL;
    }
    const uint8_t *start = p;
    size_t soft_cut = 0;
    while (*p) {
        size_t n = utf8_len(p);
        if (n == 3 && p[0] == 0xe3 && p[1] == 0x80 && p[2] == 0x82) {         /* 。 */
            *out_len = (p + 3) - start;
            return (const char *)start;
        }
        if (n == 3 && p[0] == 0xef && p[1] == 0xbc && (p[2] == 0x81 || p[2] == 0x9f)) {  /* ！？ */
            *out_len = (p + 3) - start;
            return (const char *)start;
        }
        if (*p == '\n') {
            *out_len = p - start;
            return (const char *)start;
        }
        if (n == 1 && (*p == '.' || *p == '!' || *p == '?') &&
            (p[1] == '\0' || is_space_byte((char)p[1]) || p[1] == '\n')) {
            *out_len = (p + 1) - start;
            return (const char *)start;
        }
        if (n == 3 && p[0] == 0xe3 && p[1] == 0x80 && p[2] == 0x81) {         /* 、 */
            soft_cut = (p + 3) - start;
        }
        if (n == 1 && *p == ' ') {
            soft_cut = p - start;
        }
        if ((size_t)(p - start) >= SENTENCE_MAX) {
            *out_len = soft_cut ? soft_cut : (size_t)(p - start);
            return (const char *)start;
        }
        p += n;
    }
    *out_len = p - start;
    return (const char *)start;
}

/* ---- The fetch task ---- */

static void tts_task(void *arg)
{
    for (;;) {
        ak_job_t job;
        if (xQueueReceive(s_jobs, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        bool ok = true;
        const char *cursor = job.text;
        const char *sentence;
        size_t len;
        while (ok && (sentence = next_sentence(cursor, &len)) != NULL) {
            if (job.gen != s_gen) {
                ok = false;
                break;
            }
            if (!len) {
                cursor = sentence + 1;
                continue;
            }
            char *one = psram_malloc(len + 1);
            if (!one) {
                ok = false;
                break;
            }
            memcpy(one, sentence, len);
            one[len] = '\0';
            ok = speak_sentence(job.gen, one);
            free(one);
            cursor = sentence + len;
        }
        free(job.text);
        if (job.gen == s_gen) {
            s_state = ok ? AK_DONE : AK_ERROR;
        }
    }
}

static bool tts_init(void)
{
    if (s_ready) {
        return true;
    }
    s_pcm = xStreamBufferCreateWithCaps(PCM_BUF_BYTES, 1, MALLOC_CAP_SPIRAM);
    s_jobs = xQueueCreate(1, sizeof(ak_job_t));
    if (!s_pcm || !s_jobs ||
        xTaskCreate(tts_task, "akari_tts", 8192, NULL, 5, NULL) != pdPASS) {
        ESP_LOGW(TAG, "no memory for the TTS task");
        return false;
    }
    s_ready = true;
    return true;
}

/* ---- Public API ---- */

bool akari_tts_start(const char *text)
{
    if (!akari_tts_configured() || !text || !text[0] || !tts_init()) {
        return false;
    }
    /* A light cleanup: markup characters the engine would trip over. */
    char *copy = psram_malloc(strlen(text) + 1);
    if (!copy) {
        return false;
    }
    char *o = copy;
    for (const char *p = text; *p; p++) {
        if (*p != '*' && *p != '`') {
            *o++ = *p;
        }
    }
    *o = '\0';
    if (!copy[0]) {
        free(copy);
        return false;
    }

    akari_tts_abort();   /* supersedes anything still being fetched */
    uint32_t gen = s_gen;
    /* Drop speech the previous text left unread. */
    int16_t junk[256];
    while (xStreamBufferReceive(s_pcm, junk, sizeof(junk), 0) > 0) {
    }
    ak_job_t job = { .text = copy, .gen = gen };
    if (xQueueOverwrite(s_jobs, &job) != pdTRUE) {
        free(copy);
        return false;
    }
    s_state = AK_RUNNING;
    return true;
}

size_t akari_tts_read(int16_t *out, size_t max_frames)
{
    if (!s_ready) {
        return 0;
    }
    return xStreamBufferReceive(s_pcm, out, max_frames * sizeof(int16_t), 0) / sizeof(int16_t);
}

size_t akari_tts_available(void)
{
    if (!s_ready) {
        return 0;
    }
    return xStreamBufferBytesAvailable(s_pcm) / sizeof(int16_t);
}

bool akari_tts_ended(void)
{
    return s_state == AK_DONE || s_state == AK_ERROR;
}

bool akari_tts_failed(void)
{
    return s_state == AK_ERROR;
}

void akari_tts_abort(void)
{
    s_gen++;
    if (s_state == AK_RUNNING) {
        s_state = AK_IDLE;
    }
    if (s_ready) {
        ak_job_t stale;
        if (xQueueReceive(s_jobs, &stale, 0) == pdTRUE) {
            free(stale.text);
        }
    }
}
