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
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Akari voice: speaks the chat session's text replies with an external
 * VOICEVOX TTS endpoint (see docs/akari-voicevox-tts.md).
 *
 * The endpoint URL and its Bearer token are NOT part of this repository:
 * they come from akari_tts_secrets.h, a gitignored header generated from
 * akari_tts_secrets.example.h on the build machine. Without it the feature
 * compiles but stays inert (akari_tts_configured() is false) and replies
 * fall back to being shown at reading pace, exactly as stock firmware.
 *
 * Flow: akari_tts_start() hands a message's text to a dedicated task,
 * which splits it into sentences and, per sentence, runs the VOICEVOX
 * /audio_query + /streaming_synthesis pair over HTTPS: the query is
 * still computed for the complete sentence (so intonation stays
 * natural), but the audio streams back and is fed to the session as
 * it arrives, so speech starts with the first chunk instead of after
 * the whole sentence has synthesized. A short prebuffer (0.75 s of
 * speech, released early when synthesis ends) cushions playback
 * against the engine's near-real-time generation rate. Synthesized
 * speech is 16 kHz mono PCM16; the chat session drains it with
 * akari_tts_read() into its reply stream, so captions stay paced to
 * the speech. Any failure is reported through akari_tts_failed() and
 * the session falls back to reading pace for the rest of the message:
 * a TTS outage never breaks a turn.
 */

/* True when an endpoint URL and token were provisioned at build time. */
bool akari_tts_configured(void);

/*
 * Queue `text` (UTF-8, NUL-terminated) for synthesis, superseding any
 * speech still being fetched. False when unconfigured, when the text is
 * empty, or when the hand-off could not be made (the caller then falls
 * back to showing the message). Returns before the first audio exists;
 * poll akari_tts_read().
 */
bool akari_tts_start(const char *text);

/* Copies up to `max_frames` of synthesized 16 kHz mono PCM; never blocks. */
size_t akari_tts_read(int16_t *out, size_t max_frames);

/* Frames of synthesized speech buffered and not yet read. */
size_t akari_tts_available(void);

/* No more speech is coming for the current text (finished or failed). */
bool akari_tts_ended(void);

/* The current text's synthesis failed; buffered speech, if any, is short. */
bool akari_tts_failed(void);

/* Drops the current text and any speech buffered for it. */
void akari_tts_abort(void);

#ifdef __cplusplus
}
#endif
