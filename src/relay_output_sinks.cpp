#include "srtrelay/relay_io.hpp"

#include <errno.h>
#include <netdb.h>
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

std::string FormatSockaddr(const sockaddr* addr, socklen_t addr_len) {
    if (addr == nullptr || addr_len == 0) {
        return "unknown";
    }
    char host[NI_MAXHOST] {};
    char service[NI_MAXSERV] {};
    const int rc = getnameinfo(addr,
                               addr_len,
                               host,
                               sizeof(host),
                               service,
                               sizeof(service),
                               NI_NUMERICHOST | NI_NUMERICSERV);
    if (rc != 0) {
        return "unknown";
    }
    return std::string(host) + ":" + service;
}

void ReadFdEndpoints(int fd, std::string* local_endpoint, std::string* remote_endpoint) {
    if (local_endpoint != nullptr) {
        *local_endpoint = "unknown";
    }
    if (remote_endpoint != nullptr) {
        *remote_endpoint = "unknown";
    }
    if (fd < 0) {
        return;
    }

    sockaddr_storage local_addr {};
    socklen_t local_len = sizeof(local_addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&local_addr), &local_len) == 0 && local_endpoint != nullptr) {
        *local_endpoint = FormatSockaddr(reinterpret_cast<sockaddr*>(&local_addr), local_len);
    }
    sockaddr_storage remote_addr {};
    socklen_t remote_len = sizeof(remote_addr);
    if (::getpeername(fd, reinterpret_cast<sockaddr*>(&remote_addr), &remote_len) == 0 && remote_endpoint != nullptr) {
        *remote_endpoint = FormatSockaddr(reinterpret_cast<sockaddr*>(&remote_addr), remote_len);
    }
}

