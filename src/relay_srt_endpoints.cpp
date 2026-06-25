#include "relay_io_internal.hpp"

#include <netdb.h>

#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace srtrelay::detail {

void CloseSocketList(std::vector<SRTSOCKET>* sockets) {
    if (sockets == nullptr) return;
    for (auto& sock : *sockets) {
        if (sock != SRT_INVALID_SOCK) {
            srt_close(sock);
            sock = SRT_INVALID_SOCK;
        }
    }
    sockets->clear();
}

SRTSOCKET CreateListeningSocket(const SrtUri& uri, const Config& cfg, const Logger& logger, const char* endpoint_name) {
    SRTSOCKET listener = srt_create_socket();
    if (listener == SRT_INVALID_SOCK) {
        throw std::runtime_error(std::string("failed to create ") + endpoint_name + " listener socket: " + SrtLastErrorString());
    }
    try {
        const int one = 1;
        ApplyIntSockOpt(listener, SRTO_REUSEADDR, one, "SRTO_REUSEADDR");
        ApplyIntSockOpt(listener, SRTO_GROUPCONNECT, one, "SRTO_GROUPCONNECT");
        ApplyIntSockOpt(listener, SRTO_RCVTIMEO, cfg.io_timeout_ms, "SRTO_RCVTIMEO");
        ApplyCommonSrtOptions(listener, uri, logger);

        socklen_t addr_len = 0;
        sockaddr_storage addr = ResolveSockaddr(uri.host, uri.port, &addr_len);
        if (srt_bind(listener, reinterpret_cast<sockaddr*>(&addr), addr_len) == SRT_ERROR) {
            throw std::runtime_error(std::string(endpoint_name) + " bind failed: " + SrtLastErrorString());
        }
        if (srt_listen(listener, 8) == SRT_ERROR) {
            throw std::runtime_error(std::string(endpoint_name) + " listen failed: " + SrtLastErrorString());
        }
    } catch (...) {
        srt_close(listener);
        throw;
    }

    logger.Log(LogLevel::kInfo, "srt-listening",
               std::string("endpoint=") + endpoint_name,
               "uri_host=" + uri.host,
               "uri_port=" + std::to_string(uri.port));
    return listener;
}

SRTSOCKET AcceptBondSession(const std::vector<SRTSOCKET>& listeners, const Config& cfg, const char* endpoint_name) {
    if (listeners.empty()) {
        throw std::runtime_error(std::string(endpoint_name) + " listener set is empty");
    }
    SRTSOCKET accepted = srt_accept_bond(listeners.data(),
                                         static_cast<int>(listeners.size()),
                                         static_cast<int64_t>(cfg.io_timeout_ms));
    if (accepted == SRT_INVALID_SOCK) {
        throw std::runtime_error(std::string(endpoint_name) + " accept failed: " + SrtLastErrorString());
    }
    return accepted;
}

namespace {

const std::string* QueryFirstValue(const std::map<std::string, std::string>& query,
                                   std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        const auto it = query.find(key);
        if (it != query.end() && !it->second.empty()) return &it->second;
    }
    return nullptr;
}

bool ResolveSourceSockaddrForPeerFamily(const std::string& source_host,
                                        int peer_family,
                                        sockaddr_storage* out_addr,
                                        socklen_t* out_len) {
    if (source_host.empty() || out_addr == nullptr || out_len == nullptr) {
        return false;
    }

    addrinfo hints {};
    hints.ai_family = (peer_family == AF_INET || peer_family == AF_INET6) ? peer_family : AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = AI_NUMERICHOST;

    addrinfo* result = nullptr;
    const int rc = getaddrinfo(source_host.c_str(), "0", &hints, &result);
    if (rc != 0 || result == nullptr) {
        return false;
    }

    std::memset(out_addr, 0, sizeof(*out_addr));
    std::memcpy(out_addr, result->ai_addr, result->ai_addrlen);
    *out_len = static_cast<socklen_t>(result->ai_addrlen);
    freeaddrinfo(result);
    return true;
}

}  // namespace

