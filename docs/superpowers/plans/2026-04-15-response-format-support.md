# response_format Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `response_format` support to `POST /v1/audio/transcriptions` so that OpenClaw's default `text` format returns plain text, `json` keeps working, `verbose_json` returns segments with timestamps, and unsupported values (`srt`, `vtt`, unknown) return 400.

**Architecture:** Single handler file change. Parse `response_format` from the multipart body early in the handler, validate against an allowlist, collect whisper segments conditionally (only for `verbose_json`), then branch on format when building the HTTP response.

**Tech Stack:** C++17, Boost.Beast HTTP, whisper.cpp C API, nlohmann/json, GoogleTest.

**Spec:** `docs/superpowers/specs/2026-04-15-response-format-support-design.md`

---

## File Map

| Action | Path | Responsibility |
|---|---|---|
| Modify | `tests/unit/test_handle_transcribe.cpp` | Add tests for all response_format values |
| Modify | `src/server/handlers/HandleTranscribe.cpp` | Parse/validate response_format; format response accordingly |

---

## Task 1: Tests for `response_format` validation and routing

**Files:**
- Modify: `tests/unit/test_handle_transcribe.cpp`

### Background

The existing `makeMultipartBody` helper doesn't support `response_format`. We'll add a new helper `makeMultipartBodyWithFormat` that accepts it as a parameter. Tests that require a real model run are guarded by `GTEST_SKIP()` via the existing `SetUp()`.

- [ ] **Step 1: Add `makeMultipartBodyWithFormat` helper after the existing `makeMultipartBody` function**

Open `tests/unit/test_handle_transcribe.cpp` and add this function after the closing `}` of `makeMultipartBody` (around line 53):

```cpp
static std::string makeMultipartBodyWithFormat(const std::string& boundary,
                                                const std::vector<uint8_t>& audio,
                                                const std::string& response_format,
                                                const std::string& language = "") {
    std::string body = "--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n";
    body.append(reinterpret_cast<const char*>(audio.data()), audio.size());
    body += "\r\n--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"model\"\r\n\r\nwhisper-1\r\n";
    body += "--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"response_format\"\r\n\r\n" +
        response_format + "\r\n";
    if (!language.empty()) {
        body += "--" + boundary + "\r\n"
            "Content-Disposition: form-data; name=\"language\"\r\n\r\n" +
            language + "\r\n";
    }
    body += "--" + boundary + "--\r\n";
    return body;
}
```

- [ ] **Step 2: Add validation tests (no model needed)**

Add these four tests inside the file, after the existing `ReturnsBadRequestForGarbageAudio` test. These tests do **not** need the model — they fail before reaching whisper — but they are inside `HandleTranscribeTest` so they still get `config` and `no_auth` from `SetUp`. Since `SetUp` calls `GTEST_SKIP()` when the model is missing, add a note: these tests validate early-exit paths, so they will be skipped if the model file is absent. If you want to run them without a model, extract them into a separate fixture that doesn't skip. For now, leaving them here is fine since they exercise the same handler.

```cpp
TEST_F(HandleTranscribeTest, ReturnsBadRequestForUnsupportedFormatSrt) {
    auto audio = makeSilentWav(0.5f);
    auto body  = makeMultipartBodyWithFormat("bnd", audio, "srt");

    http::request<http::string_body> req{http::verb::post, "/v1/audio/transcriptions", 11};
    req.set(http::field::content_type, "multipart/form-data; boundary=bnd");
    req.body() = body;
    req.prepare_payload();

    http::response<http::string_body> res;
    HandleTranscribe::handle(req, [&](http::response<http::string_body> r) { res = std::move(r); },
                             config, no_auth);

    EXPECT_EQ(res.result(), http::status::bad_request);
    EXPECT_NE(res.body().find("srt"), std::string::npos);
    EXPECT_NE(res.body().find("not supported"), std::string::npos);
}

TEST_F(HandleTranscribeTest, ReturnsBadRequestForUnsupportedFormatVtt) {
    auto audio = makeSilentWav(0.5f);
    auto body  = makeMultipartBodyWithFormat("bnd", audio, "vtt");

    http::request<http::string_body> req{http::verb::post, "/v1/audio/transcriptions", 11};
    req.set(http::field::content_type, "multipart/form-data; boundary=bnd");
    req.body() = body;
    req.prepare_payload();

    http::response<http::string_body> res;
    HandleTranscribe::handle(req, [&](http::response<http::string_body> r) { res = std::move(r); },
                             config, no_auth);

    EXPECT_EQ(res.result(), http::status::bad_request);
    EXPECT_NE(res.body().find("vtt"), std::string::npos);
}

TEST_F(HandleTranscribeTest, ReturnsBadRequestForUnknownFormat) {
    auto audio = makeSilentWav(0.5f);
    auto body  = makeMultipartBodyWithFormat("bnd", audio, "banana");

    http::request<http::string_body> req{http::verb::post, "/v1/audio/transcriptions", 11};
    req.set(http::field::content_type, "multipart/form-data; boundary=bnd");
    req.body() = body;
    req.prepare_payload();

    http::response<http::string_body> res;
    HandleTranscribe::handle(req, [&](http::response<http::string_body> r) { res = std::move(r); },
                             config, no_auth);

    EXPECT_EQ(res.result(), http::status::bad_request);
    EXPECT_NE(res.body().find("banana"), std::string::npos);
}
```

