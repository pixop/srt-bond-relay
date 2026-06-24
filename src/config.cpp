#include "srtrelay/config.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace srtrelay {

void PrintUsage() {
    std::cout
        << "Usage:\n"
        << "  srt-bond-relay \\\n"
        << "    --input '<srt://...|stdin|group-list>' \\\n"
        << "    --output '<srt://...|stdout|group-list>' \\\n"
        << "    --stats-interval-ms 1000 \\\n"
        << "    --reconnect-delay-ms 1000\n\n"
        << "Optional flags:\n"
        << "  --max-message-size 1456\n"
        << "  --log-level info|debug|warn|error\n"
        << "  --exit-on-input-failure true|false\n"
        << "  --exit-on-output-failure true|false\n"
        << "  --verify-linkage\n"
        << "  --io-timeout-ms 1000\n"
        << "  --metrics-enabled true|false\n"
        << "  --metrics-host 127.0.0.1\n"
        << "  --metrics-port 9464\n"
        << "\nI/O mode notes:\n"
        << "  Input:  srt mode=listener|caller, stdin aliases: stdin|-|fd://stdin\n"
        << "  Output: srt mode=caller|listener, stdout aliases: stdout|-|fd://stdout\n"
        << "  Group list for bonded SRT: separate endpoints with ';' or ','\n"
        << "  Bond mode query options: grouptype|group_type|bond|bond_mode\n"
        << "  Bonded per-path source IP options: srcip|sourceip|localip|adapterip|adapter_ip\n"
        << "  --help\n";
}

bool ParseBool(const std::string& value) {
    if (value == "true" || value == "1" || value == "yes") return true;
    if (value == "false" || value == "0" || value == "no") return false;
    throw std::runtime_error("invalid boolean value: " + value);
}

Config ParseArgs(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto require_value = [&](const char* key) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error(std::string("missing value for ") + key);
            }
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h") {
            PrintUsage();
            std::exit(0);
        } else if (arg == "--input") {
            cfg.input_uri = require_value("--input");
        } else if (arg == "--output") {
            cfg.output_uri = require_value("--output");
        } else if (arg == "--stats-interval-ms") {
            cfg.stats_interval_ms = std::stoi(require_value("--stats-interval-ms"));
        } else if (arg == "--reconnect-delay-ms") {
            cfg.reconnect_delay_ms = std::stoi(require_value("--reconnect-delay-ms"));
        } else if (arg == "--max-message-size") {
            cfg.max_message_size = std::stoi(require_value("--max-message-size"));
        } else if (arg == "--log-level") {
            cfg.log_level = ParseLogLevel(require_value("--log-level"));
        } else if (arg == "--exit-on-input-failure") {
            cfg.exit_on_input_failure = ParseBool(require_value("--exit-on-input-failure"));
        } else if (arg == "--exit-on-output-failure") {
            cfg.exit_on_output_failure = ParseBool(require_value("--exit-on-output-failure"));
        } else if (arg == "--verify-linkage") {
            cfg.verify_linkage = true;
        } else if (arg == "--io-timeout-ms") {
            cfg.io_timeout_ms = std::stoi(require_value("--io-timeout-ms"));
        } else if (arg == "--metrics-enabled") {
            cfg.metrics_enabled = ParseBool(require_value("--metrics-enabled"));
        } else if (arg == "--metrics-host") {
            cfg.metrics_host = require_value("--metrics-host");
        } else if (arg == "--metrics-port") {
            cfg.metrics_port = std::stoi(require_value("--metrics-port"));
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }

    if (!cfg.verify_linkage) {
        if (cfg.input_uri.empty()) throw std::runtime_error("--input is required");
        if (cfg.output_uri.empty()) throw std::runtime_error("--output is required");
    }

    if (cfg.stats_interval_ms <= 0) throw std::runtime_error("--stats-interval-ms must be > 0");
    if (cfg.reconnect_delay_ms <= 0) throw std::runtime_error("--reconnect-delay-ms must be > 0");
    if (cfg.max_message_size <= 0) throw std::runtime_error("--max-message-size must be > 0");
    if (cfg.io_timeout_ms <= 0) throw std::runtime_error("--io-timeout-ms must be > 0");
    if (cfg.metrics_port <= 0 || cfg.metrics_port > 65535) {
        throw std::runtime_error("--metrics-port must be in range 1..65535");
    }
    if (cfg.metrics_host.empty()) {
        throw std::runtime_error("--metrics-host must not be empty");
    }
    return cfg;
}

}  // namespace srtrelay