void ReadSrtEndpoints(SRTSOCKET sock, std::string* local_endpoint, std::string* remote_endpoint) {
    if (local_endpoint != nullptr) {
        *local_endpoint = "unknown";
    }
    if (remote_endpoint != nullptr) {
        *remote_endpoint = "unknown";
    }
    if (sock == SRT_INVALID_SOCK) {
        return;
    }

    sockaddr_storage local_addr {};
    int local_len = sizeof(local_addr);
    if (srt_getsockname(sock, reinterpret_cast<sockaddr*>(&local_addr), &local_len) != SRT_ERROR &&
        local_endpoint != nullptr) {
        *local_endpoint = FormatSockaddr(reinterpret_cast<sockaddr*>(&local_addr), static_cast<socklen_t>(local_len));
    }

    sockaddr_storage remote_addr {};
    int remote_len = sizeof(remote_addr);
    if (srt_getpeername(sock, reinterpret_cast<sockaddr*>(&remote_addr), &remote_len) != SRT_ERROR &&
        remote_endpoint != nullptr) {
        *remote_endpoint = FormatSockaddr(reinterpret_cast<sockaddr*>(&remote_addr), static_cast<socklen_t>(remote_len));
    }
}

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
            std::string local_endpoint;
            std::string remote_endpoint;
            ReadSrtEndpoints(sock, &local_endpoint, &remote_endpoint);
            logger.Log(LogLevel::kInfo, "output-connected",
                       std::string("mode=caller"),
                       "output_index=" + std::to_string(attempt_ctx.source_index.value_or(1)),
                       "bonded=" + std::string(bonded_ ? "true" : "false"),
                       "members=" + std::to_string(uris_.size()),
                       "socket=" + std::to_string(sock),
                       "local_endpoint=" + local_endpoint,
                       "remote_endpoint=" + remote_endpoint,
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

    OutputSendResult Send(const char* data, int size) override {
        SRT_MSGCTRL tx_ctrl = srt_msgctrl_default;
        SRT_SOCKGROUPDATA tx_group_data[MetricsState::kMaxTrackedMembers] {};
        tx_ctrl.grpdata = tx_group_data;
        tx_ctrl.grpdata_size = sizeof(tx_group_data) / sizeof(tx_group_data[0]);
        const int rc = srt_sendmsg2(socket_.Get(), data, size, &tx_ctrl);
        if (rc == SRT_ERROR) {
            send_error_kind_ = IsSrtTimeoutError() ? IoErrorKind::kTimeout : IoErrorKind::kDisconnected;
            send_error_message_ = SrtLastErrorString();
            return {OutputSendStatus::kError, 0};
        }
        if (metrics_ != nullptr) {
            UpdateOutputLinkHealthFromMsgCtrl(tx_ctrl, metrics_);
        }
        send_error_kind_ = IoErrorKind::kNone;
        send_error_message_.clear();
        return {OutputSendStatus::kSent, rc};
    }

    void MarkDisconnected(MetricsState* metrics) override {
        socket_.Reset();
        metrics->output_connected.store(0, std::memory_order_relaxed);
        ResetOutputTrackingMetrics(metrics);
    }

    SRTSOCKET TransportSocket() const override { return socket_.Get(); }
    OutputMetricsMode MetricsMode() const override { return OutputMetricsMode::kSrtSocket; }
    bool IsConnected() const override { return socket_.Valid(); }
    bool IsListening() const override { return false; }
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
                std::string local_endpoint;
                std::string remote_endpoint;
                ReadSrtEndpoints(accepted, &local_endpoint, &remote_endpoint);
                logger.Log(LogLevel::kInfo, "output-client-connected",
                           "socket=" + std::to_string(accepted),
                           "output_index=" + std::to_string(attempt_ctx.source_index.value_or(1)),
                           "local_endpoint=" + local_endpoint,
                           "remote_endpoint=" + remote_endpoint,
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

    OutputSendResult Send(const char* data, int size) override {
        SRT_MSGCTRL tx_ctrl = srt_msgctrl_default;
        SRT_SOCKGROUPDATA tx_group_data[MetricsState::kMaxTrackedMembers] {};
        tx_ctrl.grpdata = tx_group_data;
        tx_ctrl.grpdata_size = sizeof(tx_group_data) / sizeof(tx_group_data[0]);
        const int rc = srt_sendmsg2(session_.Get(), data, size, &tx_ctrl);
        if (rc == SRT_ERROR) {
            send_error_kind_ = IsSrtTimeoutError() ? IoErrorKind::kTimeout : IoErrorKind::kDisconnected;
            send_error_message_ = SrtLastErrorString();
            return {OutputSendStatus::kError, 0};
        }
        if (metrics_ != nullptr) {
            UpdateOutputLinkHealthFromMsgCtrl(tx_ctrl, metrics_);
        }
        send_error_kind_ = IoErrorKind::kNone;
        send_error_message_.clear();
        return {OutputSendStatus::kSent, rc};
    }

    void MarkDisconnected(MetricsState* metrics) override {
        session_.Reset();
        metrics->output_connected.store(0, std::memory_order_relaxed);
        ResetOutputTrackingMetrics(metrics);
    }

    SRTSOCKET TransportSocket() const override { return session_.Get(); }
    OutputMetricsMode MetricsMode() const override { return OutputMetricsMode::kSrtSocket; }
    bool IsConnected() const override { return session_.Valid(); }
    bool IsListening() const override { return !listeners_.empty(); }
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
                     const Logger& logger,
                     RelayStats* stats,
                     MetricsState* metrics,
                     const EnsureAttemptContext& attempt_ctx) override {
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
        std::string local_endpoint;
        std::string remote_endpoint;
        ReadFdEndpoints(fd, &local_endpoint, &remote_endpoint);
        logger.Log(LogLevel::kInfo,
                   "output-connected",
                   "output_index=" + std::to_string(attempt_ctx.source_index.value_or(1)),
                   std::string("mode=") + (listener_ ? "listener" : "caller"),
                   "socket_fd=" + std::to_string(fd),
                   "local_endpoint=" + local_endpoint,
                   "remote_endpoint=" + remote_endpoint);
    }

    OutputSendResult Send(const char* data, int size) override {
        const ssize_t sent = ::send(socket_fd_, data, static_cast<size_t>(size), 0);
        if (sent < 0) {
            send_error_kind_ = IsUdpTimeoutErrno(errno) ? IoErrorKind::kTimeout : IoErrorKind::kError;
            send_error_message_ = std::strerror(errno);
            return {OutputSendStatus::kError, 0};
        }
        send_error_kind_ = IoErrorKind::kNone;
        send_error_message_.clear();
        return {OutputSendStatus::kSent, static_cast<int>(sent)};
    }

    void MarkDisconnected(MetricsState* metrics) override {
        CloseSocket();
        metrics->output_connected.store(0, std::memory_order_relaxed);
        ResetOutputTrackingMetrics(metrics);
    }

    SRTSOCKET TransportSocket() const override { return SRT_INVALID_SOCK; }
    OutputMetricsMode MetricsMode() const override { return OutputMetricsMode::kStdout; }
    bool IsConnected() const override { return socket_fd_ >= 0; }
    bool IsListening() const override { return listener_ && socket_fd_ >= 0; }
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

    OutputSendResult Send(const char* data, int size) override {
        int total = 0;
        while (total < size) {
            const ssize_t written = ::write(STDOUT_FILENO, data + total, static_cast<size_t>(size - total));
            if (written < 0) {
                send_error_kind_ = IsUdpTimeoutErrno(errno) ? IoErrorKind::kTimeout : IoErrorKind::kError;
                send_error_message_ = std::strerror(errno);
                return {OutputSendStatus::kError, 0};
            }
            total += static_cast<int>(written);
        }
        send_error_kind_ = IoErrorKind::kNone;
        send_error_message_.clear();
        return {OutputSendStatus::kSent, total};
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

class OutputFanoutSink : public OutputSink {
public:
    explicit OutputFanoutSink(std::vector<OutputEndpointSpec> specs) : specs_(std::move(specs)) {
        sinks_.reserve(specs_.size());
        for (const auto& spec : specs_) {
            sinks_.push_back(BuildOutputSink(spec));
        }
    }

    void EnsureReady(const Config& cfg,
                     const Logger& logger,
                     RelayStats* stats,
                     MetricsState* metrics,
                     const EnsureAttemptContext& attempt_ctx) override {
        ensure_error_kind_ = IoErrorKind::kNone;
        ensure_error_message_.clear();
        logger_ = &logger;
        metrics_ = metrics;

        bool had_error = false;
        for (size_t i = 0; i < sinks_.size(); ++i) {
            OutputSink* sink = sinks_.at(i).get();
            if (!sink->NeedsEnsurePoll()) {
                continue;
            }
            EnsureAttemptContext child_ctx = attempt_ctx;
            child_ctx.source_index = i + 1;
            try {
                sink->EnsureReady(cfg, logger, stats, metrics, child_ctx);
            } catch (const std::exception& ex) {
                had_error = true;
                ensure_error_kind_ = sink->LastEnsureErrorKind();
                ensure_error_message_ = sink->LastEnsureErrorMessage().empty()
                    ? std::string(ex.what())
                    : sink->LastEnsureErrorMessage();
                logger.Log(LogLevel::kWarn,
                           "output-branch-ensure-failed",
                           "output_index=" + std::to_string(i + 1),
                           "reason_detail=" + ensure_error_message_,
                           "attempt_id=" + std::to_string(attempt_ctx.attempt_id),
                           "incident_id=" + (attempt_ctx.incident_id.empty() ? std::string("none") : attempt_ctx.incident_id));
            } catch (...) {
                had_error = true;
                ensure_error_kind_ = sink->LastEnsureErrorKind();
                ensure_error_message_ = sink->LastEnsureErrorMessage().empty()
                    ? std::string("unknown output ensure failure")
                    : sink->LastEnsureErrorMessage();
                logger.Log(LogLevel::kWarn,
                           "output-branch-ensure-failed",
                           "output_index=" + std::to_string(i + 1),
                           "reason_detail=" + ensure_error_message_,
                           "attempt_id=" + std::to_string(attempt_ctx.attempt_id),
                           "incident_id=" + (attempt_ctx.incident_id.empty() ? std::string("none") : attempt_ctx.incident_id));
            }
        }

        RefreshAggregateConnected(metrics);
        if (!IsConnected() && had_error) {
            throw std::runtime_error(ensure_error_message_.empty() ? "all outputs disconnected" : ensure_error_message_);
        }
    }

    OutputSendResult Send(const char* data, int size) override {
        send_error_kind_ = IoErrorKind::kNone;
        send_error_message_.clear();
        bool sent_any = false;
        bool had_error = false;
        int sent_bytes = 0;

        for (size_t i = 0; i < sinks_.size(); ++i) {
            OutputSink* sink = sinks_.at(i).get();
            if (!sink->IsConnected()) {
                continue;
            }
            const OutputSendResult child_result = sink->Send(data, size);
            if (child_result.status == OutputSendStatus::kError) {
                had_error = true;
                send_error_kind_ = sink->LastSendErrorKind();
                send_error_message_ = sink->LastSendErrorMessage();
                sink->MarkDisconnected(metrics_ == nullptr ? &fallback_metrics_ : metrics_);
                if (logger_ != nullptr) {
                    logger_->Log(LogLevel::kWarn,
                                 "output-branch-send-failed",
                                 "output_index=" + std::to_string(i + 1),
                                 "reason_detail=" + (send_error_message_.empty()
                                     ? std::string("output send failed")
                                     : send_error_message_));
                }
                continue;
            }
            sent_any = true;
            if (sent_bytes == 0) {
                sent_bytes = child_result.bytes;
            }
        }

        RefreshAggregateConnected(metrics_ == nullptr ? &fallback_metrics_ : metrics_);
        if (sent_any) {
            send_error_kind_ = IoErrorKind::kNone;
            send_error_message_.clear();
            return {OutputSendStatus::kSent, sent_bytes};
        }
        if (had_error) {
            return {OutputSendStatus::kError, 0};
        }
        send_error_kind_ = IoErrorKind::kDisconnected;
        send_error_message_ = "no connected outputs available";
        return {OutputSendStatus::kError, 0};
    }

    void MarkDisconnected(MetricsState* metrics) override {
        for (auto& sink : sinks_) {
            sink->MarkDisconnected(metrics);
        }
        metrics->output_connected.store(0, std::memory_order_relaxed);
        ResetOutputTrackingMetrics(metrics);
    }

    SRTSOCKET TransportSocket() const override {
        for (const auto& sink : sinks_) {
            if (sink->MetricsMode() == OutputMetricsMode::kSrtSocket && sink->TransportSocket() != SRT_INVALID_SOCK) {
                return sink->TransportSocket();
            }
        }
        return SRT_INVALID_SOCK;
    }

    OutputMetricsMode MetricsMode() const override {
        return TransportSocket() == SRT_INVALID_SOCK ? OutputMetricsMode::kStdout : OutputMetricsMode::kSrtSocket;
    }

    bool IsConnected() const override {
        for (const auto& sink : sinks_) {
            if (sink->IsConnected()) {
                return true;
            }
        }
        return false;
    }

    bool IsListening() const override {
        for (const auto& sink : sinks_) {
            if (sink->IsListening()) {
                return true;
            }
        }
        return false;
    }

    bool NeedsEnsurePoll() const override {
        for (const auto& sink : sinks_) {
            if (sink->NeedsEnsurePoll()) {
                return true;
            }
        }
        return false;
    }

    IoErrorKind LastSendErrorKind() const override { return send_error_kind_; }
    std::string LastSendErrorMessage() const override { return send_error_message_; }
    IoErrorKind LastEnsureErrorKind() const override { return ensure_error_kind_; }
    std::string LastEnsureErrorMessage() const override { return ensure_error_message_; }
    size_t OutputCount() const override { return sinks_.size(); }
    bool OutputConnected(size_t index) const override {
        return index < sinks_.size() ? sinks_[index]->IsConnected() : false;
    }
    bool OutputListening(size_t index) const override {
        return index < sinks_.size() ? sinks_[index]->IsListening() : false;
    }

private:
    void RefreshAggregateConnected(MetricsState* metrics) const {
        if (metrics == nullptr) {
            return;
        }
        metrics->output_connected.store(IsConnected() ? 1 : 0, std::memory_order_relaxed);
    }

    std::vector<OutputEndpointSpec> specs_;
    std::vector<std::unique_ptr<OutputSink>> sinks_;
    IoErrorKind send_error_kind_ = IoErrorKind::kNone;
    std::string send_error_message_;
    IoErrorKind ensure_error_kind_ = IoErrorKind::kNone;
    std::string ensure_error_message_;
    const Logger* logger_ = nullptr;
    MetricsState* metrics_ = nullptr;
    MetricsState fallback_metrics_;
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

std::unique_ptr<OutputSink> BuildOutputSink(const Config&, std::vector<OutputEndpointSpec> specs) {
    if (specs.empty()) {
        throw std::runtime_error("no output specs configured");
    }
    if (specs.size() == 1) {
        return BuildOutputSink(specs.front());
    }
    return std::make_unique<OutputFanoutSink>(std::move(specs));
}

}  // namespace srtrelay