- [ ] **Step 3: Add inference tests (`text`, `verbose_json`, default-is-json)**

These tests run whisper and require the model. Add after the previous tests:

```cpp
TEST_F(HandleTranscribeTest, ResponseFormatTextReturnsPlainText) {
    auto audio = makeSilentWav(0.5f);
    auto body  = makeMultipartBodyWithFormat("bnd", audio, "text", "en");

    http::request<http::string_body> req{http::verb::post, "/v1/audio/transcriptions", 11};
    req.set(http::field::content_type, "multipart/form-data; boundary=bnd");
    req.body() = body;
    req.prepare_payload();

    http::response<http::string_body> res;
    HandleTranscribe::handle(req, [&](http::response<http::string_body> r) { res = std::move(r); },
                             config, no_auth);

    EXPECT_EQ(res.result(), http::status::ok);
    // Content-Type must be text/plain (not application/json)
    EXPECT_NE(std::string(res[http::field::content_type]).find("text/plain"), std::string::npos);
    // Body must not start with '{' (not JSON)
    const std::string& b = res.body();
    EXPECT_TRUE(b.empty() || b.front() != '{')
        << "Expected plain text, got: " << b;
    // No leading whitespace
    EXPECT_TRUE(b.empty() || b.front() != ' ')
        << "Expected trimmed text, got leading space";
}

TEST_F(HandleTranscribeTest, ResponseFormatVerboseJsonHasSegmentsKey) {
    auto audio = makeSilentWav(0.5f);
    auto body  = makeMultipartBodyWithFormat("bnd", audio, "verbose_json", "en");

    http::request<http::string_body> req{http::verb::post, "/v1/audio/transcriptions", 11};
    req.set(http::field::content_type, "multipart/form-data; boundary=bnd");
    req.body() = body;
    req.prepare_payload();

    http::response<http::string_body> res;
    HandleTranscribe::handle(req, [&](http::response<http::string_body> r) { res = std::move(r); },
                             config, no_auth);

    EXPECT_EQ(res.result(), http::status::ok);
    EXPECT_NE(std::string(res[http::field::content_type]).find("application/json"), std::string::npos);
    EXPECT_NE(res.body().find("\"segments\""), std::string::npos);
    EXPECT_NE(res.body().find("\"task\""), std::string::npos);
    EXPECT_NE(res.body().find("\"duration\""), std::string::npos);
    EXPECT_NE(res.body().find("\"text\""), std::string::npos);
}

TEST_F(HandleTranscribeTest, NoResponseFormatFieldDefaultsToJson) {
    // makeMultipartBody (the original helper) sends no response_format field
    auto audio = makeSilentWav(0.5f);
    auto body  = makeMultipartBody("bnd", audio, "en");

    http::request<http::string_body> req{http::verb::post, "/v1/audio/transcriptions", 11};
    req.set(http::field::content_type, "multipart/form-data; boundary=bnd");
    req.body() = body;
    req.prepare_payload();

    http::response<http::string_body> res;
    HandleTranscribe::handle(req, [&](http::response<http::string_body> r) { res = std::move(r); },
                             config, no_auth);

    EXPECT_EQ(res.result(), http::status::ok);
    EXPECT_NE(std::string(res[http::field::content_type]).find("application/json"), std::string::npos);
    EXPECT_NE(res.body().find("\"text\""), std::string::npos);
}
```

- [ ] **Step 4: Build tests and confirm they fail**

