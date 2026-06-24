#pragma once

#include <string>

#include "srtrelay/logger.hpp"

namespace srtrelay {

struct Config {
    std::string input_uri;
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
};

void PrintUsage();
bool ParseBool(const std::string& value);
Config ParseArgs(int argc, char** argv);

}  // namespace srtrelay
