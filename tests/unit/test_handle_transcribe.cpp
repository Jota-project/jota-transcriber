#include <gtest/gtest.h>
#include "server/handlers/HandleTranscribe.h"
#include "server/ServerConfig.h"
#include "server/AuthManager.h"
#include "auth/ApiAuthConfig.h"
#include "whisper/ModelCache.h"
#include "whisper/InferenceLimiter.h"
#include <filesystem>
#include <cstring>
#include <cstdint>

#ifndef PROJECT_ROOT
#define PROJECT_ROOT "."
#endif

const std::string TRANSCRIBE_MODEL =
    std::string(PROJECT_ROOT) + "/third_party/whisper.cpp/models/ggml-small.bin";

namespace http = boost::beast::http;

// Build a 0.5s silent WAV in memory.
static std::vector<uint8_t> makeSilentWav(float dur = 0.5f, int sr = 16000) {
    int n = static_cast<int>(dur * sr);
    uint32_t data_bytes = static_cast<uint32_t>(n * 2);
    struct __attribute__((packed)) Hdr {
        char     riff[4]={'R','I','F','F'}; uint32_t chunk_sz;
        char     wave[4]={'W','A','V','E'}; char fmt[4]={'f','m','t',' '};
        uint32_t sub1=16; uint16_t fmt_=1,ch=1; uint32_t sr_,br_; uint16_t ba=2,bps=16;
        char     data[4]={'d','a','t','a'}; uint32_t sub2;
    };
    Hdr h; h.chunk_sz=36+data_bytes; h.sr_=sr; h.br_=sr*2; h.sub2=data_bytes;
    std::vector<uint8_t> buf(sizeof(Hdr)+data_bytes, 0);
    memcpy(buf.data(), &h, sizeof(Hdr));
    return buf;
}

// Build a multipart/form-data body with a WAV file + model field.
static std::string makeMultipartBody(const std::string& boundary,
                                     const std::vector<uint8_t>& audio,
                                     const std::string& language = "") {
    std::string body = "--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n";
    body.append(reinterpret_cast<const char*>(audio.data()), audio.size());
    body += "\r\n--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"model\"\r\n\r\nwhisper-1\r\n";
    if (!language.empty()) {
        body += "--" + boundary + "\r\n"
            "Content-Disposition: form-data; name=\"language\"\r\n\r\n" +
            language + "\r\n";
    }
    body += "--" + boundary + "--\r\n";
    return body;
}

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

class HandleTranscribeTest : public ::testing::Test {
protected:
    ServerConfig config;
    std::shared_ptr<AuthManager> no_auth;

    void SetUp() override {
        if (!std::filesystem::exists(TRANSCRIBE_MODEL))
            GTEST_SKIP() << "Model not found: " << TRANSCRIBE_MODEL;
        config.model_path      = TRANSCRIBE_MODEL;
        config.whisper_threads = 2;
        config.whisper_beam_size = 1;
        ModelCache::instance().configure(-1); // keep forever during tests
        ApiAuthConfig auth_cfg;               // no auth token → auth disabled
        no_auth = std::make_shared<AuthManager>(auth_cfg);
    }

    void TearDown() override {
        ModelCache::instance().forceUnload();
    }
};

TEST_F(HandleTranscribeTest, ReturnsBadRequestWhenMissingFile) {
    std::string boundary = "bound";
    std::string body = "--bound\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\nwhisper-1\r\n--bound--\r\n";

    http::request<http::string_body> req{http::verb::post, "/v1/audio/transcriptions", 11};
    req.set(http::field::content_type, "multipart/form-data; boundary=bound");
    req.body() = body;
    req.prepare_payload();

    http::response<http::string_body> captured_res;
    HandleTranscribe::handle(req, [&](http::response<http::string_body> r) {
        captured_res = std::move(r);
    }, config, no_auth);

    EXPECT_EQ(captured_res.result(), http::status::bad_request);
    EXPECT_NE(captured_res.body().find("file"), std::string::npos);
}

TEST_F(HandleTranscribeTest, TranscribesSilenceAndReturnsJson) {
    auto audio   = makeSilentWav(0.5f);
    auto body    = makeMultipartBody("boundary123", audio, "en");

    http::request<http::string_body> req{http::verb::post, "/v1/audio/transcriptions", 11};
    req.set(http::field::content_type, "multipart/form-data; boundary=boundary123");
    req.body() = body;
    req.prepare_payload();

    http::response<http::string_body> captured_res;
    HandleTranscribe::handle(req, [&](http::response<http::string_body> r) {
        captured_res = std::move(r);
    }, config, no_auth);

    EXPECT_EQ(captured_res.result(), http::status::ok);
    EXPECT_NE(captured_res.body().find("\"text\""), std::string::npos);
}

