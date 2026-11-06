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
/*
 * Speech held back before the session may start draining a message.
 * The engine generates streaming audio at only ~1.05x real time on the
 * host CPU, so starting playback the instant the first bytes land
 * leaves no cushion against network jitter; 0.75 s of head start does.
 */
#define PREBUF_FRAMES (AKARI_RATE * 3 / 4)
/* One sentence's audio_query answer, capped against a broken server. */
#define QUERY_MAX (256 * 1024)
/* One sentence's streamed audio, capped the same way (~2 min of speech). */
#define STREAM_MAX_BYTES (AKARI_RATE * 2 * 125)
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
static bool s_prebuf;   /* holding speech back until PREBUF_FRAMES land */

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

/* ---- Streaming WAV ---- */

/*
 * Incremental conversion of the /streaming_synthesis response to
 * 16 kHz mono PCM in the session's buffer.
 *
 * Observed wire format (engine 0.26.1, measured on the host): one
 * canonical WAV — a 44-byte header (RIFF, fmt, data; sizes filled in,
 * 16 kHz mono PCM16 as requested) followed by raw PCM, delivered in
 * bursts of ~0.25 s of audio as generation proceeds. The parser below
 * still walks the header chunks properly, so extra chunks or a
 * different fmt size do not break it; the data chunk's declared size
 * is not trusted as a terminator, the end of the stream is.
 */
typedef struct {
    int channels;
    uint32_t rate;
    resampler_t rs;
    int16_t stage[1024];
    size_t staged;
    int16_t first_channel;   /* stereo pairing across read boundaries */
    bool have_first;
    uint8_t byte_carry;      /* sample alignment across read boundaries */
    bool have_carry;
} pcm_sink_t;

static bool sink_flush(uint32_t gen, pcm_sink_t *s)
{
    if (!s->staged) {
        return true;
    }
    bool ok = push_pcm(gen, s->stage, s->staged);
    s->staged = 0;
    return ok;
}

static bool sink_mono(uint32_t gen, pcm_sink_t *s, int16_t v)
{
    if (s->rate == AKARI_RATE) {
        s->stage[s->staged++] = v;
        return s->staged < sizeof(s->stage) / sizeof(s->stage[0]) || sink_flush(gen, s);
    }
    int16_t out[4];
    size_t n = resample(&s->rs, &v, 1, out);
    for (size_t k = 0; k < n; k++) {
        s->stage[s->staged++] = out[k];
        if (s->staged == sizeof(s->stage) / sizeof(s->stage[0]) && !sink_flush(gen, s)) {
            return false;
        }
    }
    return true;
}

static bool sink_bytes(uint32_t gen, pcm_sink_t *s, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (!s->have_carry) {
            s->byte_carry = p[i];
            s->have_carry = true;
            continue;
        }
        int16_t v = (int16_t)(s->byte_carry | ((uint16_t)p[i] << 8));
        s->have_carry = false;
        if (s->channels == 2) {
            if (!s->have_first) {
                s->first_channel = v;
                s->have_first = true;
                continue;
            }
            v = (int16_t)(((int32_t)s->first_channel + v) / 2);
            s->have_first = false;
        }
        if (!sink_mono(gen, s, v)) {
            return false;
        }
    }
    return true;
}

/*
 * Locates the fmt and data chunks in the response prefix. Returns the
 * offset where PCM data starts, -1 when more header bytes are needed,
 * or -2 on a malformed header. A non-data chunk reaching past the
 * prefix is reported malformed: this engine's header is 44 bytes, and
 * failing the sentence beats growing an unbounded header buffer.
 */
#define WAV_PREFIX_MAX 4096
static long wav_prefix(const uint8_t *buf, size_t len, pcm_sink_t *s)
{
    if (len < 12) {
        return -1;
    }
    if (memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) {
        ESP_LOGW(TAG, "TTS response is not WAV");
        return -2;
    }
    bool have_fmt = false;
    size_t off = 12;
    while (off + 8 <= len) {
        uint32_t size = (uint32_t)buf[off + 4] | ((uint32_t)buf[off + 5] << 8) |
                        ((uint32_t)buf[off + 6] << 16) | ((uint32_t)buf[off + 7] << 24);
        if (!memcmp(buf + off, "fmt ", 4)) {
            if (off + 8 + 16 > len) {
                return -1;
            }
            const uint8_t *f = buf + off + 8;
            int format = f[0] | (f[1] << 8);
            int channels = f[2] | (f[3] << 8);
            uint32_t rate = (uint32_t)f[4] | ((uint32_t)f[5] << 8) |
                            ((uint32_t)f[6] << 16) | ((uint32_t)f[7] << 24);
            int bits = f[14] | (f[15] << 8);
            if (size < 16 || format != 1 || bits != 16 ||
                (channels != 1 && channels != 2) || !rate) {
                ESP_LOGW(TAG, "TTS WAV is not PCM16 mono/stereo");
                return -2;
            }
            s->channels = channels;
            s->rate = rate;
            resampler_init(&s->rs, rate);
            have_fmt = true;
        } else if (!memcmp(buf + off, "data", 4)) {
            if (!have_fmt) {
                ESP_LOGW(TAG, "TTS WAV data before fmt");
                return -2;
            }
            return (long)(off + 8);
        } else if (off + 8 + size > len) {
            return -2;
        }
        off += 8 + size + (size & 1);
    }
    return len >= WAV_PREFIX_MAX ? -2 : -1;
}