```bash
cmake -B build -DBUILD_TESTS=ON -DBUILD_SERVER=OFF -DBUILD_SHARED_LIBS=OFF
cmake --build build --target unit_tests -j$(nproc) 2>&1 | tail -5
```

Expected: compiles successfully (the new tests call `HandleTranscribe::handle` which exists). Then run:

```bash
./build/tests/unit_tests --gtest_filter="HandleTranscribeTest.ResponseFormatText*:HandleTranscribeTest.ResponseFormatVerbose*:HandleTranscribeTest.NoResponseFormat*:HandleTranscribeTest.ReturnsBadRequestForUnsupported*:HandleTranscribeTest.ReturnsBadRequestForUnknown*" 2>&1 | tail -20
```

Expected: validation tests (`Srt`, `Vtt`, `Unknown`) FAIL because the current handler doesn't validate `response_format`. Inference tests (`Text`, `VerboseJson`, `DefaultsToJson`) may PASS for `DefaultsToJson` (existing behavior) and FAIL for `Text` (returns JSON instead of text/plain) and `VerboseJson` (missing `segments` key).

---

## Task 2: Implement `response_format` in `HandleTranscribe.cpp`

**Files:**
- Modify: `src/server/handlers/HandleTranscribe.cpp`

- [ ] **Step 1: Add `#include <unordered_set>` at the top**

In `src/server/handlers/HandleTranscribe.cpp`, add after the existing includes:

```cpp
#include <unordered_set>
```

- [ ] **Step 2: Add response_format parsing and validation after the multipart parse block**

The multipart parse block ends around line 87 (`if (!parts.count("model")) { ... }`). Add this block immediately after it (before the `// ── 4. Decode audio` comment):

```cpp
    // ── 3b. response_format ───────────────────────────────────────────────────
    std::string response_format = "json";
    if (parts.count("response_format")) {
        response_format.assign(parts.at("response_format").data.begin(),
                               parts.at("response_format").data.end());
    }
    static const std::unordered_set<std::string> kSupportedFormats{"json", "text", "verbose_json"};
    if (!kSupportedFormats.count(response_format)) {
        send(makeError(http::status::bad_request,
                       "response_format '" + response_format + "' is not supported. "
                       "Supported values: json, text, verbose_json",
                       "invalid_request_error", ver));
        return;
    }
```

- [ ] **Step 3: Move `language` out of the guard block and add `Segment` + `segments` before it**

`language` is currently declared at line 138 **inside** the `{ InferenceLimiter::Guard ... }` block, but Step 4 references it after the block closes. `segments` has the same scope problem. Both must be declared before the guard block.

Find this line (line 125 in the current file):

```cpp
    // ── 7. Transcribe ─────────────────────────────────────────────────────────
    std::string text;
    {
        InferenceLimiter::Guard guard;
```

Replace it with:

```cpp
    // ── 7. Transcribe ─────────────────────────────────────────────────────────
    // Declared before guard so they're accessible when formatting the response below.
    struct Segment {
        float       start;
        float       end;
        std::string text;
        float       no_speech_prob;
    };
    std::vector<Segment> segments;

    std::string language = "auto";
    if (parts.count("language")) {
        language.assign(parts.at("language").data.begin(),
                        parts.at("language").data.end());
    }

    std::string text;
    {
        InferenceLimiter::Guard guard;
```

Then, inside the guard block, **delete** the existing `language` block (lines 137–142):

```cpp
        // Determine language
        std::string language = "auto";
        if (parts.count("language")) {
            language.assign(parts.at("language").data.begin(),
                            parts.at("language").data.end());
        }
```

Finally, replace the existing segment-collection loop inside the guard block:

```cpp
        int n = whisper_full_n_segments_from_state(state);
        for (int i = 0; i < n; ++i) {
            const char* seg = whisper_full_get_segment_text_from_state(state, i);
            if (seg) text += seg;
        }
```

with:

```cpp
        int n = whisper_full_n_segments_from_state(state);
        for (int i = 0; i < n; ++i) {
            const char* seg = whisper_full_get_segment_text_from_state(state, i);
            if (!seg) continue;
            if (response_format == "verbose_json") {
                int64_t t0  = whisper_full_get_segment_t0_from_state(state, i);
                int64_t t1  = whisper_full_get_segment_t1_from_state(state, i);
                float   nsp = whisper_full_get_segment_no_speech_prob_from_state(state, i);
                segments.push_back({t0 / 100.0f, t1 / 100.0f, seg, nsp});
            }
            text += seg;
        }
```

