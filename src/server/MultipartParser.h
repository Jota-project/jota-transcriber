#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace MultipartParser {

struct Part {
    std::string              filename;     // empty for text fields
    std::string              content_type; // empty for text fields
    std::vector<uint8_t>     data;
};

// Extracts the boundary value from a Content-Type header string like
// "multipart/form-data; boundary=----Boundary123"
// Returns empty string if boundary is not present.
std::string extractBoundary(const std::string& content_type);

// Parses a multipart/form-data body string.
// Returns a map of field name → Part.
// Throws std::runtime_error if the body is malformed (no boundary found, etc.).
std::unordered_map<std::string, Part> parse(const std::string& body,
                                             const std::string& boundary);

} // namespace MultipartParser