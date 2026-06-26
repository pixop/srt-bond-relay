#include "srtrelay/relay_io.hpp"

#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <stdexcept>
#include <string>

#include "relay_io_internal.hpp"

namespace srtrelay {

namespace {

constexpr int kDefaultUdpTtl = 64;

bool IsUdpTimeoutErrno(int err) {
    return err == EAGAIN || err == EWOULDBLOCK;
}

bool HasMeaningfulSrtError(const std::string& value) {
    return !value.empty() && value != "Success";
}

std::string ComposeSrtEnsureErrorMessage(const std::string& cause) {
    const std::string srt_error = SrtLastErrorString();
    if (!HasMeaningfulSrtError(srt_error)) {
        return cause.empty() ? std::string("unknown error") : cause;
    }
    if (cause.empty()) {
        return srt_error;
    }
    if (cause.find(srt_error) != std::string::npos) {
        return cause;
    }
    return cause + " (srt: " + srt_error + ")";
}

void SetUdpSendTimeoutMs(int fd, int timeout_ms) {
    timeval tv {};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
        throw std::runtime_error("failed to set UDP send timeout: " + std::string(std::strerror(errno)));
    }
}

void ApplyUdpOptionInt(int fd, int level, int name, int value, const char* opt_name) {
    if (::setsockopt(fd, level, name, &value, sizeof(value)) != 0) {
        throw std::runtime_error(std::string("failed to set UDP option ") + opt_name + ": " + std::strerror(errno));
    }
}

void ApplyUdpSocketOptions(int fd, const UdpUri& uri) {
    if (const std::string reuseaddr = QueryString(uri.query, "reuseaddr"); !reuseaddr.empty()) {
        ApplyUdpOptionInt(fd, SOL_SOCKET, SO_REUSEADDR, ParseIntOptionValue(reuseaddr, "reuseaddr"), "SO_REUSEADDR");
    }
    if (const std::string sndbuf = QueryString(uri.query, "sndbuf"); !sndbuf.empty()) {
        ApplyUdpOptionInt(fd, SOL_SOCKET, SO_SNDBUF, ParseIntOptionValue(sndbuf, "sndbuf"), "SO_SNDBUF");
    }
    if (const std::string rcvbuf = QueryString(uri.query, "rcvbuf"); !rcvbuf.empty()) {
        ApplyUdpOptionInt(fd, SOL_SOCKET, SO_RCVBUF, ParseIntOptionValue(rcvbuf, "rcvbuf"), "SO_RCVBUF");
    }
    if (const std::string ttl = QueryString(uri.query, "ttl"); !ttl.empty()) {
        ApplyUdpOptionInt(fd, IPPROTO_IP, IP_TTL, ParseIntOptionValue(ttl, "ttl"), "IP_TTL");
    } else {
        ApplyUdpOptionInt(fd, IPPROTO_IP, IP_TTL, kDefaultUdpTtl, "IP_TTL");
    }
}

void MaybeBindUdpLocalAddress(int fd, const UdpUri& uri) {
    std::string local_ip = QueryString(uri.query, "localip");
    std::string local_port_value = QueryString(uri.query, "localport");
    if (local_ip.empty() && local_port_value.empty()) {
        return;
    }
    if (local_ip.empty()) {
        local_ip = "0.0.0.0";
    }
    int local_port = 0;
    if (!local_port_value.empty()) {
        local_port = ParseIntOptionValue(local_port_value, "localport");
    }
    socklen_t local_len = 0;
    sockaddr_storage local_addr = ResolveSockaddr(local_ip, local_port, &local_len);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&local_addr), local_len) != 0) {
        throw std::runtime_error("UDP local bind failed: " + std::string(std::strerror(errno)));
    }
}

class SrtOutputCallerSink : public OutputSink {
public:
    SrtOutputCallerSink(std::vector<SrtUri> uris, bool bonded, SRT_GROUP_TYPE group_type)
        : uris_(std::move(uris)), bonded_(bonded), group_type_(group_type) {}

