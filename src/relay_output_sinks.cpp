#include "srtrelay/relay_io.hpp"

#include <unistd.h>

#include <stdexcept>
#include <string>

#include "relay_io_internal.hpp"

namespace srtrelay {

namespace {

class SrtOutputCallerSink : public OutputSink {
public:
    SrtOutputCallerSink(std::vector<SrtUri> uris, bool bonded, SRT_GROUP_TYPE group_type)
        : uris_(std::move(uris)), bonded_(bonded), group_type_(group_type) {}

    void EnsureReady(const Config& cfg, const Logger& logger, RelayStats* stats, MetricsState* metrics) override {
        if (socket_.Valid()) return;
        try {
            SRTSOCKET sock = SRT_INVALID_SOCK;
            if (bonded_) {
                sock = detail::ConnectBondedCallerGroup(uris_, group_type_, cfg, logger, true);
            } else {
                sock = detail::ConnectSingleCallerSocket(uris_.front(), cfg, logger, true);
            }
            socket_.Set(sock);
            metrics->output_connected.store(1, std::memory_order_relaxed);
            logger.Log(LogLevel::kInfo, "output-connected",
                       std::string("mode=caller"),
                       "bonded=" + std::string(bonded_ ? "true" : "false"),
                       "members=" + std::to_string(uris_.size()),
                       "socket=" + std::to_string(sock));
        } catch (...) {
            stats->reconnect_count++;
            metrics->reconnect_count.store(stats->reconnect_count, std::memory_order_relaxed);
            metrics->output_connected.store(0, std::memory_order_relaxed);
            throw;
        }
    }

    int Send(const char* data, int size) override {
        SRT_MSGCTRL tx_ctrl = srt_msgctrl_default;
        return srt_sendmsg2(socket_.Get(), data, size, &tx_ctrl);
    }

    void MarkDisconnected(MetricsState* metrics) override {
        socket_.Reset();
        metrics->output_connected.store(0, std::memory_order_relaxed);
    }

    SRTSOCKET TransportSocket() const override { return socket_.Get(); }
    OutputMetricsMode MetricsMode() const override { return OutputMetricsMode::kSrtSocket; }
    bool IsConnected() const override { return socket_.Valid(); }

private:
    std::vector<SrtUri> uris_;
    bool bonded_ = false;
    SRT_GROUP_TYPE group_type_ = SRT_GTYPE_UNDEFINED;
    SrtSocketHolder socket_;
};

class SrtOutputListenerSink : public OutputSink {
public:
    explicit SrtOutputListenerSink(std::vector<SrtUri> uris) : uris_(std::move(uris)) {}
    ~SrtOutputListenerSink() override { detail::CloseSocketList(&listeners_); }

    void EnsureReady(const Config& cfg, const Logger& logger, RelayStats*, MetricsState* metrics) override {
        if (listeners_.empty()) {
            listeners_.reserve(uris_.size());
            for (const auto& uri : uris_) {
                listeners_.push_back(detail::CreateListeningSocket(uri, cfg, logger, "output"));
            }
        }
        if (!session_.Valid()) {
            const SRTSOCKET accepted = detail::AcceptBondSession(listeners_, cfg, "output");
            ApplyIntSockOpt(accepted, SRTO_SNDTIMEO, cfg.io_timeout_ms, "SRTO_SNDTIMEO");
            session_.Set(accepted);
            logger.Log(LogLevel::kInfo, "output-client-connected",
                       "socket=" + std::to_string(accepted));
        }
        metrics->output_connected.store(session_.Valid() ? 1 : 0, std::memory_order_relaxed);
    }

    int Send(const char* data, int size) override {
        SRT_MSGCTRL tx_ctrl = srt_msgctrl_default;
        return srt_sendmsg2(session_.Get(), data, size, &tx_ctrl);
    }

    void MarkDisconnected(MetricsState* metrics) override {
        session_.Reset();
        metrics->output_connected.store(0, std::memory_order_relaxed);
    }

    SRTSOCKET TransportSocket() const override { return session_.Get(); }
    OutputMetricsMode MetricsMode() const override { return OutputMetricsMode::kSrtSocket; }
    bool IsConnected() const override { return session_.Valid(); }

private:
    std::vector<SrtUri> uris_;
    std::vector<SRTSOCKET> listeners_;
    SrtSocketHolder session_;
};

class StdoutOutputSink : public OutputSink {
public:
    void EnsureReady(const Config&, const Logger&, RelayStats*, MetricsState* metrics) override {
        if (!healthy_) {
            throw std::runtime_error("stdout sink is not writable");
        }
        metrics->output_connected.store(1, std::memory_order_relaxed);
    }

    int Send(const char* data, int size) override {
        int total = 0;
        while (total < size) {
            const ssize_t written = ::write(STDOUT_FILENO, data + total, static_cast<size_t>(size - total));
            if (written < 0) {
                return SRT_ERROR;
            }
            total += static_cast<int>(written);
        }
        return total;
    }

    void MarkDisconnected(MetricsState* metrics) override {
        healthy_ = false;
        metrics->output_connected.store(0, std::memory_order_relaxed);
    }

    SRTSOCKET TransportSocket() const override { return SRT_INVALID_SOCK; }
    OutputMetricsMode MetricsMode() const override { return OutputMetricsMode::kStdout; }
    bool IsConnected() const override { return healthy_; }

private:
    bool healthy_ = true;
};

}  // namespace

std::unique_ptr<OutputSink> BuildOutputSink(const OutputEndpointSpec& spec) {
    if (spec.kind == OutputEndpointKind::kStdout) {
        return std::make_unique<StdoutOutputSink>();
    }
    if (spec.kind == OutputEndpointKind::kSrtListener) {
        return std::make_unique<SrtOutputListenerSink>(spec.uris);
    }
    return std::make_unique<SrtOutputCallerSink>(spec.uris, spec.bonded, spec.group_type);
}

}  // namespace srtrelay
