#include "srtrelay/relay.hpp"

#include <chrono>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

#include "srtrelay/metrics.hpp"
#include "srtrelay/relay_io.hpp"
#include "srtrelay/srt_utils.hpp"
#include "srtrelay/causality.hpp"

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

struct SideCausalityState {
    uint64_t next_attempt_id = 1;
    uint64_t active_attempt_id = 0;
    bool attempt_active = false;
    uint64_t consecutive_receive_timeouts = 0;
};

struct IncidentState {
    uint64_t next_incident_seq = 1;
    bool active = false;
    std::string id;
    int64_t opened_unix_ms = 0;
    FailureSide first_side = FailureSide::kInput;
    ReasonClass first_reason_class = ReasonClass::kInternal;
    ReasonCode first_reason_code = ReasonCode::kInternalError;
    uint64_t input_failures = 0;
    uint64_t output_failures = 0;
    uint64_t input_timeouts = 0;
    uint64_t output_timeouts = 0;
};

class CausalityTracker {
public:
    explicit CausalityTracker(MetricsState* metrics) : metrics_(metrics) {}
    static constexpr uint64_t kReceiveTimeoutIncidentThreshold = 3;

    uint64_t EnsureAttemptStarted(FailureSide side) {
        auto& state = SideState(side);
        if (!state.attempt_active) {
            state.attempt_active = true;
            state.active_attempt_id = state.next_attempt_id++;
            const size_t side_index = SideIndex(side);
            metrics_->active_attempt_id[side_index].store(state.active_attempt_id, std::memory_order_relaxed);
            metrics_->reconnect_attempts_total[side_index].fetch_add(1, std::memory_order_relaxed);
        }
        return state.active_attempt_id;
    }

    uint64_t ActiveAttemptId(FailureSide side) const {
        return side == FailureSide::kInput ? input_.active_attempt_id : output_.active_attempt_id;
    }

    void NoteDataSuccess(FailureSide side) {
        auto& state = SideState(side);
        state.consecutive_receive_timeouts = 0;
    }

    void CompleteAttempt(FailureSide side) {
        auto& state = SideState(side);
        state.attempt_active = false;
        state.active_attempt_id = 0;
        metrics_->active_attempt_id[SideIndex(side)].store(0, std::memory_order_relaxed);
    }

    std::string ActiveIncidentId() const { return incident_.active ? incident_.id : std::string(); }