    void EnsureReady(const Config& cfg,
                     const Logger& logger,
                     RelayStats* stats,
                     MetricsState* metrics,
                     const EnsureAttemptContext& attempt_ctx) override {
        ensure_error_kind_ = IoErrorKind::kNone;
        ensure_error_message_.clear();
        metrics_ = metrics;
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
                       "socket=" + std::to_string(sock),
                       "attempt_id=" + std::to_string(attempt_ctx.attempt_id),
                       "incident_id=" + (attempt_ctx.incident_id.empty() ? std::string("none") : attempt_ctx.incident_id));
        } catch (const std::exception& ex) {
            stats->reconnect_count++;
            metrics->reconnect_count.store(stats->reconnect_count, std::memory_order_relaxed);
            metrics->output_connected.store(0, std::memory_order_relaxed);
            ensure_error_kind_ = IsSrtTimeoutError() ? IoErrorKind::kTimeout : IoErrorKind::kError;
            ensure_error_message_ = ComposeSrtEnsureErrorMessage(ex.what());
            throw;
        } catch (...) {
            stats->reconnect_count++;
            metrics->reconnect_count.store(stats->reconnect_count, std::memory_order_relaxed);
            metrics->output_connected.store(0, std::memory_order_relaxed);
            ensure_error_kind_ = IsSrtTimeoutError() ? IoErrorKind::kTimeout : IoErrorKind::kError;
            ensure_error_message_ = ComposeSrtEnsureErrorMessage("unknown exception");
            throw;
        }
    }

    int Send(const char* data, int size) override {
        SRT_MSGCTRL tx_ctrl = srt_msgctrl_default;
        SRT_SOCKGROUPDATA tx_group_data[MetricsState::kMaxTrackedMembers] {};
        tx_ctrl.grpdata = tx_group_data;
        tx_ctrl.grpdata_size = sizeof(tx_group_data) / sizeof(tx_group_data[0]);
        const int rc = srt_sendmsg2(socket_.Get(), data, size, &tx_ctrl);
        if (rc == SRT_ERROR) {
            send_error_kind_ = IsSrtTimeoutError() ? IoErrorKind::kTimeout : IoErrorKind::kDisconnected;
            send_error_message_ = SrtLastErrorString();
            return SRT_ERROR;
        }
        if (metrics_ != nullptr) {
            UpdateOutputLinkHealthFromMsgCtrl(tx_ctrl, metrics_);
        }
        send_error_kind_ = IoErrorKind::kNone;
        send_error_message_.clear();
        return rc;
    }

    void MarkDisconnected(MetricsState* metrics) override {
        socket_.Reset();
        metrics->output_connected.store(0, std::memory_order_relaxed);
        ResetOutputTrackingMetrics(metrics);
    }

    SRTSOCKET TransportSocket() const override { return socket_.Get(); }
    OutputMetricsMode MetricsMode() const override { return OutputMetricsMode::kSrtSocket; }
    bool IsConnected() const override { return socket_.Valid(); }
    IoErrorKind LastSendErrorKind() const override { return send_error_kind_; }
    std::string LastSendErrorMessage() const override { return send_error_message_; }
    IoErrorKind LastEnsureErrorKind() const override { return ensure_error_kind_; }
    std::string LastEnsureErrorMessage() const override { return ensure_error_message_; }

private:
    std::vector<SrtUri> uris_;
    bool bonded_ = false;
    SRT_GROUP_TYPE group_type_ = SRT_GTYPE_UNDEFINED;
    SrtSocketHolder socket_;
    IoErrorKind send_error_kind_ = IoErrorKind::kNone;
    std::string send_error_message_;
    IoErrorKind ensure_error_kind_ = IoErrorKind::kNone;
    std::string ensure_error_message_;
    MetricsState* metrics_ = nullptr;
};

class SrtOutputListenerSink : public OutputSink {
public:
    explicit SrtOutputListenerSink(std::vector<SrtUri> uris) : uris_(std::move(uris)) {}
    ~SrtOutputListenerSink() override { detail::CloseSocketList(&listeners_); }