After these edits `text`, `language`, `segments`, and `Segment` are all declared in the outer function scope and accessible in Step 4.

- [ ] **Step 4: Replace the final response with format-aware branching**

Find and replace the last two lines of the function (after `ModelCache::instance().release()`):

```cpp
    json response = {{"text", text}};
    send(makeJson(http::status::ok, response.dump(), ver));
```

Replace with:

```cpp
    Log::info("HandleTranscribe: transcribed " + std::to_string(pcm.size()) +
              " samples → " + std::to_string(text.size()) + " chars"
              + " format=" + response_format);

    if (response_format == "text") {
        auto first = text.find_first_not_of(" \t\r\n");
        std::string trimmed = (first == std::string::npos) ? "" : text.substr(first);
        http::response<http::string_body> res;
        res.version(ver);
        res.result(http::status::ok);
        res.set(http::field::server, BOOST_BEAST_VERSION_STRING);
        res.set(http::field::content_type, "text/plain; charset=utf-8");
        res.body() = trimmed;
        res.prepare_payload();
        send(res);
        return;
    }

    if (response_format == "json") {
        send(makeJson(http::status::ok, json{{"text", text}}.dump(), ver));
        return;
    }

    // verbose_json
    json segs = json::array();
    for (const auto& s : segments) {
        segs.push_back({
            {"start",          s.start},
            {"end",            s.end},
            {"text",           s.text},
            {"no_speech_prob", s.no_speech_prob}
        });
    }
    json vj = {
        {"task",     "transcribe"},
        {"language", language},
        {"duration", segments.empty() ? 0.0f : segments.back().end},
        {"text",     text},
        {"segments", segs}
    };
    send(makeJson(http::status::ok, vj.dump(), ver));
```

- [ ] **Step 5: Also remove the now-redundant Log::info line that came before the old response**

The original file has this line before the old `json response = ...`:

```cpp
    Log::info("HandleTranscribe: transcribed " + std::to_string(pcm.size()) +
              " samples → " + std::to_string(text.size()) + " chars");
```

This should now be removed since Step 4's replacement already includes an updated log line with `format=` appended.

- [ ] **Step 6: Build server and confirm it compiles**

```bash
cmake -B build -DBUILD_TESTS=OFF -DBUILD_SERVER=ON -DBUILD_SHARED_LIBS=OFF
cmake --build build --target jota-transcriber -j$(nproc) 2>&1 | tail -10
```

Expected: clean build, zero errors, zero warnings about unused variables.

- [ ] **Step 7: Build and run tests**

```bash
cmake -B build -DBUILD_TESTS=ON -DBUILD_SERVER=OFF -DBUILD_SHARED_LIBS=OFF
cmake --build build --target unit_tests -j$(nproc) 2>&1 | tail -5
./build/tests/unit_tests --gtest_filter="HandleTranscribeTest.*" 2>&1
```

Expected output (model present):
```
[==========] Running N tests from 1 test suite.
[  PASSED  ] N tests.
[  FAILED  ] 0 tests.
```

All `HandleTranscribeTest` tests must pass:
- `ReturnsBadRequestWhenMissingFile` — existing, must still pass
- `TranscribesSilenceAndReturnsJson` — existing, must still pass
- `ReturnsBadRequestForGarbageAudio` — existing, must still pass
- `ReturnsBadRequestForUnsupportedFormatSrt` — new, must pass
- `ReturnsBadRequestForUnsupportedFormatVtt` — new, must pass
- `ReturnsBadRequestForUnknownFormat` — new, must pass
- `ResponseFormatTextReturnsPlainText` — new, must pass
- `ResponseFormatVerboseJsonHasSegmentsKey` — new, must pass
- `NoResponseFormatFieldDefaultsToJson` — new, must pass

- [ ] **Step 8: Commit**

```bash
git add src/server/handlers/HandleTranscribe.cpp \
        tests/unit/test_handle_transcribe.cpp
git commit -m "feat: support response_format=text|json|verbose_json in /v1/audio/transcriptions

- text: returns text/plain with leading whitespace trimmed
- json: returns {\"text\":\"...\"} (previous behaviour, now explicit)
- verbose_json: returns task/language/duration/text/segments[]
- srt, vtt, unknown: returns 400 invalid_request_error

Fixes default OpenClaw compatibility (openclaw/openclaw openai-whisper-api
skill defaults to response_format=text, not json)

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"
```
