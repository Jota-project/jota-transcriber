#include "MultipartParser.h"
#include <stdexcept>
#include <sstream>
#include <algorithm>

namespace MultipartParser {

std::string extractBoundary(const std::string& content_type) {
    auto pos = content_type.find("boundary=");
    if (pos == std::string::npos) return "";
    std::string b = content_type.substr(pos + 9);
    // Strip optional double quotes
    if (b.size() >= 2 && b.front() == '"') {
        auto end = b.find('"', 1);
        b = (end != std::string::npos) ? b.substr(1, end - 1) : b.substr(1);
    }
    // Strip trailing semicolons and whitespace
    while (!b.empty() && (b.back() == ';' || b.back() == ' ' || b.back() == '\r'))
        b.pop_back();
    return b;
}

static std::string extractDispositionParam(const std::string& header,
                                           const std::string& param) {
    std::string needle = param + "=\"";
    auto pos = header.find(needle);
    if (pos == std::string::npos) return "";
    pos += needle.size();
    auto end = header.find('"', pos);
    return (end != std::string::npos) ? header.substr(pos, end - pos) : "";
}

std::unordered_map<std::string, Part> parse(const std::string& body,
                                             const std::string& boundary) {
    std::unordered_map<std::string, Part> result;

    const std::string first_delim = "--" + boundary;
    const std::string part_delim  = "\r\n--" + boundary;

    size_t pos = body.find(first_delim);
    if (pos == std::string::npos)
        throw std::runtime_error("MultipartParser: boundary not found in body");
    pos += first_delim.size();

    while (true) {
        // After boundary: either "--" (end) or "\r\n" (start of part headers)
        if (pos + 2 <= body.size() && body.substr(pos, 2) == "--") break;
        if (pos + 2 <= body.size() && body.substr(pos, 2) == "\r\n") pos += 2;
        else break;

        // Find end of part headers
        size_t hdrs_end = body.find("\r\n\r\n", pos);
        if (hdrs_end == std::string::npos)
            throw std::runtime_error("MultipartParser: malformed part — no header terminator");

        std::string hdrs = body.substr(pos, hdrs_end - pos);
        pos = hdrs_end + 4; // skip \r\n\r\n

        // Find next boundary using naive string search. RFC 2046 requires that
        // the boundary string does not appear in the binary content of any part;
        // if it does, the parser will incorrectly truncate the data.
        size_t next = body.find(part_delim, pos);
        if (next == std::string::npos)
            throw std::runtime_error("MultipartParser: malformed part — no closing boundary");

        // Parse headers
        Part part;
        std::string field_name;
        std::istringstream ss(hdrs);
        std::string line;
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::string line_lc = line;
            std::transform(line_lc.begin(), line_lc.end(), line_lc.begin(), ::tolower);
            if (line_lc.rfind("content-disposition:", 0) == 0) {
                field_name    = extractDispositionParam(line, "name");
                part.filename = extractDispositionParam(line, "filename");
            } else if (line_lc.rfind("content-type:", 0) == 0) {
                part.content_type = line.substr(line.find(':') + 1);
                // Trim leading space
                auto ns = part.content_type.find_first_not_of(' ');
                if (ns != std::string::npos) part.content_type = part.content_type.substr(ns);
            }
        }

        if (!field_name.empty()) {
            std::string raw = body.substr(pos, next - pos);
            part.data.assign(raw.begin(), raw.end());
            result[field_name] = std::move(part);
        }

        pos = next + part_delim.size();
    }

    return result;
}

} // namespace MultipartParser