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

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Akari history: the last few finished turns of the conversation, kept
 * in PSRAM for the UI's history page (see muse_ui.c). The chat session
 * records each user utterance once it is final and each assistant
 * message once it completes; the page renders them as one text so the
 * replies can be read back after they were spoken.
 *
 * Storage is a small ring: the newest AKARI_HISTORY_ENTRIES turns,
 * each truncated to a few hundred bytes on a whole UTF-8 character.
 * Everything here is safe to call from any task: adds come from the
 * chat task, reads from the LVGL task, and a mutex serializes them.
 */

/* Records one finished turn; empty text is ignored. */
void akari_history_add(bool is_user, const char *text);

/* Bumped on every add; the UI polls it to refresh a visible page. */
unsigned akari_history_version(void);

/*
 * The whole history as one display string, oldest first, each entry
 * headed by its speaker ("ねじわ様：" / "朱莉：") and separated by a
 * blank line. Returns a PSRAM allocation the caller frees with free(),
 * or NULL when allocation fails. An empty history renders as "".
 */
char *akari_history_render(void);

#ifdef __cplusplus
}
#endif
