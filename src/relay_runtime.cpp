#include "srtrelay/relay.hpp"

#include <chrono>
#include <stdexcept>
#include <thread>
#include <vector>

#include "srtrelay/metrics.hpp"
#include "srtrelay/relay_io.hpp"
#include "srtrelay/srt_utils.hpp"

namespace srtrelay {

namespace {

std::atomic<bool> g_shutdown_requested{false};

void SleepReconnectDelay(const Config& cfg) {
    const auto delay = std::chrono::milliseconds(cfg.reconnect_delay_ms);
    const auto chunk = std::chrono::milliseconds(100);
    std::chrono::milliseconds slept(0);
    while (slept < delay && !g_shutdown_requested.load()) {
        const auto to_sleep = std::min(chunk, delay - slept);
        std::this_thread::sleep_for(to_sleep);
        slept += to_sleep;
    }
}

const char* InputModeLabel(InputEndpointKind kind) {
    switch (kind) {
        case InputEndpointKind::kStdin:
            return "stdin";
        case InputEndpointKind::kSrtCaller:
            return "srt-caller";
        case InputEndpointKind::kSrtListener:
            return "srt-listener";
        case InputEndpointKind::kUdpCaller:
            return "udp-caller";
        case InputEndpointKind::kUdpListener:
            return "udp-listener";
    }
    return "unknown";
}

const char* OutputModeLabel(OutputEndpointKind kind) {
    switch (kind) {
        case OutputEndpointKind::kStdout:
            return "stdout";
        case OutputEndpointKind::kSrtListener:
            return "srt-listener";
        case OutputEndpointKind::kSrtCaller:
            return "srt-caller";
        case OutputEndpointKind::kUdpCaller:
            return "udp-caller";
        case OutputEndpointKind::kUdpListener:
            return "udp-listener";
    }
    return "unknown";
}

bool IsSrtInputKind(InputEndpointKind kind) {
    return kind == InputEndpointKind::kSrtListener || kind == InputEndpointKind::kSrtCaller;
}

int RelayMain(const Config& cfg, const Logger& logger) {
    const InputEndpointSpec input_spec = ParseInputEndpointSpec(cfg);
    const OutputEndpointSpec output_spec = ParseOutputEndpointSpec(cfg);

    RelayStats stats;
    RelayState state;
    MetricsState metrics;
    std::vector<char> buffer(static_cast<size_t>(cfg.max_message_size));
    auto last_stats_at = std::chrono::steady_clock::now();
    const auto startup_ms = UnixNowMs();
    metrics.last_rx_unix_ms.store(startup_ms, std::memory_order_relaxed);
    metrics.last_tx_unix_ms.store(startup_ms, std::memory_order_relaxed);

    std::unique_ptr<InputSource> input_source = BuildInputSource(input_spec);
    std::unique_ptr<OutputSink> output_sink = BuildOutputSink(output_spec);
    MetricsServer metrics_server(cfg, logger, metrics);
    metrics_server.Start();

    logger.Log(LogLevel::kInfo, "startup",
               "input_uri=" + cfg.input_uri,
               "output_uri=" + cfg.output_uri,
               "input_mode=" + std::string(InputModeLabel(input_spec.kind)),
               "output_mode=" + std::string(OutputModeLabel(output_spec.kind)),
               "input_bonded=" + std::string(input_spec.bonded ? "true" : "false"),
               "output_bonded=" + std::string(output_spec.bonded ? "true" : "false"),
               "stats_interval_ms=" + std::to_string(cfg.stats_interval_ms),
               "reconnect_delay_ms=" + std::to_string(cfg.reconnect_delay_ms),
               "max_message_size=" + std::to_string(cfg.max_message_size),
               "io_timeout_ms=" + std::to_string(cfg.io_timeout_ms),
               "metrics_enabled=" + std::string(cfg.metrics_enabled ? "true" : "false"),
               "metrics_host=" + cfg.metrics_host,
               "metrics_port=" + std::to_string(cfg.metrics_port));

    while (!g_shutdown_requested.load()) {
        state.input_listening = input_source->IsListening();
        state.input_connected = input_source->IsConnected();
        state.output_connected = output_sink->IsConnected();
        MaybeLogStats(cfg, logger, &stats, state, &metrics,
                      input_source->SessionSocket(), output_sink->TransportSocket(),
                      output_sink->MetricsMode(), &last_stats_at);

        if (!input_source->IsConnected()) {
            try {
                input_source->EnsureReady(cfg, logger, &metrics);
            } catch (const std::exception& ex) {
                if (input_source->LastEnsureErrorKind() == IoErrorKind::kTimeout) {
                    continue;
                }
                const std::string message = input_source->LastEnsureErrorMessage().empty()
                                                ? std::string(ex.what())
                                                : input_source->LastEnsureErrorMessage();
                logger.Log(LogLevel::kWarn, "input-ensure-failed", "error=" + message);
                input_source->HandleReceiveError(cfg, logger, &metrics);
                if (cfg.exit_on_input_failure) return 2;
                SleepReconnectDelay(cfg);
                continue;
            }
        }

        if (!output_sink->IsConnected()) {
            try {
                output_sink->EnsureReady(cfg, logger, &stats, &metrics);
            } catch (const std::exception& ex) {
                if (output_sink->LastEnsureErrorKind() == IoErrorKind::kTimeout) {
                    continue;
                }
                const std::string message = output_sink->LastEnsureErrorMessage().empty()
                                                ? std::string(ex.what())
                                                : output_sink->LastEnsureErrorMessage();
                logger.Log(LogLevel::kWarn, "output-connect-failed",
                           "error=" + message,
                           "reconnect_count=" + std::to_string(stats.reconnect_count));
                output_sink->MarkDisconnected(&metrics);
                if (cfg.exit_on_output_failure) return 3;
                SleepReconnectDelay(cfg);
                continue;
            }
        }

        SRT_MSGCTRL rx_ctrl = srt_msgctrl_default;
        SRT_SOCKGROUPDATA rx_group_data[16] {};
        rx_ctrl.grpdata = rx_group_data;
        rx_ctrl.grpdata_size = sizeof(rx_group_data) / sizeof(rx_group_data[0]);
        const int recv_size = input_source->Receive(cfg, &buffer, &rx_ctrl);
        if (recv_size == SRT_ERROR) {
            if (input_source->IsTerminalEof()) {
                logger.Log(LogLevel::kInfo, "input-eof");
                break;
            }
            if (input_source->LastReceiveErrorKind() == IoErrorKind::kTimeout) {
                continue;
            }
            std::string err = input_source->LastReceiveErrorMessage();
            if (err.empty()) err = "input receive failed";
            logger.Log(LogLevel::kWarn, "input-disconnected", "error=" + err);
            input_source->HandleReceiveError(cfg, logger, &metrics);
            if (cfg.exit_on_input_failure) return 2;
            continue;
        }

        stats.total_rx_bytes += static_cast<uint64_t>(recv_size);
        stats.total_rx_msgs += 1;
        stats.interval_rx_bytes += static_cast<uint64_t>(recv_size);
        stats.interval_rx_msgs += 1;
        if (IsSrtInputKind(input_spec.kind)) {
            UpdateInputLinkHealthFromMsgCtrl(rx_ctrl, &metrics);
        }
        metrics.total_rx_bytes.store(stats.total_rx_bytes, std::memory_order_relaxed);
        metrics.total_rx_msgs.store(stats.total_rx_msgs, std::memory_order_relaxed);
        metrics.last_rx_unix_ms.store(UnixNowMs(), std::memory_order_relaxed);

        const int sent_size = output_sink->Send(buffer.data(), recv_size);
        if (sent_size == SRT_ERROR) {
            stats.total_send_failures += 1;
            stats.interval_send_failures += 1;
            stats.reconnect_count += 1;
            metrics.total_send_failures.store(stats.total_send_failures, std::memory_order_relaxed);
            metrics.reconnect_count.store(stats.reconnect_count, std::memory_order_relaxed);

            std::string err = output_sink->LastSendErrorMessage();
            if (err.empty()) err = "output send failed";
            logger.Log(LogLevel::kWarn, "output-send-failed",
                       "error=" + err,
                       "reconnect_count=" + std::to_string(stats.reconnect_count));
            output_sink->MarkDisconnected(&metrics);
            if (cfg.exit_on_output_failure) return 3;
            SleepReconnectDelay(cfg);
            continue;
        }

        stats.total_tx_bytes += static_cast<uint64_t>(sent_size);
        stats.total_tx_msgs += 1;
        stats.interval_tx_bytes += static_cast<uint64_t>(sent_size);
        stats.interval_tx_msgs += 1;
        metrics.total_tx_bytes.store(stats.total_tx_bytes, std::memory_order_relaxed);
        metrics.total_tx_msgs.store(stats.total_tx_msgs, std::memory_order_relaxed);
        metrics.last_tx_unix_ms.store(UnixNowMs(), std::memory_order_relaxed);
    }

    logger.Log(LogLevel::kInfo, "shutdown",
               "total_rx_bytes=" + std::to_string(stats.total_rx_bytes),
               "total_tx_bytes=" + std::to_string(stats.total_tx_bytes),
               "total_rx_msgs=" + std::to_string(stats.total_rx_msgs),
               "total_tx_msgs=" + std::to_string(stats.total_tx_msgs),
               "total_send_failures=" + std::to_string(stats.total_send_failures),
               "reconnect_count=" + std::to_string(stats.reconnect_count));
    return 0;
}

}  // namespace

void OnSignal(int) {
    g_shutdown_requested.store(true);
}

int RunRelay(const Config& cfg, const Logger& logger) {
    return RelayMain(cfg, logger);
}

}  // namespace srtrelay
