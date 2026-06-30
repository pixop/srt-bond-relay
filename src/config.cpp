#include "srtrelay/config.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace srtrelay {

namespace {

#ifndef SRT_BOND_RELAY_VERSION
#define SRT_BOND_RELAY_VERSION "dev"
#endif

bool IsStdinInputSpec(const std::string& input_uri) {
    return input_uri == "stdin" || input_uri == "-" || input_uri == "fd://stdin";
}

}  // namespace

void PrintUsage() {
    std::cout
        << "Usage:\n"
        << "  srt-bond-relay \\\n"
        << "    --input '<srt://...|udp://...|stdin|group-list>' [--input '...'] \\\n"
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
        << "  --primary-input-index <1..N>\n"
        << "  --switch-mode serial|delayed\n"
        << "  --version\n"
        << "\nI/O mode notes:\n"
        << "  Input:  srt:// mode=listener|caller, udp:// mode=listener (caller unsupported), stdin aliases: stdin|-|fd://stdin\n"
        << "  Output: srt:// mode=caller|listener, udp:// mode=caller (listener unsupported), stdout aliases: stdout|-|fd://stdout\n"
        << "  Group list for bonded SRT: separate endpoints with ';' or ','\n"
        << "  Group lists and bond options are only supported for SRT URIs\n"
        << "  Repeat --input to configure independent switched input sources\n"
        << "  Multi-input mode supports max one stdin source\n"
        << "  Bond mode query options: grouptype|group_type|bond|bond_mode\n"
        << "  Bonded per-path source IP options: srcip|sourceip|localip|adapterip|adapter_ip\n"
        << "  UDP query options: rcvbuf|sndbuf|reuseaddr|ttl|localip|localport\n"
        << "  --help\n";
}

const char* SoftwareVersion() { return SRT_BOND_RELAY_VERSION; }

void PrintVersion() { std::cout << "srt-bond-relay " << SoftwareVersion() << '\n'; }

bool ParseBool(const std::string& value) {
    if (value == "true" || value == "1" || value == "yes") return true;
    if (value == "false" || value == "0" || value == "no") return false;
    throw std::runtime_error("invalid boolean value: " + value);
}

SwitchMode ParseSwitchMode(const std::string& value) {
    if (value == "serial") {
        return SwitchMode::kSerial;
    }
    if (value == "delayed") {
        return SwitchMode::kDelayed;
    }
    throw std::runtime_error("invalid --switch-mode value: " + value + " (expected serial or delayed)");
}

const char* SwitchModeName(SwitchMode mode) {
    switch (mode) {
        case SwitchMode::kSerial:
            return "serial";
        case SwitchMode::kDelayed:
            return "delayed";
    }
    return "unknown";
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
        } else if (arg == "--version" || arg == "-V") {
            PrintVersion();
            std::exit(0);
        } else if (arg == "--input") {
            cfg.input_uris.push_back(require_value("--input"));
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
        } else if (arg == "--primary-input-index") {
            const int index = std::stoi(require_value("--primary-input-index"));
            if (index <= 0) {
                throw std::runtime_error("--primary-input-index must be in range 1..N");
            }
            cfg.primary_input_index = static_cast<size_t>(index - 1);
        } else if (arg == "--switch-mode") {
            cfg.switch_mode = ParseSwitchMode(require_value("--switch-mode"));
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }

    if (!cfg.verify_linkage) {
        if (cfg.input_uris.empty()) throw std::runtime_error("--input is required");
        if (cfg.input_uris.size() > 16) {
            throw std::runtime_error("at most 16 --input values are supported");
        }
        if (cfg.output_uri.empty()) throw std::runtime_error("--output is required");
        size_t stdin_count = 0;
        for (const auto& input_uri : cfg.input_uris) {
            if (IsStdinInputSpec(input_uri)) {
                ++stdin_count;
            }
        }
        if (stdin_count > 1) {
            throw std::runtime_error("multi-input mode supports at most one stdin source");
        }
        if (cfg.primary_input_index.has_value() && *cfg.primary_input_index >= cfg.input_uris.size()) {
            throw std::runtime_error("--primary-input-index must be in range 1.." +
                                     std::to_string(cfg.input_uris.size()));
        }
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
