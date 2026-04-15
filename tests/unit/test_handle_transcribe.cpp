#include <gtest/gtest.h>
#include "server/handlers/HandleTranscribe.h"
#include "server/ServerConfig.h"
#include "server/AuthManager.h"
#include "auth/ApiAuthConfig.h"
#include "whisper/ModelCache.h"
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