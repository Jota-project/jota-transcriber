#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <cctype>

struct HallucinationConfig {
    size_t max_length = 500;
    int max_consecutive = 4;
    int max_repeated_bigrams = 4;
    int max_repeated_trigrams = 3;
    int max_word_frequency = 8;
    bool clean_instead_of_discard = true;
    int max_clean_consecutive = 2;
};

inline bool isHallucination(const std::string& text, const HallucinationConfig& cfg = {}) {
    if (text.length() > cfg.max_length) return true;

    std::vector<std::string> words;
    std::istringstream ss(text);
    std::string w;
    while (ss >> w) {
        words.push_back(w);
    }
    if (words.size() < 2) return false;

    int consecutive = 1;
    for (size_t i = 1; i < words.size(); ++i) {
        if (words[i] == words[i-1]) {
            if (++consecutive >= cfg.max_consecutive) return true;
        } else {
            consecutive = 1;
        }
    }

    std::unordered_map<std::string, int> bigrams;
    for (size_t i = 1; i < words.size(); ++i) {
        std::string bg = words[i-1] + " " + words[i];
        if (++bigrams[bg] >= cfg.max_repeated_bigrams) return true;
    }

    std::unordered_map<std::string, int> trigrams;
    for (size_t i = 2; i < words.size(); ++i) {
        std::string tg = words[i-2] + " " + words[i-1] + " " + words[i];
        if (++trigrams[tg] >= cfg.max_repeated_trigrams) return true;
    }

    std::unordered_map<std::string, int> word_freq;
    for (const auto& word : words) {
        if (++word_freq[word] >= cfg.max_word_frequency) return true;
    }

    return false;
}

inline std::string cleanHallucination(const std::string& text, const HallucinationConfig& cfg = {}) {
    if (text.empty()) return {};

    std::vector<std::string> words;
    std::istringstream ss(text);
    std::string w;
    while (ss >> w) {
        words.push_back(w);
    }
    if (words.empty()) return text;
    if (words.size() == 1) return text;

    std::vector<std::string> cleaned;
    int consecutive_count = 1;

    for (size_t i = 0; i < words.size(); ++i) {
        if (i > 0 && words[i] == words[i-1]) {
            consecutive_count++;
 } else {
            consecutive_count = 1;
        }

        if (consecutive_count <= cfg.max_clean_consecutive) {
            cleaned.push_back(words[i]);
        }
    }

    std::unordered_map<std::string, int> bigrams;
    std::vector<std::string> final_result;
    for (size_t i = 0; i < cleaned.size(); ++i) {
        final_result.push_back(cleaned[i]);
        if (i > 0) {
            std::string bg = cleaned[i-1] + " " + cleaned[i];
            if (++bigrams[bg] >= cfg.max_repeated_bigrams) {
                final_result.pop_back();
            }
        }
    }

    std::ostringstream out;
    for (size_t i = 0; i < final_result.size(); ++i) {
        if (i > 0) out << " ";
        out << final_result[i];
    }
    return out.str();
}

struct ModelHallucinationProfile {
    HallucinationConfig config;
    std::string name;
};

inline HallucinationConfig getHallucinationConfigForModel(const std::string& model_path) {
    HallucinationConfig cfg;

    if (model_path.find("large") != std::string::npos) {
        cfg.max_consecutive = 6;
        cfg.max_repeated_bigrams = 6;
        cfg.max_repeated_trigrams = 5;
        cfg.max_word_frequency = 12;
        cfg.max_clean_consecutive = 3;
    } else if (model_path.find("medium") != std::string::npos) {
        cfg.max_consecutive = 5;
        cfg.max_repeated_bigrams = 5;
        cfg.max_repeated_trigrams = 4;
        cfg.max_word_frequency = 10;
        cfg.max_clean_consecutive = 2;
    } else if (model_path.find("small") != std::string::npos) {
        cfg.max_consecutive = 4;
        cfg.max_repeated_bigrams = 4;
        cfg.max_repeated_trigrams = 3;
        cfg.max_word_frequency = 8;
        cfg.max_clean_consecutive = 2;
    } else if (model_path.find("base") != std::string::npos) {
        cfg.max_consecutive = 3;
        cfg.max_repeated_bigrams = 3;
        cfg.max_repeated_trigrams = 2;
        cfg.max_word_frequency = 6;
        cfg.max_clean_consecutive = 2;
    } else {
        cfg.max_consecutive = 4;
        cfg.max_repeated_bigrams = 4;
        cfg.max_repeated_trigrams = 3;
        cfg.max_word_frequency = 8;
        cfg.max_clean_consecutive = 2;
    }

    return cfg;
}