    void EnsureReady(const Config& cfg,
                     const Logger& logger,
                     RelayStats*,
                     MetricsState* metrics,
                     const EnsureAttemptContext& attempt_ctx) override {
        ensure_error_kind_ = IoErrorKind::kNone;
        ensure_error_message_.clear();
        metrics_ = metrics;
        try {
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
                           "socket=" + std::to_string(accepted),
                           "attempt_id=" + std::to_string(attempt_ctx.attempt_id),
                           "incident_id=" + (attempt_ctx.incident_id.empty() ? std::string("none") : attempt_ctx.incident_id));
            }
            metrics->output_connected.store(session_.Valid() ? 1 : 0, std::memory_order_relaxed);
        } catch (const std::exception& ex) {
            ensure_error_kind_ = IsSrtTimeoutError() ? IoErrorKind::kTimeout : IoErrorKind::kError;
            ensure_error_message_ = ComposeSrtEnsureErrorMessage(ex.what());
            throw;
        } catch (...) {
            ensure_error_kind_ = IsSrtTimeoutError() ? IoErrorKind::kTimeout : IoErrorKind::kError;
            ensure_error_message_ = ComposeSrtEnsureErrorMessage("unknown exception");
            throw;
        }
    }

    int Send(const char* data, int size) override {
        SRT_MSGCTRL tx_ctrl = srt_msgctrl_default;
        SRT_SOCKGROUPDATA tx_group_data[MetricsState::kMaxTrackedMembers] {};
        tx_ctrl.grpdata = tx_group_data;
        tx_ctrl.grpdata_size = sizeof(tx_group_data) / sizeof(tx_group_data[0]);
        const int rc = srt_sendmsg2(session_.Get(), data, size, &tx_ctrl);
        if (rc == SRT_ERROR) {
            send_error_kind_ = IsSrtTimeoutError() ? IoErrorKind::kTimeout : IoErrorKind::kDisconnected;
            send_error_message_ = SrtLastErrorString();
            return SRT_ERROR;
        }
        if (metrics_ != nullptr) {
            UpdateOutputLinkHealthFromMsgCtrl(tx_ctrl, metrics_);
        }
        send_error_kind_ = IoErrorKind::kNone;
        send_error_message_.clear();
        return rc;
    }

    void MarkDisconnected(MetricsState* metrics) override {
        session_.Reset();
        metrics->output_connected.store(0, std::memory_order_relaxed);
        ResetOutputTrackingMetrics(metrics);
    }

    SRTSOCKET TransportSocket() const override { return session_.Get(); }
    OutputMetricsMode MetricsMode() const override { return OutputMetricsMode::kSrtSocket; }
    bool IsConnected() const override { return session_.Valid(); }
    IoErrorKind LastSendErrorKind() const override { return send_error_kind_; }
    std::string LastSendErrorMessage() const override { return send_error_message_; }
    IoErrorKind LastEnsureErrorKind() const override { return ensure_error_kind_; }
    std::string LastEnsureErrorMessage() const override { return ensure_error_message_; }

private:
    std::vector<SrtUri> uris_;
    std::vector<SRTSOCKET> listeners_;
    SrtSocketHolder session_;
    IoErrorKind send_error_kind_ = IoErrorKind::kNone;
    std::string send_error_message_;
    IoErrorKind ensure_error_kind_ = IoErrorKind::kNone;
    std::string ensure_error_message_;
    MetricsState* metrics_ = nullptr;
};

class UdpOutputSink : public OutputSink {
public:
    UdpOutputSink(UdpUri uri, bool listener) : uri_(std::move(uri)), listener_(listener) {}
    ~UdpOutputSink() override { CloseSocket(); }

    void EnsureReady(const Config& cfg,
                     const Logger&,
                     RelayStats* stats,
                     MetricsState* metrics,
                     const EnsureAttemptContext&) override {
        ensure_error_kind_ = IoErrorKind::kNone;
        ensure_error_message_.clear();
        if (socket_fd_ >= 0) {
            return;
        }
        socklen_t peer_len = 0;
        sockaddr_storage peer_addr = ResolveSockaddr(uri_.host, uri_.port, &peer_len);
        const int fd = ::socket(peer_addr.ss_family, SOCK_DGRAM, IPPROTO_UDP);
        if (fd < 0) {
            ensure_error_kind_ = IoErrorKind::kError;
            ensure_error_message_ = std::strerror(errno);
            stats->reconnect_count++;
            metrics->reconnect_count.store(stats->reconnect_count, std::memory_order_relaxed);
            throw std::runtime_error("failed to create UDP socket: " + ensure_error_message_);
        }
        try {
            ApplyUdpSocketOptions(fd, uri_);
            SetUdpSendTimeoutMs(fd, cfg.io_timeout_ms);
            if (listener_) {
                if (::bind(fd, reinterpret_cast<sockaddr*>(&peer_addr), peer_len) != 0) {
                    throw std::runtime_error("UDP bind failed: " + std::string(std::strerror(errno)));
                }
            } else {
                MaybeBindUdpLocalAddress(fd, uri_);
                if (::connect(fd, reinterpret_cast<sockaddr*>(&peer_addr), peer_len) != 0) {
                    throw std::runtime_error("UDP connect failed: " + std::string(std::strerror(errno)));
                }
            }
        } catch (...) {
            const int err = errno;
            ::close(fd);
            ensure_error_kind_ = IsUdpTimeoutErrno(err) ? IoErrorKind::kTimeout : IoErrorKind::kError;
            ensure_error_message_ = std::strerror(err);
            stats->reconnect_count++;
            metrics->reconnect_count.store(stats->reconnect_count, std::memory_order_relaxed);
            metrics->output_connected.store(0, std::memory_order_relaxed);
            throw;
        }

        socket_fd_ = fd;
        metrics->output_connected.store(1, std::memory_order_relaxed);
    }

