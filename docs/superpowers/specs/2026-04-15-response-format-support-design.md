# Design: `response_format` Support for `/v1/audio/transcriptions`

**Date:** 2026-04-15
**Status:** Approved

## Context

The `POST /v1/audio/transcriptions` endpoint was implemented but ignores the `response_format` field, always returning `{"text":"..."}` JSON. OpenClaw's `openai-whisper-api` skill defaults to `response_format=text` (not `json`), so every default invocation writes `{"text":"the transcript"}` to the output `.txt` file instead of the plain transcript. This is a compatibility blocker.

## What OpenClaw sends

From OpenClaw's `transcribe.sh`:

```bash
curl POST /v1/audio/transcriptions
  -H "Authorization: Bearer $KEY"
  -H "Accept: application/json"
  -F "file=@audio.ogg"
  -F "model=whisper-1"
  -F "response_format=text"    # DEFAULT — plain text expected back
  [-F "language=en"]
  [-F "prompt=..."]
  # --json flag sets response_format=json
```

## Supported formats

| `response_format` | Content-Type | Body |
|---|---|---|
| `text` | `text/plain; charset=utf-8` | Plain transcript, leading whitespace trimmed |
| `json` | `application/json` | `{"text":"..."}` |
| `verbose_json` | `application/json` | Full object with segments and timestamps (see below) |
| `srt`, `vtt`, or unknown | — | `400` with OpenAI-style error |

## Changes

**Single file modified:** `src/server/handlers/HandleTranscribe.cpp`

No changes to CMakeLists.txt, Dockerfile, headers, or other files.

## Request flow (updated)

```
1. Auth
2. Size limit
3. Parse multipart
4. Extract + validate response_format        ← NEW
   - default: "json"
   - 400 for srt / vtt / unknown values
5. Decode audio
6. Inference capacity check
7. Acquire model context
8. Whisper inference
   - Collect segments with timestamps         ← NEW (only when verbose_json)
   - Concatenate text (always)
9. Free state + release model
10. Format response by response_format        ← NEW
    - text         → text/plain, leading whitespace trimmed
    - json         → {"text":"..."}  (previous behavior)
    - verbose_json → {"task","language","duration","text","segments":[...]}
```

## `response_format=text` — trim behaviour

whisper.cpp emits each segment with a leading space (e.g. `" Hola mundo."`). The full
concatenated string therefore starts with a space. For `text` format only, we trim the
leading whitespace from the final concatenated string. Inter-segment spacing is preserved
because each segment after the first contributes its own leading space as a natural join.

```
Seg 0: " Hola mundo."
Seg 1: " Esto es una prueba."
Concat: " Hola mundo. Esto es una prueba."
Trimmed: "Hola mundo. Esto es una prueba."   ← only first char removed
```

## `response_format=verbose_json` — response shape

```json
{
  "task": "transcribe",
  "language": "spanish",
  "duration": 12.5,
  "text": " Hola mundo. Esto es una prueba.",
  "segments": [
    {
      "start": 0.0,
      "end": 3.2,
      "text": " Hola mundo.",
      "no_speech_prob": 0.03
    },
    {
      "start": 3.2,
      "end": 5.8,
      "text": " Esto es una prueba.",
      "no_speech_prob": 0.01
    }
  ]
}
```

Timestamps come from `whisper_full_get_segment_t0/t1_from_state()` (centiseconds → divided by 100.0).
`no_speech_prob` from `whisper_full_get_segment_no_speech_prob_from_state()`.
`duration` is `segments.back().end`; 0.0 if no segments.
`language` is the value passed in the request (or `"auto"` if not provided).

Segment collection is **conditional** — only when `response_format == "verbose_json"`.
For `text` and `json`, no extra whisper API calls are made.

## Segment struct (local to HandleTranscribe.cpp)

```cpp
struct Segment {
    float       start;
    float       end;
    std::string text;
    float       no_speech_prob;
};
```

Declared inside the `handle()` function scope. Not exposed in the header.

## Error response for unsupported formats

```json
{
  "error": {
    "message": "response_format 'srt' is not supported. Supported values: json, text, verbose_json",
    "type": "invalid_request_error"
  }
}
```

HTTP status: `400`.

## What is NOT in scope

- `srt` / `vtt` subtitle formats
- `timestamp_granularities[]` (word-level timestamps)
- Token-level data in `verbose_json` segments (`tokens`, `avg_logprob`, `compression_ratio`, `temperature`)
- Any changes outside `HandleTranscribe.cpp`
