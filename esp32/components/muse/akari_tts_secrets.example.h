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
 * TEMPLATE — copy this file to akari_tts_secrets.h in this directory and
 * fill in your own VOICEVOX endpoint there. akari_tts_secrets.h is
 * gitignored: never commit real values. Without the file (or with the
 * values left empty) the Akari voice stays off and replies are shown at
 * reading pace, as in stock firmware.
 *
 *   AKARI_TTS_URL    Base URL of the VOICEVOX endpoint, HTTPS, no path:
 *                    the firmware appends /audio_query and /streaming_synthesis.
 *   AKARI_TTS_TOKEN  Bearer token the endpoint expects, sent as the
 *                    Authorization header of every request.
 */

#pragma once

#define AKARI_TTS_URL ""
#define AKARI_TTS_TOKEN ""
