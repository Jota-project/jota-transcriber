#pragma once
#include <string>
#include <sstream>
#include <unordered_map>

/**
 * Detects transcription hallucination loops.
 * Returns true if 'text' contains obvious repetition artifacts:
 *   - Total length > 500 chars (decoder loop filling context)
 *   - 4+ consecutive identical words
 *   - 4+ repeated bigrams
 */
inline bool isHallucination(const std::string& text) {
    if (text.length() > 500) return true;

    std::istringstream ss(text);
    std::unordered_map<std::string, int> bigrams;
    std::string prev, cur;
    int consec = 0;

    while (ss >> cur) {
        if (cur == prev) {
            if (++consec >= 4) return true;
        } else {
            consec = 1;
        }
        if (!prev.empty() && ++bigrams[prev + ' ' + cur] >= 4) return true;
        prev = cur;
    }
    return false;
}
