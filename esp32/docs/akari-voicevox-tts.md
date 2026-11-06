# Akari voice: VOICEVOX replies on the StopWatch

A fork-local feature (build option `MUSE_AKARI_TTS`, enabled by the
M5Stack StopWatch board profile) that speaks the Muse chat session's
text replies aloud with an external VOICEVOX TTS endpoint, instead of
only showing them at reading pace.

## How it works

Stock firmware paces a finished reply message with silence in
`start_tts()` in `muse_chat_session.cpp`; the comment there describes
plugging in a TTS API of your own. This feature takes that hook:

1. When a message finishes, if the device's **Speaker** setting is on
   and a TTS endpoint is provisioned (below), the session hands the
   message's text to `akari_tts_start()` and marks the message as
   PCM-fed instead of silence-paced.
2. A dedicated task (`akari_tts.c`) splits the text into sentences
   (。！？, newlines, ASCII sentence ends; long runs are cut near a
   punctuation mark) and, per sentence, calls the endpoint:
   `POST /audio_query?text=…&speaker=17`, tunes the returned query
   (`speedScale` 1.15, `outputSamplingRate` 16000, mono), then
   `POST /streaming_synthesis?speaker=17`. Style id 17 is the chosen
   voice. The query is always computed for the complete sentence, so
   intonation stays natural; only the audio delivery is streamed.
3. The response is one WAV (44-byte header, then raw PCM) delivered
   in bursts of ~0.25 s of audio as the engine generates it. The
   firmware parses the header incrementally and converts the PCM to
   16 kHz mono PCM16 as it arrives, feeding the session's reply
   buffer, so speech starts with the first chunk instead of after
   the whole sentence — and the existing playback and caption pacing
   work unchanged: captions follow the speech. Because the engine
   generates streaming audio at only ~1.05x real time on the host
   CPU, the session's drain of each message holds back the first
   0.75 s of speech as a cushion against jitter (released early
   when synthesis ends, e.g. for very short replies).
4. Any failure (no network, HTTP error, malformed audio) flips the
   message back to silence pacing for its remainder. A TTS outage can
   delay speech; it can never break a turn.

Synthesis runs on its own task, so the session's connection loop never
blocks on the TTS endpoint.

## The on/off switch

The gadget's existing **Speaker** setting is the switch — the same one
in the watch's Settings → Sound page, on the watch face's speaker
button, and in the device settings exposed over BLE. It is persisted
in NVS (`muse_settings`, key `speaker`) and defaults to on.

- **On**: replies are spoken (when an endpoint is provisioned).
- **Off**: no TTS request is made at all; replies are shown at reading
  pace, exactly as stock firmware.

## Provisioning the endpoint (secrets)

The endpoint URL and its Bearer token are **never committed**. They
reach the firmware through a gitignored header:

1. Copy `components/muse/akari_tts_secrets.example.h` to
   `components/muse/akari_tts_secrets.h` (same directory).
2. Fill in `AKARI_TTS_URL` (base URL; `/audio_query` and
   `/streaming_synthesis` are appended) and `AKARI_TTS_TOKEN`.
3. Rebuild and flash as usual (`tools/muse/board.sh build stopwatch`).

Without the header, or with empty values, `akari_tts_configured()` is
false and the firmware behaves exactly like stock: the code is present
but inert. Neither value is written to the logs — request failures log
only the HTTP status.

## Limits

- One message is spoken at a time, in order; a new press cancels the
  fetch along with the turn.
- Replies are capped by the session's own text buffer; very long
  replies speak what the session kept.
- Light cleanup only: `*` and `` ` `` characters are dropped before
  synthesis. Line structure is kept (newlines end sentences).