    ReasonDescriptor RecordFailure(const Logger& logger,
                                   FailureSide side,
                                   FailureOperation operation,
                                   bool is_timeout,
                                   bool is_disconnected,
                                   bool listener_mode,
                                   const std::string& reason_detail,
                                   const std::string& source,
                                   uint64_t attempt_id,
                                   int64_t socket_id) {
        const ReasonDescriptor reason =
            ClassifyReason(operation, is_timeout, is_disconnected, listener_mode, reason_detail);
        const int64_t now_ms = UnixNowMs();
        const int64_t now_seconds = now_ms / 1000;
        auto& side_state = SideState(side);
        bool suppress_incident_open = false;
        if (operation == FailureOperation::kReceive && reason.reason_class == ReasonClass::kTimeout) {
            side_state.consecutive_receive_timeouts += 1;
            suppress_incident_open =
                !incident_.active &&
                side_state.consecutive_receive_timeouts < kReceiveTimeoutIncidentThreshold;
        } else {
            side_state.consecutive_receive_timeouts = 0;
        }
        if (!incident_.active && !suppress_incident_open) {
            incident_.active = true;
            incident_.id = "inc-" + std::to_string(incident_.next_incident_seq++);
            incident_.opened_unix_ms = now_ms;
            incident_.first_side = side;
            incident_.first_reason_class = reason.reason_class;
            incident_.first_reason_code = reason.code;
            incident_.input_failures = 0;
            incident_.output_failures = 0;
            incident_.input_timeouts = 0;
            incident_.output_timeouts = 0;
            metrics_->incident_active.store(1, std::memory_order_relaxed);
            metrics_->incident_open_unix_ms.store(now_ms, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lock(metrics_->causality_mutex);
                metrics_->active_incident_id = incident_.id;
            }
            logger.Log(LogLevel::kWarn,
                       "incident-open",
                       std::string("incident_id=") + incident_.id,
                       std::string("first_fault_side=") + FailureSideName(side),
                       std::string("first_reason_code=") + ReasonCodeName(reason.code),
                       std::string("first_reason_class=") + ReasonClassName(reason.reason_class),
                       "first_reason_detail=" + reason_detail,
                       "first_source=" + source,
                       "first_socket_id=" + std::to_string(socket_id),
                       "first_attempt_id=" + std::to_string(attempt_id));
        }

        if (side == FailureSide::kInput) {
            incident_.input_failures += 1;
            metrics_->last_input_failure_unix_seconds.store(now_seconds, std::memory_order_relaxed);
        } else {
            incident_.output_failures += 1;
            metrics_->last_output_failure_unix_seconds.store(now_seconds, std::memory_order_relaxed);
        }
        if (reason.reason_class == ReasonClass::kTimeout) {
            if (side == FailureSide::kInput) {
                incident_.input_timeouts += 1;
            } else {
                incident_.output_timeouts += 1;
            }
        }
        const size_t side_index = SideIndex(side);
        metrics_->disconnects_total[side_index][ReasonCodeIndex(reason.code)].fetch_add(1, std::memory_order_relaxed);
        if (reason.has_timeout_type) {
            metrics_->timeouts_total[side_index][TimeoutTypeIndex(reason.timeout_type)].fetch_add(1, std::memory_order_relaxed);
        }

        LastFailureSnapshot snapshot;
        snapshot.timestamp_unix_seconds = now_seconds;
        snapshot.reason_code = ReasonCodeName(reason.code);
        snapshot.reason_class = ReasonClassName(reason.reason_class);
        snapshot.reason_detail = reason_detail;
        snapshot.incident_id = incident_.id;
        snapshot.attempt_id = attempt_id;
        snapshot.source = source;
        {
            std::lock_guard<std::mutex> lock(metrics_->causality_mutex);
            if (side == FailureSide::kInput) {
                metrics_->input_last_failure = snapshot;
            } else {
                metrics_->output_last_failure = snapshot;
            }
        }
        return reason;
    }

    void MaybeCloseIncident(const Logger& logger, bool input_connected, bool output_connected) {
        if (!incident_.active || !input_connected || !output_connected) {
            return;
        }
        if (incident_.first_reason_class == ReasonClass::kTimeout) {
            if (incident_.first_side == FailureSide::kInput) {
                const int64_t last_rx_ms = metrics_->last_rx_unix_ms.load(std::memory_order_relaxed);
                if (last_rx_ms <= incident_.opened_unix_ms) {
                    return;
                }
            } else {
                const int64_t last_tx_ms = metrics_->last_tx_unix_ms.load(std::memory_order_relaxed);
                if (last_tx_ms <= incident_.opened_unix_ms) {
                    return;
                }
            }
        }
        const int64_t now_ms = UnixNowMs();
        logger.Log(LogLevel::kInfo,
                   "incident-close",
                   "incident_id=" + incident_.id,
                   "duration_ms=" + std::to_string(std::max<int64_t>(0, now_ms - incident_.opened_unix_ms)),
                   std::string("first_fault_side=") + FailureSideName(incident_.first_side),
                   std::string("first_reason_class=") + ReasonClassName(incident_.first_reason_class),
                   std::string("first_reason_code=") + ReasonCodeName(incident_.first_reason_code),
                   "input_failures=" + std::to_string(incident_.input_failures),
                   "output_failures=" + std::to_string(incident_.output_failures),
                   "input_timeouts=" + std::to_string(incident_.input_timeouts),
                   "output_timeouts=" + std::to_string(incident_.output_timeouts));
        incident_.active = false;
        incident_.id.clear();
        incident_.opened_unix_ms = 0;
        metrics_->incident_active.store(0, std::memory_order_relaxed);
        metrics_->incident_open_unix_ms.store(0, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(metrics_->causality_mutex);
            metrics_->active_incident_id.clear();
        }
    }

private:
    SideCausalityState& SideState(FailureSide side) {
        return side == FailureSide::kInput ? input_ : output_;
    }

    MetricsState* metrics_ = nullptr;
    SideCausalityState input_;
    SideCausalityState output_;
    IncidentState incident_;
};

