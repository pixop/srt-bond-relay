#include "srtrelay/relay_io.hpp"

#include <unistd.h>

#include <stdexcept>
#include <string>

#include "relay_io_internal.hpp"

namespace srtrelay {

namespace {

class SrtInputListenerSource : public InputSource {
public:
    explicit SrtInputListenerSource(std::vector<SrtUri> uris) : uris_(std::move(uris)) {}
    ~SrtInputListenerSource() override { detail::CloseSocketList(&listeners_); }

    void EnsureReady(const Config& cfg, const Logger& logger, MetricsState* metrics) override {
        if (listeners_.empty()) {
            listeners_.reserve(uris_.size());
            for (const auto& uri : uris_) {
                listeners_.push_back(detail::CreateListeningSocket(uri, cfg, logger, "input"));
            }
            metrics->input_listening.store(1, std::memory_order_relaxed);
        }
        if (!session_.Valid()) {
            SRTSOCKET accepted = detail::AcceptBondSession(listeners_, cfg, "input");
            ApplyIntSockOpt(accepted, SRTO_RCVTIMEO, cfg.io_timeout_ms, "SRTO_RCVTIMEO");
            session_.Set(accepted);
            metrics->input_connected.store(1, std::memory_order_relaxed);
            logger.Log(LogLevel::kInfo, "input-connected", "socket=" + std::to_string(accepted));
        }
    }

    int Receive(const Config& cfg, std::vector<char>* buffer, SRT_MSGCTRL* rx_ctrl) override {
        return srt_recvmsg2(session_.Get(), buffer->data(), cfg.max_message_size, rx_ctrl);
    }

    void HandleReceiveError(const Config&, const Logger&, MetricsState* metrics) override {
        metrics->input_connected.store(0, std::memory_order_relaxed);
        metrics->input_listening.store(0, std::memory_order_relaxed);
        ResetInputTrackingMetrics(metrics);
        session_.Reset();
        detail::CloseSocketList(&listeners_);
    }

    SRTSOCKET SessionSocket() const override { return session_.Get(); }
    bool IsListening() const override { return !listeners_.empty(); }
    bool IsConnected() const override { return session_.Valid(); }
    bool IsTerminalEof() const override { return false; }

private:
    std::vector<SrtUri> uris_;
    std::vector<SRTSOCKET> listeners_;
    SrtSocketHolder session_;
};

class SrtInputCallerSource : public InputSource {
public:
    SrtInputCallerSource(std::vector<SrtUri> uris, bool bonded, SRT_GROUP_TYPE group_type)
        : uris_(std::move(uris)), bonded_(bonded), group_type_(group_type) {}

    void EnsureReady(const Config& cfg, const Logger& logger, MetricsState* metrics) override {
        if (session_.Valid()) {
            return;
        }
        SRTSOCKET sock = SRT_INVALID_SOCK;
        if (bonded_) {
            sock = detail::ConnectBondedCallerGroup(uris_, group_type_, cfg, logger, false);
        } else {
            sock = detail::ConnectSingleCallerSocket(uris_.front(), cfg, logger, false);
        }
        session_.Set(sock);
        metrics->input_listening.store(0, std::memory_order_relaxed);
        metrics->input_connected.store(1, std::memory_order_relaxed);
        logger.Log(LogLevel::kInfo, "input-connected",
                   std::string("mode=caller"),
                   "bonded=" + std::string(bonded_ ? "true" : "false"),
                   "members=" + std::to_string(uris_.size()),
                   "socket=" + std::to_string(sock));
    }

    int Receive(const Config& cfg, std::vector<char>* buffer, SRT_MSGCTRL* rx_ctrl) override {
        return srt_recvmsg2(session_.Get(), buffer->data(), cfg.max_message_size, rx_ctrl);
    }

    void HandleReceiveError(const Config&, const Logger&, MetricsState* metrics) override {
        metrics->input_connected.store(0, std::memory_order_relaxed);
        metrics->input_listening.store(0, std::memory_order_relaxed);
        ResetInputTrackingMetrics(metrics);
        session_.Reset();
    }

    SRTSOCKET SessionSocket() const override { return session_.Get(); }
    bool IsListening() const override { return false; }
    bool IsConnected() const override { return session_.Valid(); }
    bool IsTerminalEof() const override { return false; }

private:
    std::vector<SrtUri> uris_;
    bool bonded_ = false;
    SRT_GROUP_TYPE group_type_ = SRT_GTYPE_UNDEFINED;
    SrtSocketHolder session_;
};

class StdinInputSource : public InputSource {
public:
    void EnsureReady(const Config&, const Logger&, MetricsState* metrics) override {
        if (eof_) {
            throw std::runtime_error("stdin reached EOF");
        }
        metrics->input_listening.store(0, std::memory_order_relaxed);
        metrics->input_connected.store(1, std::memory_order_relaxed);
    }

    int Receive(const Config& cfg, std::vector<char>* buffer, SRT_MSGCTRL* rx_ctrl) override {
        (void)rx_ctrl;
        const ssize_t n = ::read(STDIN_FILENO, buffer->data(), static_cast<size_t>(cfg.max_message_size));
        if (n > 0) return static_cast<int>(n);
        if (n == 0) {
            eof_ = true;
            return SRT_ERROR;
        }
        return SRT_ERROR;
    }

    void HandleReceiveError(const Config&, const Logger&, MetricsState* metrics) override {
        metrics->input_connected.store(0, std::memory_order_relaxed);
        metrics->input_listening.store(0, std::memory_order_relaxed);
        ResetInputTrackingMetrics(metrics);
    }

    SRTSOCKET SessionSocket() const override { return SRT_INVALID_SOCK; }
    bool IsListening() const override { return false; }
    bool IsConnected() const override { return !eof_; }
    bool IsTerminalEof() const override { return eof_; }

private:
    bool eof_ = false;
};

}  // namespace

std::unique_ptr<InputSource> BuildInputSource(const InputEndpointSpec& spec) {
    if (spec.kind == InputEndpointKind::kStdin) {
        return std::make_unique<StdinInputSource>();
    }
    if (spec.kind == InputEndpointKind::kSrtCaller) {
        return std::make_unique<SrtInputCallerSource>(spec.uris, spec.bonded, spec.group_type);
    }
    return std::make_unique<SrtInputListenerSource>(spec.uris);
}

}  // namespace srtrelay
