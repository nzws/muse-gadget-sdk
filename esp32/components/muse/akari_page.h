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

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Akari dynamic page: a Block-Kit-style page for the tile between the
 * clock and the chat history (see muse_ui.c). The page's content is a
 * small JSON document hosted next to the VOICEVOX endpoint: the watch
 * GETs it (AKARI_PAGE_URL from the gitignored akari_tts_secrets.h, the
 * same Bearer token as the TTS endpoint) and renders its blocks;
 * buttons on the page POST an action id to a webhook (AKARI_HOOK_URL),
 * whose JSON answer can toast, replace the page or ask for a reload.
 * Editing the hosted JSON is all it takes to change what the page
 * shows and does; nothing here is specific to one page.
 *
 * While the page is the visible tile, the page JSON may also bind the
 * physical talk and speaker ("fn") buttons to action ids; the input
 * task asks akari_page_key() before giving a press its usual meaning.
 *
 * All HTTP runs on this module's own PSRAM task. Callers only ever
 * touch the parsed model, the version counter and the toast, all
 * serialized by a mutex, so everything here is safe from any task.
 */

#define AKARI_PAGE_MAX_BLOCKS 24
#define AKARI_PAGE_TEXT_MAX 384   /* bytes, cut on a whole UTF-8 character */
#define AKARI_PAGE_ID_MAX 48
#define AKARI_PAGE_LABEL_MAX 96

/* Physical keys akari_page_key() binds: the talk button and the
 * speaker ("fn") button. The power button is never intercepted. */
enum {
    AKARI_PAGE_KEY_TALK = 0,
    AKARI_PAGE_KEY_FN = 1,
};

typedef enum {
    AKB_TITLE,
    AKB_TEXT,
    AKB_CAPTION,
    AKB_DIVIDER,
    AKB_SPACER,
    AKB_BUTTON,      /* one button, btn[0] */
    AKB_BUTTON_ROW,  /* up to two buttons side by side */
} akari_block_kind_t;

typedef struct {
    char id[AKARI_PAGE_ID_MAX];
    char label[AKARI_PAGE_LABEL_MAX];
    bool confirm;   /* first tap arms, second tap within 3 s fires */
    bool danger;    /* rendered in Akari red */
} akari_button_t;

typedef struct {
    akari_block_kind_t kind;
    char text[AKARI_PAGE_TEXT_MAX];   /* title / text / caption */
    int spacer_h;                     /* AKB_SPACER: height in px */
    akari_button_t btn[2];            /* AKB_BUTTON / AKB_BUTTON_ROW */
    int n_btn;
} akari_block_t;

/* The parsed page: one flat struct so the UI can take a copy. */
typedef struct {
    char title[AKARI_PAGE_TEXT_MAX];   /* optional header, may be empty */
    akari_block_t blocks[AKARI_PAGE_MAX_BLOCKS];
    int n_blocks;
} akari_page_model_t;

/* True when a page URL and token were provisioned at build time. */
bool akari_page_configured(void);

/* Bumped whenever the page, the bindings or the toast change; the UI
 * polls it while the page tile is visible. */
unsigned akari_page_version(void);

/*
 * A PSRAM copy of the current page, or NULL when none has loaded yet.
 * The caller frees it with free(). Pairs with akari_page_failed():
 * with no page loaded, true means the last fetch failed (show an error)
 * rather than still loading.
 */
akari_page_model_t *akari_page_get(void);
bool akari_page_failed(void);

/*
 * The page tile became visible or stopped being visible. Becoming
 * visible fetches the page at once; while visible it refetches every
 * two minutes. Never blocks: the fetch runs on the page task.
 */
void akari_page_set_active(bool active);

/*
 * Enqueues a button action for the page task: a POST of
 * {"action_id": id, "source": source} to the webhook. `source` is
 * "tap" for on-screen buttons; the key bindings pass their own.
 */
void akari_page_action(const char *id, const char *source);

/*
 * The webhook's current toast, while it is live (~3.5 s). Copies it
 * into `out` and returns true; false once it has expired.
 */
bool akari_page_toast(char *out, size_t cap);

/*
 * A physical key press (AKARI_PAGE_KEY_*) while the page tile is
 * visible: when the page binds this key, fires the bound action and
 * returns true, and the caller drops the press's usual meaning.
 * Never consumes a press while a chat turn is recording or a call is
 * up, and never without a binding from a loaded page.
 */
bool akari_page_key(int key);

/* Whether akari_page_key() would consume this key right now, without
 * firing anything (the input task asks on a waking press, which must
 * not start a voice turn it cannot see, nor fire blind). */
bool akari_page_key_bound(int key);

#ifdef __cplusplus
}
#endif