int RelayMain(const Config& cfg, const Logger& logger) {
    const std::vector<InputEndpointSpec> input_specs = ParseInputEndpointSpecs(cfg);
    const InputEndpointSpec& input_spec = input_specs.front();
    const OutputEndpointSpec output_spec = ParseOutputEndpointSpec(cfg);

    RelayStats stats;
    RelayState state;
    MetricsState metrics;
    std::vector<char> buffer(static_cast<size_t>(cfg.max_message_size));
    std::vector<char> stdin_pending;
    size_t stdin_pending_offset = 0;
    auto last_stats_at = std::chrono::steady_clock::now();
    const auto startup_ms = UnixNowMs();
    metrics.last_rx_unix_ms.store(startup_ms, std::memory_order_relaxed);
    metrics.last_tx_unix_ms.store(startup_ms, std::memory_order_relaxed);
    metrics.input_sources_total.store(static_cast<int64_t>(input_specs.size()), std::memory_order_relaxed);
    metrics.primary_input_index.store(
        cfg.primary_input_index.has_value() ? static_cast<int64_t>(*cfg.primary_input_index + 1) : 0,
        std::memory_order_relaxed);
    metrics.switch_policy.store(cfg.primary_input_index.has_value() ? 1 : 0, std::memory_order_relaxed);
    metrics.switch_mode.store(cfg.switch_mode == SwitchMode::kDelayed ? 1 : 0, std::memory_order_relaxed);
    for (size_t i = 0; i < MetricsState::kMaxInputSources; ++i) {
        metrics.input_source_connected[i].store(0, std::memory_order_relaxed);
        metrics.input_source_listening[i].store(0, std::memory_order_relaxed);
        metrics.input_source_bond_mode[i].store(0, std::memory_order_relaxed);
    }
    for (size_t i = 0; i < input_specs.size() && i < MetricsState::kMaxInputSources; ++i) {
        const auto group_type = input_specs[i].group_type;
        const int bond_mode = group_type == SRT_GTYPE_BROADCAST ? 1 :
                              (group_type == SRT_GTYPE_BACKUP ? 2 : 0);
        metrics.input_source_bond_mode[i].store(bond_mode, std::memory_order_relaxed);
    }

    std::unique_ptr<InputSource> input_source = BuildInputSource(cfg, input_specs);
    std::unique_ptr<OutputSink> output_sink = BuildOutputSink(output_spec);
    const bool stdin_repacketize_enabled = (input_specs.size() == 1 &&
                                            input_spec.kind == InputEndpointKind::kStdin);
    int stdin_chunk_size = cfg.max_message_size;
    if (stdin_repacketize_enabled) {
        constexpr int kTsPacketSize = 188;
        const int ts_aligned = (cfg.max_message_size / kTsPacketSize) * kTsPacketSize;
        if (ts_aligned > 0) {
            stdin_chunk_size = ts_aligned;
        }
        logger.Log(LogLevel::kInfo,
                   "stdin-repacketizer-enabled",
                   "chunk_size=" + std::to_string(stdin_chunk_size),
                   "max_message_size=" + std::to_string(cfg.max_message_size));
    }
    MetricsServer metrics_server(cfg, logger, metrics, input_specs, output_spec);
    metrics_server.Start();

    std::ostringstream input_uris_stream;
    for (size_t i = 0; i < cfg.input_uris.size(); ++i) {
        if (i > 0) {
            input_uris_stream << ",";
        }
        input_uris_stream << cfg.input_uris[i];
    }

    logger.Log(LogLevel::kInfo, "startup",
               "input_uris=" + input_uris_stream.str(),
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
               "metrics_port=" + std::to_string(cfg.metrics_port),
               "switch_policy=" + std::string(InputSwitchPolicyName(
                   cfg.primary_input_index.has_value() ? InputSwitchPolicy::kPreferredPrimary
                                                       : InputSwitchPolicy::kRoundRobin)),
               "switch_mode=" + std::string(SwitchModeName(cfg.switch_mode)));

    CausalityTracker causality(&metrics);
    auto active_input_kind = [&]() -> InputEndpointKind {
        const size_t active_index = input_source->ActiveInputIndex();
        if (active_index < input_specs.size()) {
            return input_specs[active_index].kind;
        }
        return input_specs.front().kind;
    };
    while (!g_shutdown_requested.load()) {
        for (size_t i = 0; i < MetricsState::kMaxInputSources; ++i) {
            metrics.input_source_connected[i].store(0, std::memory_order_relaxed);
            metrics.input_source_listening[i].store(0, std::memory_order_relaxed);
        }
        const size_t active_index_for_state = input_source->ActiveInputIndex();
        if (active_index_for_state < MetricsState::kMaxInputSources) {
            metrics.input_source_connected[active_index_for_state].store(
                input_source->IsConnected() ? 1 : 0, std::memory_order_relaxed);
            metrics.input_source_listening[active_index_for_state].store(
                input_source->IsListening() ? 1 : 0, std::memory_order_relaxed);
        }
        state.input_listening = input_source->IsListening();
        state.input_connected = input_source->IsConnected();
        state.output_connected = output_sink->IsConnected();
        metrics.input_listening.store(state.input_listening ? 1 : 0, std::memory_order_relaxed);
        metrics.input_connected.store(state.input_connected ? 1 : 0, std::memory_order_relaxed);
        metrics.output_connected.store(state.output_connected ? 1 : 0, std::memory_order_relaxed);
        metrics.input_session_socket_id.store(static_cast<int64_t>(input_source->SessionSocket()),
                                              std::memory_order_relaxed);
        const SRTSOCKET output_transport_sock =
            (output_sink->MetricsMode() == OutputMetricsMode::kSrtSocket)
                ? output_sink->TransportSocket()
                : SRT_INVALID_SOCK;
        metrics.output_transport_socket_id.store(static_cast<int64_t>(output_transport_sock),
                                                 std::memory_order_relaxed);
        MaybeLogStats(cfg, logger, &stats, state, &metrics,
                      input_source->SessionSocket(), output_sink->TransportSocket(),
                      output_sink->MetricsMode(), causality.ActiveIncidentId(), &last_stats_at);
        metrics.active_input_index.store(static_cast<int64_t>(input_source->ActiveInputIndex() + 1),
                                         std::memory_order_relaxed);
        metrics.input_switches_total.store(input_source->InputSwitchCount(), std::memory_order_relaxed);
        causality.MaybeCloseIncident(logger, input_source->IsConnected(), output_sink->IsConnected());

        const bool input_reconnect_needed = !input_source->IsConnected();
        if (input_source->NeedsEnsurePoll()) {
            const uint64_t attempt_id = input_reconnect_needed
                ? causality.EnsureAttemptStarted(FailureSide::kInput)
                : 0;
            if (input_reconnect_needed) {
                logger.Log(LogLevel::kInfo,
                           "input-ensure-attempt",
                           "side=input",
                           "attempt_id=" + std::to_string(attempt_id),
                           "incident_id=" + (causality.ActiveIncidentId().empty() ? std::string("none") : causality.ActiveIncidentId()));
            }
            try {
                input_source->EnsureReady(
                    cfg, logger, &metrics, EnsureAttemptContext{attempt_id, causality.ActiveIncidentId()});
                if (input_reconnect_needed) {
                    causality.CompleteAttempt(FailureSide::kInput);
                }
            } catch (const std::exception& ex) {
                if (!input_reconnect_needed && input_source->IsConnected()) {
                    continue;
                }
                const bool is_timeout = input_source->LastEnsureErrorKind() == IoErrorKind::kTimeout;
                const std::string message = input_source->LastEnsureErrorMessage().empty()
                                                ? std::string(ex.what())
                                                : input_source->LastEnsureErrorMessage();
                const ReasonDescriptor reason = causality.RecordFailure(logger,
                                                                        FailureSide::kInput,
                                                                        FailureOperation::kEnsure,
                                                                        is_timeout,
                                                                        false,
                                                                        active_input_kind() == InputEndpointKind::kSrtListener ||
                                                                            active_input_kind() == InputEndpointKind::kUdpListener,
                                                                        message,
                                                                        "input.ensure",
                                                                        attempt_id,
                                                                        static_cast<int64_t>(input_source->SessionSocket()));
                if (input_source->LastEnsureErrorKind() == IoErrorKind::kTimeout) {
                    logger.Log(LogLevel::kDebug,
                               "input-ensure-timeout",
                               "side=input",
                               "reason_code=" + std::string(ReasonCodeName(reason.code)),
                               "reason_class=" + std::string(ReasonClassName(reason.reason_class)),
                               "reason_detail=" + message,
                               "attempt_id=" + std::to_string(attempt_id),
                               "incident_id=" + causality.ActiveIncidentId());
                    continue;
                }
                logger.Log(LogLevel::kWarn,
                           "input-ensure-failed",
                           "side=input",
                           "reason_code=" + std::string(ReasonCodeName(reason.code)),
                           "reason_class=" + std::string(ReasonClassName(reason.reason_class)),
                           "reason_detail=" + message,
                           "attempt_id=" + std::to_string(attempt_id),
                           "incident_id=" + causality.ActiveIncidentId());
                input_source->HandleReceiveError(cfg, logger, &metrics);
                logger.Log(LogLevel::kWarn,
                           "input-reset",
                           "side=input",
                           "attempt_id=" + std::to_string(attempt_id),
                           "incident_id=" + causality.ActiveIncidentId(),
                           "reason_code=" + std::string(ReasonCodeName(reason.code)));
                if (cfg.exit_on_input_failure) return 2;
                SleepReconnectDelay(cfg);
                continue;
            }
        }

        if (!output_sink->IsConnected()) {
            const uint64_t attempt_id = causality.EnsureAttemptStarted(FailureSide::kOutput);
            logger.Log(LogLevel::kInfo,
                       "output-ensure-attempt",
                       "side=output",
                       "attempt_id=" + std::to_string(attempt_id),
                       "incident_id=" + (causality.ActiveIncidentId().empty() ? std::string("none") : causality.ActiveIncidentId()));
            try {
                output_sink->EnsureReady(
                    cfg, logger, &stats, &metrics, EnsureAttemptContext{attempt_id, causality.ActiveIncidentId()});
                causality.CompleteAttempt(FailureSide::kOutput);
            } catch (const std::exception& ex) {
                const bool is_timeout = output_sink->LastEnsureErrorKind() == IoErrorKind::kTimeout;
                const std::string message = output_sink->LastEnsureErrorMessage().empty()
                                                ? std::string(ex.what())
                                                : output_sink->LastEnsureErrorMessage();
                const ReasonDescriptor reason = causality.RecordFailure(logger,
                                                                        FailureSide::kOutput,
                                                                        FailureOperation::kEnsure,
                                                                        is_timeout,
                                                                        false,
                                                                        output_spec.kind == OutputEndpointKind::kSrtListener ||
                                                                            output_spec.kind == OutputEndpointKind::kUdpListener,
                                                                        message,
                                                                        "output.ensure",
                                                                        attempt_id,
                                                                        static_cast<int64_t>(output_sink->TransportSocket()));
                if (output_sink->LastEnsureErrorKind() == IoErrorKind::kTimeout) {
                    logger.Log(LogLevel::kDebug,
                               "output-ensure-timeout",
                               "side=output",
                               "reason_code=" + std::string(ReasonCodeName(reason.code)),
                               "reason_class=" + std::string(ReasonClassName(reason.reason_class)),
                               "reason_detail=" + message,
                               "attempt_id=" + std::to_string(attempt_id),
                               "incident_id=" + causality.ActiveIncidentId());
                    continue;
                }
                logger.Log(LogLevel::kWarn, "output-connect-failed",
                           "side=output",
                           "reason_code=" + std::string(ReasonCodeName(reason.code)),
                           "reason_class=" + std::string(ReasonClassName(reason.reason_class)),
                           "reason_detail=" + message,
                           "attempt_id=" + std::to_string(attempt_id),
                           "incident_id=" + causality.ActiveIncidentId(),
                           "reconnect_count=" + std::to_string(stats.reconnect_count));
                output_sink->MarkDisconnected(&metrics);
                logger.Log(LogLevel::kWarn,
                           "output-reset",
                           "side=output",
                           "attempt_id=" + std::to_string(attempt_id),
                           "incident_id=" + causality.ActiveIncidentId(),
                           "reason_code=" + std::string(ReasonCodeName(reason.code)));
                if (cfg.exit_on_output_failure) return 3;
                SleepReconnectDelay(cfg);
                continue;
            }
        }

        SRT_MSGCTRL rx_ctrl = srt_msgctrl_default;
        SRT_SOCKGROUPDATA rx_group_data[16] {};
        rx_ctrl.grpdata = rx_group_data;
        rx_ctrl.grpdata_size = sizeof(rx_group_data) / sizeof(rx_group_data[0]);
        const InputReceiveResult receive_result = input_source->Receive(cfg, &buffer, &rx_ctrl);
        if (receive_result.status == InputReceiveStatus::kError) {
            if (input_source->IsTerminalEof()) {
                const uint64_t attempt_id = causality.EnsureAttemptStarted(FailureSide::kInput);
                const ReasonDescriptor reason = causality.RecordFailure(logger,
                                                                        FailureSide::kInput,
                                                                        FailureOperation::kProtocol,
                                                                        false,
                                                                        false,
                                                                        false,
                                                                        "stdin EOF",
                                                                        "input.protocol",
                                                                        attempt_id,
                                                                        static_cast<int64_t>(input_source->SessionSocket()));
                logger.Log(LogLevel::kInfo,
                           "input-eof",
                           "side=input",
                           "reason_code=" + std::string(ReasonCodeName(reason.code)),
                           "reason_class=" + std::string(ReasonClassName(reason.reason_class)),
                           "reason_detail=stdin EOF",
                           "attempt_id=" + std::to_string(attempt_id),
                           "incident_id=" + causality.ActiveIncidentId());
                break;
            }
            const uint64_t attempt_id = causality.EnsureAttemptStarted(FailureSide::kInput);
            const bool is_timeout = input_source->LastReceiveErrorKind() == IoErrorKind::kTimeout;
            const bool is_disconnected = input_source->LastReceiveErrorKind() == IoErrorKind::kDisconnected;
            std::string err = input_source->LastReceiveErrorMessage();
            if (err.empty()) err = "input receive failed";
            const ReasonDescriptor reason = causality.RecordFailure(logger,
                                                                    FailureSide::kInput,
                                                                    FailureOperation::kReceive,
                                                                    is_timeout,
                                                                    is_disconnected,
                                                                    false,
                                                                    err,
                                                                    "input.receive",
                                                                    attempt_id,
                                                                    static_cast<int64_t>(input_source->SessionSocket()));
            if (is_timeout) {
                logger.Log(LogLevel::kDebug,
                           "input-receive-timeout",
                           "side=input",
                           "reason_code=" + std::string(ReasonCodeName(reason.code)),
                           "reason_class=" + std::string(ReasonClassName(reason.reason_class)),
                           "reason_detail=" + err,
                           "attempt_id=" + std::to_string(attempt_id),
                           "incident_id=" + causality.ActiveIncidentId());
                continue;
            }
            logger.Log(LogLevel::kWarn,
                       "input-disconnected",
                       "side=input",
                       "reason_code=" + std::string(ReasonCodeName(reason.code)),
                       "reason_class=" + std::string(ReasonClassName(reason.reason_class)),
                       "reason_detail=" + err,
                       "attempt_id=" + std::to_string(attempt_id),
                       "incident_id=" + causality.ActiveIncidentId());
            input_source->HandleReceiveError(cfg, logger, &metrics);
            logger.Log(LogLevel::kWarn,
                       "input-reset",
                       "side=input",
                       "reason_code=" + std::string(ReasonCodeName(reason.code)),
                       "attempt_id=" + std::to_string(attempt_id),
                       "incident_id=" + causality.ActiveIncidentId());
            if (cfg.exit_on_input_failure) return 2;
            continue;
        }

        const int recv_size = receive_result.bytes;
        stats.total_rx_bytes += static_cast<uint64_t>(recv_size);
        stats.total_rx_msgs += 1;
        stats.interval_rx_bytes += static_cast<uint64_t>(recv_size);
        stats.interval_rx_msgs += 1;
        causality.NoteDataSuccess(FailureSide::kInput);
        if (IsSrtInputKind(active_input_kind())) {
            UpdateInputLinkHealthFromMsgCtrl(rx_ctrl, &metrics);
        }
        metrics.total_rx_bytes.store(stats.total_rx_bytes, std::memory_order_relaxed);
        metrics.total_rx_msgs.store(stats.total_rx_msgs, std::memory_order_relaxed);
        metrics.last_rx_unix_ms.store(UnixNowMs(), std::memory_order_relaxed);
        auto send_chunk = [&](const char* data, int size) -> bool {
            const OutputSendResult send_result = output_sink->Send(data, size);
            if (send_result.status == OutputSendStatus::kError) {
                stats.total_send_failures += 1;
                stats.interval_send_failures += 1;
                stats.reconnect_count += 1;
                metrics.total_send_failures.store(stats.total_send_failures, std::memory_order_relaxed);
                metrics.reconnect_count.store(stats.reconnect_count, std::memory_order_relaxed);

                const uint64_t attempt_id = causality.EnsureAttemptStarted(FailureSide::kOutput);
                std::string err = output_sink->LastSendErrorMessage();
                if (err.empty()) err = "output send failed";
                const ReasonDescriptor reason = causality.RecordFailure(logger,
                                                                        FailureSide::kOutput,
                                                                        FailureOperation::kSend,
                                                                        output_sink->LastSendErrorKind() == IoErrorKind::kTimeout,
                                                                        output_sink->LastSendErrorKind() == IoErrorKind::kDisconnected,
                                                                        false,
                                                                        err,
                                                                        "output.send",
                                                                        attempt_id,
                                                                        static_cast<int64_t>(output_sink->TransportSocket()));
                logger.Log(LogLevel::kWarn, "output-send-failed",
                           "side=output",
                           "reason_code=" + std::string(ReasonCodeName(reason.code)),
                           "reason_class=" + std::string(ReasonClassName(reason.reason_class)),
                           "reason_detail=" + err,
                           "attempt_id=" + std::to_string(attempt_id),
                           "incident_id=" + causality.ActiveIncidentId(),
                           "reconnect_count=" + std::to_string(stats.reconnect_count));
                output_sink->MarkDisconnected(&metrics);
                logger.Log(LogLevel::kWarn,
                           "output-reset",
                           "side=output",
                           "reason_code=" + std::string(ReasonCodeName(reason.code)),
                           "attempt_id=" + std::to_string(attempt_id),
                           "incident_id=" + causality.ActiveIncidentId());
                if (cfg.exit_on_output_failure) {
                    g_shutdown_requested.store(true);
                } else {
                    SleepReconnectDelay(cfg);
                }
                return false;
            }

            const int sent_size = send_result.bytes;
            stats.total_tx_bytes += static_cast<uint64_t>(sent_size);
            stats.total_tx_msgs += 1;
            stats.interval_tx_bytes += static_cast<uint64_t>(sent_size);
            stats.interval_tx_msgs += 1;
            causality.NoteDataSuccess(FailureSide::kOutput);
            metrics.total_tx_bytes.store(stats.total_tx_bytes, std::memory_order_relaxed);
            metrics.total_tx_msgs.store(stats.total_tx_msgs, std::memory_order_relaxed);
            metrics.last_tx_unix_ms.store(UnixNowMs(), std::memory_order_relaxed);
            return true;
        };

        if (!stdin_repacketize_enabled) {
            if (!send_chunk(buffer.data(), recv_size)) {
                if (cfg.exit_on_output_failure) return 3;
                continue;
            }
            continue;
        }

        stdin_pending.insert(stdin_pending.end(), buffer.begin(), buffer.begin() + recv_size);
        while ((stdin_pending.size() - stdin_pending_offset) >= static_cast<size_t>(stdin_chunk_size)) {
            if (!send_chunk(stdin_pending.data() + stdin_pending_offset, stdin_chunk_size)) {
                if (cfg.exit_on_output_failure) return 3;
                break;
            }
            stdin_pending_offset += static_cast<size_t>(stdin_chunk_size);
            if (stdin_pending_offset == stdin_pending.size()) {
                stdin_pending.clear();
                stdin_pending_offset = 0;
            } else if (stdin_pending_offset > (stdin_pending.size() / 2)) {
                stdin_pending.erase(stdin_pending.begin(), stdin_pending.begin() + static_cast<std::ptrdiff_t>(stdin_pending_offset));
                stdin_pending_offset = 0;
            }
        }
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