    int Send(const char* data, int size) override {
        const ssize_t sent = ::send(socket_fd_, data, static_cast<size_t>(size), 0);
        if (sent < 0) {
            send_error_kind_ = IsUdpTimeoutErrno(errno) ? IoErrorKind::kTimeout : IoErrorKind::kError;
            send_error_message_ = std::strerror(errno);
            return SRT_ERROR;
        }
        send_error_kind_ = IoErrorKind::kNone;
        send_error_message_.clear();
        return static_cast<int>(sent);
    }

    void MarkDisconnected(MetricsState* metrics) override {
        CloseSocket();
        metrics->output_connected.store(0, std::memory_order_relaxed);
        ResetOutputTrackingMetrics(metrics);
    }

    SRTSOCKET TransportSocket() const override { return SRT_INVALID_SOCK; }
    OutputMetricsMode MetricsMode() const override { return OutputMetricsMode::kStdout; }
    bool IsConnected() const override { return socket_fd_ >= 0; }
    IoErrorKind LastSendErrorKind() const override { return send_error_kind_; }
    std::string LastSendErrorMessage() const override { return send_error_message_; }
    IoErrorKind LastEnsureErrorKind() const override { return ensure_error_kind_; }
    std::string LastEnsureErrorMessage() const override { return ensure_error_message_; }

private:
    void CloseSocket() {
        if (socket_fd_ >= 0) {
            ::close(socket_fd_);
            socket_fd_ = -1;
        }
    }

    UdpUri uri_;
    bool listener_ = false;
    int socket_fd_ = -1;
    IoErrorKind send_error_kind_ = IoErrorKind::kNone;
    std::string send_error_message_;
    IoErrorKind ensure_error_kind_ = IoErrorKind::kNone;
    std::string ensure_error_message_;
};

class StdoutOutputSink : public OutputSink {
public:
    void EnsureReady(const Config&,
                     const Logger&,
                     RelayStats*,
                     MetricsState* metrics,
                     const EnsureAttemptContext&) override {
        ensure_error_kind_ = IoErrorKind::kNone;
        ensure_error_message_.clear();
        if (!healthy_) {
            ensure_error_kind_ = IoErrorKind::kDisconnected;
            ensure_error_message_ = "stdout sink is not writable";
            throw std::runtime_error("stdout sink is not writable");
        }
        metrics->output_connected.store(1, std::memory_order_relaxed);
    }

    int Send(const char* data, int size) override {
        int total = 0;
        while (total < size) {
            const ssize_t written = ::write(STDOUT_FILENO, data + total, static_cast<size_t>(size - total));
            if (written < 0) {
                send_error_kind_ = IsUdpTimeoutErrno(errno) ? IoErrorKind::kTimeout : IoErrorKind::kError;
                send_error_message_ = std::strerror(errno);
                return SRT_ERROR;
            }
            total += static_cast<int>(written);
        }
        send_error_kind_ = IoErrorKind::kNone;
        send_error_message_.clear();
        return total;
    }

    void MarkDisconnected(MetricsState* metrics) override {
        healthy_ = false;
        metrics->output_connected.store(0, std::memory_order_relaxed);
        ResetOutputTrackingMetrics(metrics);
    }

    SRTSOCKET TransportSocket() const override { return SRT_INVALID_SOCK; }
    OutputMetricsMode MetricsMode() const override { return OutputMetricsMode::kStdout; }
    bool IsConnected() const override { return healthy_; }
    IoErrorKind LastSendErrorKind() const override { return send_error_kind_; }
    std::string LastSendErrorMessage() const override { return send_error_message_; }
    IoErrorKind LastEnsureErrorKind() const override { return ensure_error_kind_; }
    std::string LastEnsureErrorMessage() const override { return ensure_error_message_; }

private:
    bool healthy_ = true;
    IoErrorKind send_error_kind_ = IoErrorKind::kNone;
    std::string send_error_message_;
    IoErrorKind ensure_error_kind_ = IoErrorKind::kNone;
    std::string ensure_error_message_;
};

}  // namespace

std::unique_ptr<OutputSink> BuildOutputSink(const OutputEndpointSpec& spec) {
    if (spec.kind == OutputEndpointKind::kStdout) {
        return std::make_unique<StdoutOutputSink>();
    }
    if (spec.kind == OutputEndpointKind::kUdpCaller) {
        return std::make_unique<UdpOutputSink>(spec.udp_uri, false);
    }
    if (spec.kind == OutputEndpointKind::kUdpListener) {
        return std::make_unique<UdpOutputSink>(spec.udp_uri, true);
    }
    if (spec.kind == OutputEndpointKind::kSrtListener) {
        return std::make_unique<SrtOutputListenerSink>(spec.uris);
    }
    return std::make_unique<SrtOutputCallerSink>(spec.uris, spec.bonded, spec.group_type);
}

}  // namespace srtrelay