TEST_F(HandleTranscribeTest, ReturnsBadRequestForGarbageAudio) {
    std::vector<uint8_t> garbage = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01};
    std::string boundary = "b";
    std::string body = "--b\r\nContent-Disposition: form-data; name=\"file\"; filename=\"x.bin\"\r\nContent-Type: application/octet-stream\r\n\r\n";
    body.append(reinterpret_cast<const char*>(garbage.data()), garbage.size());
    body += "\r\n--b\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\nwhisper-1\r\n--b--\r\n";

    http::request<http::string_body> req{http::verb::post, "/v1/audio/transcriptions", 11};
    req.set(http::field::content_type, "multipart/form-data; boundary=b");
    req.body() = body;
    req.prepare_payload();

    http::response<http::string_body> captured_res;
    HandleTranscribe::handle(req, [&](http::response<http::string_body> r) {
        captured_res = std::move(r);
    }, config, no_auth);

    EXPECT_EQ(captured_res.result(), http::status::bad_request);
}

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
    // No trailing whitespace
    EXPECT_TRUE(b.empty() || b.back() != ' ')
        << "Expected no trailing space, got: '" << b << "'";
    EXPECT_TRUE(b.empty() || (b.back() != '\n' && b.back() != '\r'))
        << "Expected no trailing newline, got: '" << b << "'";
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

static std::string makeMultipartBodyWithExtraField(const std::string& boundary,
                                                const std::vector<uint8_t>& audio,
                                                const std::string& field_name,
                                                const std::string& field_value) {
    std::string body = "--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n";
    body.append(reinterpret_cast<const char*>(audio.data()), audio.size());
    body += "\r\n--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"model\"\r\n\r\nwhisper-1\r\n";
    body += "--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"" + field_name + "\"\r\n\r\n" +
        field_value + "\r\n";
    body += "--" + boundary + "--\r\n";
    return body;
}

TEST_F(HandleTranscribeTest, TemperatureOutOfRangeUpperReturnsBadRequest) {
    auto body = makeMultipartBodyWithExtraField("bnd", makeSilentWav(0.5f), "temperature", "1.5");

    http::request<http::string_body> req{http::verb::post, "/v1/audio/transcriptions", 11};
    req.set(http::field::content_type, "multipart/form-data; boundary=bnd");
    req.body() = body;
    req.prepare_payload();

    http::response<http::string_body> res;
    HandleTranscribe::handle(req, [&](http::response<http::string_body> r) { res = std::move(r); },
                             config, no_auth);

    EXPECT_EQ(res.result(), http::status::bad_request);
    EXPECT_NE(res.body().find("temperature"), std::string::npos);
}

TEST_F(HandleTranscribeTest, TemperatureOutOfRangeLowerReturnsBadRequest) {
    auto body = makeMultipartBodyWithExtraField("bnd", makeSilentWav(0.5f), "temperature", "-0.1");

    http::request<http::string_body> req{http::verb::post, "/v1/audio/transcriptions", 11};
    req.set(http::field::content_type, "multipart/form-data; boundary=bnd");
    req.body() = body;
    req.prepare_payload();

    http::response<http::string_body> res;
    HandleTranscribe::handle(req, [&](http::response<http::string_body> r) { res = std::move(r); },
                             config, no_auth);

    EXPECT_EQ(res.result(), http::status::bad_request);
}

TEST_F(HandleTranscribeTest, TaskTranslateSetsTranslateFlag) {
    auto audio = makeSilentWav(0.5f);
    std::string body;
    body += "--bnd\r\n"
            "Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n"
            "Content-Type: audio/wav\r\n\r\n";
    body.append(reinterpret_cast<const char*>(audio.data()), audio.size());
    body += "\r\n--bnd\r\n"
           "Content-Disposition: form-data; name=\"model\"\r\n\r\nwhisper-1\r\n"
           "--bnd\r\n"
           "Content-Disposition: form-data; name=\"response_format\"\r\n\r\nverbose_json\r\n"
           "--bnd\r\n"
           "Content-Disposition: form-data; name=\"task\"\r\n\r\ntranslate\r\n"
           "--bnd--\r\n";


    http::request<http::string_body> req{http::verb::post, "/v1/audio/transcriptions", 11};
    req.set(http::field::content_type, "multipart/form-data; boundary=bnd");
    req.body() = body;
    req.prepare_payload();

    http::response<http::string_body> res;
    HandleTranscribe::handle(req, [&](http::response<http::string_body> r) { res = std::move(r); },
                             config, no_auth);

    EXPECT_EQ(res.result(), http::status::ok);
    EXPECT_NE(res.body().find("\"task\":\"translate\""), std::string::npos);
}

TEST_F(HandleTranscribeTest, VerboseJsonResponseIncludesIdAndCreatedAt) {
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
    EXPECT_NE(res.body().find("\"id\""), std::string::npos);
    EXPECT_NE(res.body().find("\"created_at\""), std::string::npos);
}

