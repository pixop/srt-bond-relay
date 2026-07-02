#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <srt.h>

#include "srtrelay/config.hpp"
#include "srtrelay/logger.hpp"
#include "srtrelay/metrics.hpp"
#include "srtrelay/srt_utils.hpp"

namespace srtrelay {

enum class InputEndpointKind {
    kSrtListener,
    kSrtCaller,
    kUdpListener,
    kUdpCaller,
    kStdin,
};

enum class OutputEndpointKind {
    kSrtCaller,
    kSrtListener,
    kUdpCaller,
    kUdpListener,
    kStdout,
};

enum class IoErrorKind {
    kNone,
    kTimeout,
    kDisconnected,
    kError,
};

enum class InputReceiveStatus {
    kData,
    kError,
};

struct InputReceiveResult {
    InputReceiveStatus status = InputReceiveStatus::kError;
    int bytes = 0;
};

enum class OutputSendStatus {
    kSent,
    kError,
};

struct OutputSendResult {
    OutputSendStatus status = OutputSendStatus::kError;
    int bytes = 0;
};

enum class InputSwitchPolicy {
    kRoundRobin,
    kPreferredPrimary,
};

struct InputEndpointSpec {
    InputEndpointKind kind = InputEndpointKind::kSrtListener;
    std::vector<SrtUri> uris;
    UdpUri udp_uri;
    bool bonded = false;
    SRT_GROUP_TYPE group_type = SRT_GTYPE_UNDEFINED;
};

struct OutputEndpointSpec {
    OutputEndpointKind kind = OutputEndpointKind::kSrtCaller;
    std::vector<SrtUri> uris;
    UdpUri udp_uri;
    bool bonded = false;
    SRT_GROUP_TYPE group_type = SRT_GTYPE_UNDEFINED;
    bool listener_fanout_enabled = false;
    int listener_max_clients = 1;
};

struct EnsureAttemptContext {
    uint64_t attempt_id = 0;
    std::string incident_id;
    std::optional<size_t> source_index;  // 1-based input/output index for fan-out/switching contexts
};

class InputSource {
public:
    virtual ~InputSource() = default;
    virtual void EnsureReady(const Config& cfg,
                             const Logger& logger,
                             MetricsState* metrics,
                             const EnsureAttemptContext& attempt_ctx) = 0;
    virtual InputReceiveResult Receive(const Config& cfg, std::vector<char>* buffer, SRT_MSGCTRL* rx_ctrl) = 0;
    virtual void HandleReceiveError(const Config& cfg, const Logger& logger, MetricsState* metrics) = 0;
    virtual SRTSOCKET SessionSocket() const = 0;
    virtual bool IsListening() const = 0;
    virtual bool IsConnected() const = 0;
    virtual bool IsTerminalEof() const = 0;
    virtual bool NeedsEnsurePoll() const { return !IsConnected(); }
    virtual IoErrorKind LastReceiveErrorKind() const = 0;
    virtual std::string LastReceiveErrorMessage() const = 0;
    virtual IoErrorKind LastEnsureErrorKind() const = 0;
    virtual std::string LastEnsureErrorMessage() const = 0;
    virtual size_t ActiveInputIndex() const { return 0; }  // zero-based
    virtual uint64_t InputSwitchCount() const { return 0; }
};

class OutputSink {
public:
    virtual ~OutputSink() = default;
    virtual void EnsureReady(const Config& cfg,
                             const Logger& logger,
                             RelayStats* stats,
                             MetricsState* metrics,
                             const EnsureAttemptContext& attempt_ctx) = 0;
    virtual OutputSendResult Send(const char* data, int size) = 0;
    virtual void MarkDisconnected(MetricsState* metrics) = 0;
    virtual SRTSOCKET TransportSocket() const = 0;
    virtual OutputMetricsMode MetricsMode() const = 0;
    virtual bool IsConnected() const = 0;
    virtual bool IsListening() const { return false; }
    virtual bool NeedsEnsurePoll() const { return !IsConnected(); }
    virtual IoErrorKind LastSendErrorKind() const = 0;
    virtual std::string LastSendErrorMessage() const = 0;
    virtual IoErrorKind LastEnsureErrorKind() const = 0;
    virtual std::string LastEnsureErrorMessage() const = 0;
    virtual size_t OutputCount() const { return 1; }
    virtual bool OutputConnected(size_t index) const { return index == 0 && IsConnected(); }
    virtual bool OutputListening(size_t index) const { return index == 0 && IsListening(); }
    virtual int64_t OutputListenerClientsActive(size_t) const { return 0; }
    virtual uint64_t OutputListenerClientsAcceptedTotal(size_t) const { return 0; }
    virtual uint64_t OutputListenerClientsDroppedTimeoutTotal(size_t) const { return 0; }
    virtual uint64_t OutputListenerClientsDroppedDisconnectedTotal(size_t) const { return 0; }
    virtual uint64_t OutputListenerClientsDroppedErrorTotal(size_t) const { return 0; }
    virtual uint64_t OutputListenerAcceptRejectedMaxClientsTotal(size_t) const { return 0; }
    virtual std::vector<std::string> OutputListenerConnectedEndpoints(size_t) const { return {}; }
};

InputEndpointSpec ParseInputEndpointSpec(const Config& cfg);
std::vector<InputEndpointSpec> ParseInputEndpointSpecs(const Config& cfg);
OutputEndpointSpec ParseOutputEndpointSpec(const Config& cfg);
std::vector<OutputEndpointSpec> ParseOutputEndpointSpecs(const Config& cfg);
std::unique_ptr<InputSource> BuildInputSource(const InputEndpointSpec& spec);
std::unique_ptr<InputSource> BuildInputSource(const Config& cfg, std::vector<InputEndpointSpec> specs);
const char* InputSwitchPolicyName(InputSwitchPolicy policy);
std::unique_ptr<OutputSink> BuildOutputSink(const OutputEndpointSpec& spec);
std::unique_ptr<OutputSink> BuildOutputSink(const Config& cfg, std::vector<OutputEndpointSpec> specs);

}  // namespace srtrelay
