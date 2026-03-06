#pragma once
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>

// Simple thread-safe logger with timestamps and log levels.
//
// Usage:
//   Log::info("Server started", "main");
//   Log::warn("Cache miss", session_id_);
//   Log::error("Model load failed: " + e.what(), session_id_);
//   Log::debug("HTTP 200 from auth API", session_id_);
//
// Set LOG_LEVEL env var at runtime (not supported here — compile-time only).
// To suppress DEBUG output compile with -DLOG_NO_DEBUG.

class Log {
public:
    static void debug(const std::string& msg, const std::string& ctx = "") {
#ifndef LOG_NO_DEBUG
        write(Level::DEBUG, msg, ctx);
#endif
    }
    static void info (const std::string& msg, const std::string& ctx = "") { write(Level::INFO,  msg, ctx); }
    static void warn (const std::string& msg, const std::string& ctx = "") { write(Level::WARN,  msg, ctx); }
    static void error(const std::string& msg, const std::string& ctx = "") { write(Level::ERROR, msg, ctx); }

    // Mask a token/key for safe logging: show first 6 chars + "..."
    static std::string maskKey(const std::string& key) {
        if (key.size() <= 6) return "***";
        return key.substr(0, 6) + "...";
    }

private:
    enum class Level { DEBUG, INFO, WARN, ERROR };

    static void write(Level level, const std::string& msg, const std::string& ctx) {
        static std::mutex mx;
        std::lock_guard<std::mutex> lk(mx);

        auto& out = (level >= Level::WARN) ? std::cerr : std::cout;
        out << timestamp() << " " << tag(level);
        if (!ctx.empty()) out << " [" << ctx << "]";
        out << " " << msg << "\n";
        out.flush();
    }

    static std::string timestamp() {
        auto now  = std::chrono::system_clock::now();
        auto ms   = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
        auto time = std::chrono::system_clock::to_time_t(now);
        std::ostringstream oss;
        // localtime_r is thread-safe; localtime is not on all platforms
        std::tm tm{};
        localtime_r(&time, &tm);
        oss << std::put_time(&tm, "%H:%M:%S")
            << '.' << std::setfill('0') << std::setw(3) << ms.count();
        return oss.str();
    }

    static const char* tag(Level l) {
        switch (l) {
            case Level::DEBUG: return "[DEBUG]";
            case Level::INFO:  return "[INFO] ";
            case Level::WARN:  return "[WARN] ";
            case Level::ERROR: return "[ERROR]";
        }
        return "[?]   ";
    }
};
