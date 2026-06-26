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

void SetUdpTimeoutMs(int fd, int timeout_ms) {
    timeval tv {};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        throw std::runtime_error("failed to set UDP receive timeout: " + std::string(std::strerror(errno)));
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
    if (const std::string rcvbuf = QueryString(uri.query, "rcvbuf"); !rcvbuf.empty()) {
        ApplyUdpOptionInt(fd, SOL_SOCKET, SO_RCVBUF, ParseIntOptionValue(rcvbuf, "rcvbuf"), "SO_RCVBUF");
    }
    if (const std::string sndbuf = QueryString(uri.query, "sndbuf"); !sndbuf.empty()) {
        ApplyUdpOptionInt(fd, SOL_SOCKET, SO_SNDBUF, ParseIntOptionValue(sndbuf, "sndbuf"), "SO_SNDBUF");
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

class SrtInputListenerSource : public InputSource {
public:
    explicit SrtInputListenerSource(std::vector<SrtUri> uris) : uris_(std::move(uris)) {}
    ~SrtInputListenerSource() override { detail::CloseSocketList(&listeners_); }

    void EnsureReady(const Config& cfg,
                     const Logger& logger,
                     MetricsState* metrics,
                     const EnsureAttemptContext& attempt_ctx) override {
        ensure_error_kind_ = IoErrorKind::kNone;
        ensure_error_message_.clear();
        try {
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
                logger.Log(LogLevel::kInfo,
                           "input-connected",
                           "socket=" + std::to_string(accepted),
                           "attempt_id=" + std::to_string(attempt_ctx.attempt_id),
                           "incident_id=" + (attempt_ctx.incident_id.empty() ? std::string("none") : attempt_ctx.incident_id));
            }
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

    int Receive(const Config& cfg, std::vector<char>* buffer, SRT_MSGCTRL* rx_ctrl) override {
        const int rc = srt_recvmsg2(session_.Get(), buffer->data(), cfg.max_message_size, rx_ctrl);
        if (rc == SRT_ERROR) {
            if (IsSrtTimeoutError()) {
                receive_error_kind_ = IoErrorKind::kTimeout;
                receive_error_message_ = "SRT receive timeout";
            } else {
                receive_error_kind_ = IoErrorKind::kDisconnected;
                receive_error_message_ = SrtLastErrorString();
            }
            return SRT_ERROR;
        }
        receive_error_kind_ = IoErrorKind::kNone;
        receive_error_message_.clear();
        return rc;
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
    IoErrorKind LastReceiveErrorKind() const override { return receive_error_kind_; }
    std::string LastReceiveErrorMessage() const override { return receive_error_message_; }
    IoErrorKind LastEnsureErrorKind() const override { return ensure_error_kind_; }
    std::string LastEnsureErrorMessage() const override { return ensure_error_message_; }

private:
    std::vector<SrtUri> uris_;
    std::vector<SRTSOCKET> listeners_;
    SrtSocketHolder session_;
    IoErrorKind receive_error_kind_ = IoErrorKind::kNone;
    std::string receive_error_message_;
    IoErrorKind ensure_error_kind_ = IoErrorKind::kNone;
    std::string ensure_error_message_;
};

class SrtInputCallerSource : public InputSource {
public:
    SrtInputCallerSource(std::vector<SrtUri> uris, bool bonded, SRT_GROUP_TYPE group_type)
        : uris_(std::move(uris)), bonded_(bonded), group_type_(group_type) {}

    void EnsureReady(const Config& cfg,
                     const Logger& logger,
                     MetricsState* metrics,
                     const EnsureAttemptContext& attempt_ctx) override {
        ensure_error_kind_ = IoErrorKind::kNone;
        ensure_error_message_.clear();
        try {
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
                       "socket=" + std::to_string(sock),
                       "attempt_id=" + std::to_string(attempt_ctx.attempt_id),
                       "incident_id=" + (attempt_ctx.incident_id.empty() ? std::string("none") : attempt_ctx.incident_id));
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

    int Receive(const Config& cfg, std::vector<char>* buffer, SRT_MSGCTRL* rx_ctrl) override {
        const int rc = srt_recvmsg2(session_.Get(), buffer->data(), cfg.max_message_size, rx_ctrl);
        if (rc == SRT_ERROR) {
            if (IsSrtTimeoutError()) {
                receive_error_kind_ = IoErrorKind::kTimeout;
                receive_error_message_ = "SRT receive timeout";
            } else {
                receive_error_kind_ = IoErrorKind::kDisconnected;
                receive_error_message_ = SrtLastErrorString();
            }
            return SRT_ERROR;
        }
        receive_error_kind_ = IoErrorKind::kNone;
        receive_error_message_.clear();
        return rc;
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
    IoErrorKind LastReceiveErrorKind() const override { return receive_error_kind_; }
    std::string LastReceiveErrorMessage() const override { return receive_error_message_; }
    IoErrorKind LastEnsureErrorKind() const override { return ensure_error_kind_; }
    std::string LastEnsureErrorMessage() const override { return ensure_error_message_; }

private:
    std::vector<SrtUri> uris_;
    bool bonded_ = false;
    SRT_GROUP_TYPE group_type_ = SRT_GTYPE_UNDEFINED;
    SrtSocketHolder session_;
    IoErrorKind receive_error_kind_ = IoErrorKind::kNone;
    std::string receive_error_message_;
    IoErrorKind ensure_error_kind_ = IoErrorKind::kNone;
    std::string ensure_error_message_;
};

class UdpInputSource : public InputSource {
public:
    UdpInputSource(UdpUri uri, bool listener) : uri_(std::move(uri)), listener_(listener) {}
    ~UdpInputSource() override { CloseSocket(); }

    void EnsureReady(const Config& cfg,
                     const Logger&,
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
            throw std::runtime_error("failed to create UDP socket: " + ensure_error_message_);
        }

        try {
            ApplyUdpSocketOptions(fd, uri_);
            SetUdpTimeoutMs(fd, cfg.io_timeout_ms);
            MaybeBindUdpLocalAddress(fd, uri_);
            if (listener_) {
                if (::bind(fd, reinterpret_cast<sockaddr*>(&peer_addr), peer_len) != 0) {
                    throw std::runtime_error("UDP bind failed: " + std::string(std::strerror(errno)));
                }
            } else if (::connect(fd, reinterpret_cast<sockaddr*>(&peer_addr), peer_len) != 0) {
                throw std::runtime_error("UDP connect failed: " + std::string(std::strerror(errno)));
            }
        } catch (...) {
            ::close(fd);
            const int err = errno;
            ensure_error_kind_ = IsUdpTimeoutErrno(err) ? IoErrorKind::kTimeout : IoErrorKind::kError;
            ensure_error_message_ = std::strerror(err);
            throw;
        }

        socket_fd_ = fd;
        metrics->input_connected.store(1, std::memory_order_relaxed);
        metrics->input_listening.store(listener_ ? 1 : 0, std::memory_order_relaxed);
    }

    int Receive(const Config& cfg, std::vector<char>* buffer, SRT_MSGCTRL* rx_ctrl) override {
        (void)cfg;
        (void)rx_ctrl;
        const ssize_t n = ::recv(socket_fd_, buffer->data(), buffer->size(), 0);
        if (n < 0) {
            receive_error_kind_ = IsUdpTimeoutErrno(errno) ? IoErrorKind::kTimeout : IoErrorKind::kError;
            receive_error_message_ = std::strerror(errno);
            return SRT_ERROR;
        }
        receive_error_kind_ = IoErrorKind::kNone;
        receive_error_message_.clear();
        return static_cast<int>(n);
    }

    void HandleReceiveError(const Config&, const Logger&, MetricsState* metrics) override {
        metrics->input_connected.store(0, std::memory_order_relaxed);
        metrics->input_listening.store(0, std::memory_order_relaxed);
        ResetInputTrackingMetrics(metrics);
        CloseSocket();
    }

    SRTSOCKET SessionSocket() const override { return SRT_INVALID_SOCK; }
    bool IsListening() const override { return listener_ && socket_fd_ >= 0; }
    bool IsConnected() const override { return socket_fd_ >= 0; }
    bool IsTerminalEof() const override { return false; }
    IoErrorKind LastReceiveErrorKind() const override { return receive_error_kind_; }
    std::string LastReceiveErrorMessage() const override { return receive_error_message_; }
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
    bool listener_ = true;
    int socket_fd_ = -1;
    IoErrorKind receive_error_kind_ = IoErrorKind::kNone;
    std::string receive_error_message_;
    IoErrorKind ensure_error_kind_ = IoErrorKind::kNone;
    std::string ensure_error_message_;
};

class StdinInputSource : public InputSource {
public:
    void EnsureReady(const Config&,
                     const Logger&,
                     MetricsState* metrics,
                     const EnsureAttemptContext&) override {
        ensure_error_kind_ = IoErrorKind::kNone;
        ensure_error_message_.clear();
        if (eof_) {
            ensure_error_kind_ = IoErrorKind::kDisconnected;
            ensure_error_message_ = "stdin reached EOF";
            throw std::runtime_error("stdin reached EOF");
        }
        metrics->input_listening.store(0, std::memory_order_relaxed);
        metrics->input_connected.store(1, std::memory_order_relaxed);
    }

    int Receive(const Config& cfg, std::vector<char>* buffer, SRT_MSGCTRL* rx_ctrl) override {
        (void)rx_ctrl;
        const ssize_t n = ::read(STDIN_FILENO, buffer->data(), static_cast<size_t>(cfg.max_message_size));
        if (n > 0) {
            receive_error_kind_ = IoErrorKind::kNone;
            receive_error_message_.clear();
            return static_cast<int>(n);
        }
        if (n == 0) {
            eof_ = true;
            receive_error_kind_ = IoErrorKind::kDisconnected;
            receive_error_message_ = "stdin EOF";
            return SRT_ERROR;
        }
        receive_error_kind_ = (errno == EAGAIN || errno == EWOULDBLOCK) ? IoErrorKind::kTimeout : IoErrorKind::kError;
        receive_error_message_ = std::strerror(errno);
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
    IoErrorKind LastReceiveErrorKind() const override { return receive_error_kind_; }
    std::string LastReceiveErrorMessage() const override { return receive_error_message_; }
    IoErrorKind LastEnsureErrorKind() const override { return ensure_error_kind_; }
    std::string LastEnsureErrorMessage() const override { return ensure_error_message_; }

private:
    bool eof_ = false;
    IoErrorKind receive_error_kind_ = IoErrorKind::kNone;
    std::string receive_error_message_;
    IoErrorKind ensure_error_kind_ = IoErrorKind::kNone;
    std::string ensure_error_message_;
};

}  // namespace

std::unique_ptr<InputSource> BuildInputSource(const InputEndpointSpec& spec) {
    if (spec.kind == InputEndpointKind::kStdin) {
        return std::make_unique<StdinInputSource>();
    }
    if (spec.kind == InputEndpointKind::kUdpListener) {
        return std::make_unique<UdpInputSource>(spec.udp_uri, true);
    }
    if (spec.kind == InputEndpointKind::kUdpCaller) {
        return std::make_unique<UdpInputSource>(spec.udp_uri, false);
    }
    if (spec.kind == InputEndpointKind::kSrtCaller) {
        return std::make_unique<SrtInputCallerSource>(spec.uris, spec.bonded, spec.group_type);
    }
    return std::make_unique<SrtInputListenerSource>(spec.uris);
}

}  // namespace srtrelay