/*
 * POSTs one tuned audio_query to /streaming_synthesis and feeds the
 * PCM to the session as it arrives, instead of waiting for the whole
 * sentence. False on any failure (the caller marks the message failed
 * and the session falls back to reading pace for its remainder; PCM
 * already queued still plays). As everywhere here, only the HTTP
 * status is logged — never the URL or the Authorization header.
 */
static bool stream_synthesis(uint32_t gen, const char *payload)
{
    char base[192];
    base_url(base, sizeof(base));
    const char *path = "/streaming_synthesis?speaker=";
    char *url = psram_malloc(strlen(base) + strlen(path) + 4);
    if (!url) {
        return false;
    }
    strcpy(url, base);
    strcat(url, path);
    char id[4];
    snprintf(id, sizeof(id), "%d", AKARI_SPEAKER_ID);
    strcat(url, id);

    char auth[32 + sizeof(AKARI_TTS_TOKEN)];
    snprintf(auth, sizeof(auth), "Bearer %s", AKARI_TTS_TOKEN);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 20000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    bool ok = false;
    if (client) {
        size_t payload_len = strlen(payload);
        esp_http_client_set_header(client, "Authorization", auth);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        if (esp_http_client_open(client, payload_len) == ESP_OK) {
            size_t written = 0;
            while (written < payload_len) {
                int w = esp_http_client_write(client, payload + written,
                                              payload_len - written);
                if (w <= 0) {
                    break;
                }
                written += (size_t)w;
            }
            if (written == payload_len &&
                esp_http_client_fetch_headers(client) >= 0) {
                int status = esp_http_client_get_status_code(client);
                if (status != 200) {
                    ESP_LOGW(TAG, "TTS request failed (status %d)", status);
                } else {
                    ok = true;
                }
            }
        }
        if (ok) {
            pcm_sink_t sink = { .channels = 1, .rate = 0 };
            uint8_t prefix[WAV_PREFIX_MAX];
            size_t prefix_len = 0;
            bool in_data = false;
            size_t total = 0;
            uint8_t buf[2048];
            while (ok) {
                if (gen != s_gen) {
                    ok = false;
                    break;
                }
                int r = esp_http_client_read(client, (char *)buf, sizeof(buf));
                if (r < 0) {
                    ok = false;
                    break;
                }
                if (r == 0) {
                    break;   /* end of the stream */
                }
                total += (size_t)r;
                if (total > STREAM_MAX_BYTES) {
                    ok = false;
                    break;
                }
                const uint8_t *p = buf;
                size_t n = (size_t)r;
                if (!in_data) {
                    size_t take = sizeof(prefix) - prefix_len;
                    if (take > n) {
                        take = n;
                    }
                    memcpy(prefix + prefix_len, p, take);
                    prefix_len += take;
                    long used = wav_prefix(prefix, prefix_len, &sink);
                    if (used == -1) {
                        if (take < n) {
                            ok = false;   /* header alone filled the prefix */
                        }
                        continue;
                    }
                    if (used < 0) {
                        ok = false;
                        break;
                    }
                    in_data = true;
                    /* PCM that arrived with the header, then this read's
                     * bytes beyond what the prefix copied. */
                    if (!sink_bytes(gen, &sink, prefix + used, prefix_len - (size_t)used)) {
                        ok = false;
                        break;
                    }
                    p += take;
                    n -= take;
                    if (!n) {
                        continue;
                    }
                }
                if (!sink_bytes(gen, &sink, p, n)) {
                    ok = false;
                }
            }
            if (ok && !in_data) {
                ok = false;   /* stream ended before any audio */
            }
            if (ok) {
                ok = sink_flush(gen, &sink);
            }
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    free(url);
    return ok;
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

    ok = stream_synthesis(gen, tuned);
    cJSON_free(tuned);
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
    s_prebuf = true;
    return true;
}

size_t akari_tts_read(int16_t *out, size_t max_frames)
{
    if (!s_ready) {
        return 0;
    }
    if (s_prebuf) {
        /*
         * Hold the start of a message back until a cushion of speech
         * has landed (or synthesis ended, for short messages), so the
         * near-real-time generation rate cannot starve playback the
         * moment it starts. akari_tts_available() keeps reporting the
         * held frames, so the session does not finish the message
         * early; once released, reads drain normally.
         */
        if (s_state == AK_DONE || s_state == AK_ERROR) {
            s_prebuf = false;
        } else if (akari_tts_available() < PREBUF_FRAMES) {
            return 0;
        } else {
            s_prebuf = false;
        }
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
    s_prebuf = false;
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
