#pragma once

#include <iostream>
#include <sstream>
#include <string>
#include <utility>

namespace srtrelay {

enum class LogLevel {
    kDebug = 0,
    kInfo = 1,
    kWarn = 2,
    kError = 3,
};

LogLevel ParseLogLevel(const std::string& value);
const char* LogLevelName(LogLevel level);
std::string NowIso8601();

struct Logger {
    LogLevel min_level = LogLevel::kInfo;

    template <typename... Parts>
    void Log(LogLevel level, const char* event, Parts&&... parts) const {
        if (static_cast<int>(level) < static_cast<int>(min_level)) {
            return;
        }
        std::ostringstream oss;
        oss << "ts=" << NowIso8601()
            << " level=" << LogLevelName(level)
            << " event=" << event;
        ((oss << " " << std::forward<Parts>(parts)), ...);
        // Keep logs off stdout so binary stdout output stays clean.
        std::cerr << oss.str() << std::endl;
    }
};

}  // namespace srtrelay
