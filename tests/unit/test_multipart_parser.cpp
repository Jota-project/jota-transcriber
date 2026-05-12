#include <gtest/gtest.h>
#include "server/MultipartParser.h"

// Helper: build a minimal multipart body with one text field.
static std::string makeTextPart(const std::string& boundary,
                                const std::string& name,
                                const std::string& value) {
    return "--" + boundary + "\r\n"
           "Content-Disposition: form-data; name=\"" + name + "\"\r\n"
           "\r\n" +
           value + "\r\n"
           "--" + boundary + "--\r\n";
}

// Helper: build a multipart body with a binary file field + a text field.
static std::string makeFilePart(const std::string& boundary,
                                const std::string& field_name,
                                const std::string& filename,
                                const std::string& file_data,
                                const std::string& extra_name = "",
                                const std::string& extra_value = "") {
    std::string body =
        "--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"" + field_name + "\"; filename=\"" + filename + "\"\r\n"
        "Content-Type: audio/ogg\r\n"
        "\r\n" +
        file_data + "\r\n";
    if (!extra_name.empty()) {
        body +=
            "--" + boundary + "\r\n"
            "Content-Disposition: form-data; name=\"" + extra_name + "\"\r\n"
            "\r\n" +
            extra_value + "\r\n";
    }
    body += "--" + boundary + "--\r\n";
    return body;
}

TEST(MultipartParserTest, ExtractsBoundaryFromContentType) {
    std::string ct = "multipart/form-data; boundary=----WebKitBoundaryXYZ";
    EXPECT_EQ(MultipartParser::extractBoundary(ct), "----WebKitBoundaryXYZ");
}

TEST(MultipartParserTest, ExtractsBoundaryWithQuotes) {
    std::string ct = "multipart/form-data; boundary=\"myBoundary\"";
    EXPECT_EQ(MultipartParser::extractBoundary(ct), "myBoundary");
}

TEST(MultipartParserTest, ExtractsBoundaryReturnsEmptyIfMissing) {
    EXPECT_EQ(MultipartParser::extractBoundary("application/json"), "");
}

TEST(MultipartParserTest, ParsesSingleTextField) {
    std::string body = makeTextPart("bound", "model", "whisper-1");
    auto parts = MultipartParser::parse(body, "bound");
    ASSERT_EQ(parts.count("model"), 1u);
    std::string val(parts.at("model").data.begin(), parts.at("model").data.end());
    EXPECT_EQ(val, "whisper-1");
}

TEST(MultipartParserTest, ParsesFileFieldWithFilename) {
    std::string body = makeFilePart("bound", "file", "audio.ogg", "FAKEAUDIO");
    auto parts = MultipartParser::parse(body, "bound");
    ASSERT_EQ(parts.count("file"), 1u);
    EXPECT_EQ(parts.at("file").filename, "audio.ogg");
    std::string data(parts.at("file").data.begin(), parts.at("file").data.end());
    EXPECT_EQ(data, "FAKEAUDIO");
}

TEST(MultipartParserTest, ParsesFileAndTextField) {
    std::string body = makeFilePart("bound", "file", "a.ogg", "AUDIO", "model", "whisper-1");
    auto parts = MultipartParser::parse(body, "bound");
    EXPECT_EQ(parts.count("file"),  1u);
    EXPECT_EQ(parts.count("model"), 1u);
    std::string model_val(parts.at("model").data.begin(), parts.at("model").data.end());
    EXPECT_EQ(model_val, "whisper-1");
}

TEST(MultipartParserTest, ThrowsOnMissingBoundary) {
    EXPECT_THROW(MultipartParser::parse("no boundary here", "missing"), std::runtime_error);
}

TEST(MultipartParserTest, ContentTypeIsExtracted) {
    std::string body = makeFilePart("b", "file", "f.ogg", "data");
    auto parts = MultipartParser::parse(body, "b");
    EXPECT_EQ(parts.at("file").content_type, "audio/ogg");
}

// M2 — HTTP headers are case-insensitive (RFC 7230)
TEST(MultipartParserTest, ParsesLowercaseContentTypeHeader) {
    std::string body =
        "--bound\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"a.wav\"\r\n"
        "content-type: audio/wav\r\n"
        "\r\n"
        "AUDIO\r\n"
        "--bound--\r\n";
    auto parts = MultipartParser::parse(body, "bound");
    ASSERT_EQ(parts.count("file"), 1u);
    EXPECT_EQ(parts.at("file").content_type, "audio/wav");
}

TEST(MultipartParserTest, ParsesLowercaseContentDispositionHeader) {
    std::string body =
        "--bound\r\n"
        "content-disposition: form-data; name=\"model\"\r\n"
        "\r\n"
        "whisper-1\r\n"
        "--bound--\r\n";
    auto parts = MultipartParser::parse(body, "bound");
    ASSERT_EQ(parts.count("model"), 1u);
    std::string val(parts.at("model").data.begin(), parts.at("model").data.end());
    EXPECT_EQ(val, "whisper-1");
}

TEST(MultipartParserTest, ParsesMixedCaseContentTypeHeader) {
    std::string body =
        "--bound\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"a.ogg\"\r\n"
        "CONTENT-TYPE: audio/ogg\r\n"
        "\r\n"
        "AUDIO\r\n"
        "--bound--\r\n";
    auto parts = MultipartParser::parse(body, "bound");
    ASSERT_EQ(parts.count("file"), 1u);
    EXPECT_EQ(parts.at("file").content_type, "audio/ogg");
}