#pragma once

#include <optional>
#include <string>
#include <vector>

#include "srtrelay/logger.hpp"

namespace srtrelay {

enum class SwitchMode {
    kSerial,
    kDelayed,
};

struct Config {
    std::vector<std::string> input_uris;
    std::string output_uri;
    int stats_interval_ms = 1000;
    int reconnect_delay_ms = 1000;
    int max_message_size = 1456;
    LogLevel log_level = LogLevel::kInfo;
    bool exit_on_input_failure = false;
    bool exit_on_output_failure = false;
    bool verify_linkage = false;
    int io_timeout_ms = 1000;
    bool metrics_enabled = true;
    std::string metrics_host = "127.0.0.1";
    int metrics_port = 9464;
    std::optional<size_t> primary_input_index;
    SwitchMode switch_mode = SwitchMode::kSerial;
};

void PrintUsage();
const char* SoftwareVersion();
void PrintVersion();
bool ParseBool(const std::string& value);
SwitchMode ParseSwitchMode(const std::string& value);
const char* SwitchModeName(SwitchMode mode);
Config ParseArgs(int argc, char** argv);

}  // namespace srtrelay
