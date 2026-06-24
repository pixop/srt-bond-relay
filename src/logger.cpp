#include "srtrelay/logger.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <stdexcept>

namespace srtrelay {

std::string NowIso8601() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t tt = system_clock::to_time_t(now);
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tm_utc {};
    gmtime_r(&tt, &tm_utc);

    std::ostringstream oss;
    oss << std::put_time(&tm_utc, "%Y-%m-%dT%H:%M:%S")
        << "." << std::setfill('0') << std::setw(3) << ms.count() << "Z";
    return oss.str();
}

LogLevel ParseLogLevel(const std::string& value) {
    if (value == "debug") return LogLevel::kDebug;
    if (value == "info") return LogLevel::kInfo;
    if (value == "warn") return LogLevel::kWarn;
    if (value == "error") return LogLevel::kError;
    throw std::runtime_error("invalid --log-level: " + value);
}

const char* LogLevelName(LogLevel level) {
    switch (level) {
        case LogLevel::kDebug: return "debug";
        case LogLevel::kInfo: return "info";
        case LogLevel::kWarn: return "warn";
        case LogLevel::kError: return "error";
    }
    return "unknown";
}

}  // namespace srtrelay