TEST_F(HandleTranscribeTest, Returns413WhenBodyExceedsMaxUploadBytes) {
    // Set a tiny limit — any real multipart body with a WAV will exceed it.
    config.max_upload_bytes = 100;

    auto audio = makeSilentWav(0.5f);
    auto body  = makeMultipartBody("bnd", audio, "en");

    http::request<http::string_body> req{http::verb::post, "/v1/audio/transcriptions", 11};
    req.set(http::field::content_type, "multipart/form-data; boundary=bnd");
    req.body() = body;
    req.prepare_payload();

    http::response<http::string_body> res;
    HandleTranscribe::handle(req, [&](http::response<http::string_body> r) { res = std::move(r); },
                             config, no_auth);

    EXPECT_EQ(res.result(), http::status::payload_too_large);
    EXPECT_NE(res.body().find("payload_too_large_error"), std::string::npos);
}

TEST_F(HandleTranscribeTest, Returns401WhenAuthEnabledAndTokenMissing) {
    ApiAuthConfig auth_cfg;
    auth_cfg.static_token = "secret-token";
    auto with_auth = std::make_shared<AuthManager>(auth_cfg);

    auto audio = makeSilentWav(0.5f);
    auto body  = makeMultipartBody("bnd", audio);

    http::request<http::string_body> req{http::verb::post, "/v1/audio/transcriptions", 11};
    req.set(http::field::content_type, "multipart/form-data; boundary=bnd");
    // No Authorization header → token will be empty → validation fails.
    req.body() = body;
    req.prepare_payload();

    http::response<http::string_body> res;
    HandleTranscribe::handle(req, [&](http::response<http::string_body> r) { res = std::move(r); },
                             config, with_auth);

    EXPECT_EQ(res.result(), http::status::unauthorized);
    EXPECT_NE(res.body().find("\"error\""), std::string::npos);
}

TEST_F(HandleTranscribeTest, Returns503WhenInferenceLimiterFull) {
    InferenceLimiter::instance().setMaxConcurrency(1);
    InferenceLimiter::instance().acquire(); // fill the one available slot

    auto audio = makeSilentWav(0.5f);
    auto body  = makeMultipartBody("bnd", audio);

    http::request<http::string_body> req{http::verb::post, "/v1/audio/transcriptions", 11};
    req.set(http::field::content_type, "multipart/form-data; boundary=bnd");
    req.body() = body;
    req.prepare_payload();

    http::response<http::string_body> res;
    HandleTranscribe::handle(req, [&](http::response<http::string_body> r) { res = std::move(r); },
                             config, no_auth);

    // Restore before assertions so teardown is clean.
    InferenceLimiter::instance().release();
    InferenceLimiter::instance().setMaxConcurrency(100); // safe high value, avoids coupling to private default

    EXPECT_EQ(res.result(), http::status::service_unavailable);
}

TEST_F(HandleTranscribeTest, TemperatureValidInRangeReturns200) {
    auto body = makeMultipartBodyWithExtraField("bnd", makeSilentWav(0.5f), "temperature", "0.5");

    http::request<http::string_body> req{http::verb::post, "/v1/audio/transcriptions", 11};
    req.set(http::field::content_type, "multipart/form-data; boundary=bnd");
    req.body() = body;
    req.prepare_payload();

    http::response<http::string_body> res;
    HandleTranscribe::handle(req, [&](http::response<http::string_body> r) { res = std::move(r); },
                             config, no_auth);

    EXPECT_EQ(res.result(), http::status::ok);
    EXPECT_NE(res.body().find("\"text\""), std::string::npos);
}

TEST_F(HandleTranscribeTest, Returns500WhenAuthManagerIsNullButAuthConfigured) {
    config.auth_token = "expected-token";  // auth debería estar activa

    auto audio = makeSilentWav(0.5f);
    auto body  = makeMultipartBody("bnd", audio);

    http::request<http::string_body> req{http::verb::post, "/v1/audio/transcriptions", 11};
    req.set(http::field::content_type, "multipart/form-data; boundary=bnd");
    req.body() = body;
    req.prepare_payload();

    http::response<http::string_body> res;
    HandleTranscribe::handle(req,
        [&](http::response<http::string_body> r) { res = std::move(r); },
        config,
        nullptr);  // null auth_manager pese a tener config de auth

    EXPECT_EQ(res.result(), http::status::internal_server_error);
}

TEST_F(HandleTranscribeTest, NullAuthManagerWithNoAuthConfigIsAccepted) {
    // config.auth_token está vacío por defecto → auth desactivada → null es válido
    auto audio = makeSilentWav(0.5f);
    auto body  = makeMultipartBody("bnd", audio, "en");

    http::request<http::string_body> req{http::verb::post, "/v1/audio/transcriptions", 11};
    req.set(http::field::content_type, "multipart/form-data; boundary=bnd");
    req.body() = body;
    req.prepare_payload();

    http::response<http::string_body> res;
    HandleTranscribe::handle(req,
        [&](http::response<http::string_body> r) { res = std::move(r); },
        config,
        nullptr);  // null auth + sin config → OK

    EXPECT_EQ(res.result(), http::status::ok);
}
