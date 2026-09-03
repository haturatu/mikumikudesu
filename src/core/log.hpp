#pragma once

#include <cstdlib>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string_view>

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

template <typename... Args> void write(Level level, Args&&... args) {
    static std::mutex mutex;
    std::ostringstream line;
    line << '[' << name(level) << "] ";
    (line << ... << std::forward<Args>(args));

    std::scoped_lock lock(mutex);
    std::ostream& stream = (level == Level::warn || level == Level::error) ? std::cerr : std::cout;
    stream << line.str() << '\n';
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
