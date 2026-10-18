#pragma once

#include <cstdlib>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string_view>
#include <utility>

namespace dayo::log {

enum class Level { debug, info, warn, error };

inline std::string_view name(Level level) noexcept {
    switch (level) {
    case Level::debug:
        return "DEBUG";
    case Level::info:
        return "INFO";
    case Level::warn:
        return "WARN";
    case Level::error:
        return "ERROR";
    }
    return "ERROR";
}

// DAYO_LOG_LEVEL=debug|info|warn|error (default: info).
// Read once; log calls can run on the render thread.
inline Level minimumLevel() noexcept {
    static const Level configured = [] {
        const char* value = std::getenv("DAYO_LOG_LEVEL");
        const std::string_view name = value != nullptr ? value : "";
        if (name == "debug" || name == "DEBUG")
            return Level::debug;
        if (name == "warn" || name == "WARN" || name == "warning")
            return Level::warn;
        if (name == "error" || name == "ERROR")
            return Level::error;
        return Level::info;
    }();
    return configured;
}

inline bool enabled(Level level) noexcept {
    return static_cast<int>(level) >= static_cast<int>(minimumLevel());
}

inline std::mutex& outputMutex() {
    static std::mutex mutex;
    return mutex;
}

template <typename... Args> void write(Level level, Args&&... args) {
    if (!enabled(level))
        return;
    std::ostringstream line;
    line << '[' << name(level) << "] ";
    (line << ... << std::forward<Args>(args));

    std::scoped_lock lock(outputMutex());
    std::ostream& stream = (level == Level::warn || level == Level::error) ? std::cerr : std::cout;
    stream << line.str() << '\n';
    if (level == Level::warn || level == Level::error)
        stream.flush();
}

template <typename... Args> void debug(Args&&... args) {
    write(Level::debug, std::forward<Args>(args)...);
}
template <typename... Args> void info(Args&&... args) {
    write(Level::info, std::forward<Args>(args)...);
}
template <typename... Args> void warn(Args&&... args) {
    write(Level::warn, std::forward<Args>(args)...);
}
template <typename... Args> void error(Args&&... args) {
    write(Level::error, std::forward<Args>(args)...);
}
template <typename... Args> [[noreturn]] void fatal(Args&&... args) {
    write(Level::error, std::forward<Args>(args)...);
    std::exit(1);
}

} // namespace dayo::log
