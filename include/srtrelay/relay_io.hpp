#pragma once

#include <memory>
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
    kStdin,
};

enum class OutputEndpointKind {
    kSrtCaller,
    kSrtListener,
    kStdout,
};

struct InputEndpointSpec {
    InputEndpointKind kind = InputEndpointKind::kSrtListener;
    std::vector<SrtUri> uris;
    bool bonded = false;
    SRT_GROUP_TYPE group_type = SRT_GTYPE_UNDEFINED;
};

struct OutputEndpointSpec {
    OutputEndpointKind kind = OutputEndpointKind::kSrtCaller;
    std::vector<SrtUri> uris;
    bool bonded = false;
    SRT_GROUP_TYPE group_type = SRT_GTYPE_UNDEFINED;
};

class InputSource {
public:
    virtual ~InputSource() = default;
    virtual void EnsureReady(const Config& cfg, const Logger& logger, MetricsState* metrics) = 0;
    virtual int Receive(const Config& cfg, std::vector<char>* buffer, SRT_MSGCTRL* rx_ctrl) = 0;
    virtual void HandleReceiveError(const Config& cfg, const Logger& logger, MetricsState* metrics) = 0;
    virtual SRTSOCKET SessionSocket() const = 0;
    virtual bool IsListening() const = 0;
    virtual bool IsConnected() const = 0;
    virtual bool IsTerminalEof() const = 0;
};

class OutputSink {
public:
    virtual ~OutputSink() = default;
    virtual void EnsureReady(const Config& cfg, const Logger& logger, RelayStats* stats, MetricsState* metrics) = 0;
    virtual int Send(const char* data, int size) = 0;
    virtual void MarkDisconnected(MetricsState* metrics) = 0;
    virtual SRTSOCKET TransportSocket() const = 0;
    virtual OutputMetricsMode MetricsMode() const = 0;
    virtual bool IsConnected() const = 0;
};

InputEndpointSpec ParseInputEndpointSpec(const Config& cfg);
OutputEndpointSpec ParseOutputEndpointSpec(const Config& cfg);
std::unique_ptr<InputSource> BuildInputSource(const InputEndpointSpec& spec);
std::unique_ptr<OutputSink> BuildOutputSink(const OutputEndpointSpec& spec);

}  // namespace srtrelay