SRTSOCKET ConnectSingleCallerSocket(const SrtUri& uri, const Config& cfg, const Logger& logger, bool for_output) {
    SRTSOCKET sock = srt_create_socket();
    if (sock == SRT_INVALID_SOCK) {
        throw std::runtime_error("failed to create caller socket: " + SrtLastErrorString());
    }
    try {
        ApplyIntSockOpt(sock, SRTO_CONNTIMEO, cfg.io_timeout_ms, "SRTO_CONNTIMEO");
        if (for_output) {
            ApplyIntSockOpt(sock, SRTO_SNDTIMEO, cfg.io_timeout_ms, "SRTO_SNDTIMEO");
        } else {
            ApplyIntSockOpt(sock, SRTO_RCVTIMEO, cfg.io_timeout_ms, "SRTO_RCVTIMEO");
        }
        ApplyCommonSrtOptions(sock, uri, logger);
        socklen_t addr_len = 0;
        sockaddr_storage addr = ResolveSockaddr(uri.host, uri.port, &addr_len);
        if (srt_connect(sock, reinterpret_cast<sockaddr*>(&addr), addr_len) == SRT_ERROR) {
            throw std::runtime_error("SRT connect failed: " + SrtLastErrorString());
        }
    } catch (...) {
        srt_close(sock);
        throw;
    }
    return sock;
}

SRTSOCKET ConnectBondedCallerGroup(const std::vector<SrtUri>& uris,
                                   SRT_GROUP_TYPE group_type,
                                   const Config& cfg,
                                   const Logger& logger,
                                   bool for_output) {
    if (uris.empty()) {
        throw std::runtime_error("cannot create bonded caller group with empty URI set");
    }
    if (group_type == SRT_GTYPE_UNDEFINED) {
        throw std::runtime_error("bonded caller group requires explicit group type");
    }

    SRTSOCKET group = srt_create_group(group_type);
    if (group == SRT_INVALID_SOCK) {
        throw std::runtime_error("failed to create SRT group: " + SrtLastErrorString());
    }

    try {
        ApplyIntSockOpt(group, SRTO_CONNTIMEO, cfg.io_timeout_ms, "SRTO_CONNTIMEO");
        if (for_output) {
            ApplyIntSockOpt(group, SRTO_SNDTIMEO, cfg.io_timeout_ms, "SRTO_SNDTIMEO");
        } else {
            ApplyIntSockOpt(group, SRTO_RCVTIMEO, cfg.io_timeout_ms, "SRTO_RCVTIMEO");
        }
        SrtUri group_options_uri = uris.front();
        // Some libsrt builds reject SRTO_TRANSTYPE on group sockets.
        group_options_uri.query.erase("transtype");
        ApplyCommonSrtOptions(group, group_options_uri, logger);

        std::vector<SRT_SOCKGROUPCONFIG> endpoints;
        endpoints.reserve(uris.size());
        for (const auto& uri : uris) {
            socklen_t addr_len = 0;
            sockaddr_storage addr = ResolveSockaddr(uri.host, uri.port, &addr_len);
            const sockaddr* src_addr_ptr = nullptr;
            sockaddr_storage src_addr {};
            socklen_t src_addr_len = 0;

            if (const std::string* source_host =
                    QueryFirstValue(uri.query, {"srcip", "sourceip", "localip", "adapterip", "adapter_ip"})) {
                if (!ResolveSourceSockaddrForPeerFamily(*source_host,
                                                        reinterpret_cast<const sockaddr*>(&addr)->sa_family,
                                                        &src_addr,
                                                        &src_addr_len)) {
                    throw std::runtime_error("failed to resolve source adapter IP for bonded member: " + *source_host);
                }
                src_addr_ptr = reinterpret_cast<const sockaddr*>(&src_addr);
            }

            SRT_SOCKGROUPCONFIG endpoint =
                srt_prepare_endpoint(src_addr_ptr, reinterpret_cast<const sockaddr*>(&addr), static_cast<int>(addr_len));
            endpoints.push_back(endpoint);
        }

        if (srt_connect_group(group, endpoints.data(), static_cast<int>(endpoints.size())) == SRT_ERROR) {
            throw std::runtime_error("bonded group connect failed: " + SrtLastErrorString());
        }
    } catch (...) {
        srt_close(group);
        throw;
    }

    return group;
}

}  // namespace srtrelay::detail
