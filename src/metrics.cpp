#include "srtrelay/metrics.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iomanip>
#include <sstream>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "srtrelay/relay_io.hpp"
#include "srtrelay/srt_utils.hpp"
#include "metrics_link_slots.hpp"

namespace srtrelay {

int64_t UnixNowMs() {
    const auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

namespace {
#ifndef NDEBUG
uint64_t CurrentThreadToken() {
    const uint64_t token = static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    return token == 0 ? 1 : token;
}
#endif
}  // namespace

MetricsState::LinkMetricsGuard::LinkMetricsGuard(MetricsState& metrics)
    : metrics_(&metrics), lock_(metrics.link_metrics_mutex) {
    metrics_->DebugOnLinkMetricsLocked();
}

MetricsState::LinkMetricsGuard::LinkMetricsGuard(const MetricsState& metrics)
    : LinkMetricsGuard(const_cast<MetricsState&>(metrics)) {}

MetricsState::LinkMetricsGuard::~LinkMetricsGuard() {
    if (!metrics_ || !lock_.owns_lock()) {
        return;
    }
    lock_.unlock();
    metrics_->DebugOnLinkMetricsUnlocked();
}

void MetricsState::AssertLinkMetricsLocked(const char* context) const {
    (void)context;
#ifndef NDEBUG
    const uint32_t depth = link_metrics_lock_depth_.load(std::memory_order_relaxed);
    const uint64_t owner = link_metrics_lock_owner_token_.load(std::memory_order_relaxed);
    assert(depth > 0);
    assert(owner == CurrentThreadToken());
#endif
}

void MetricsState::DebugOnLinkMetricsLocked() {
#ifndef NDEBUG
    const uint64_t token = CurrentThreadToken();
    const uint32_t depth = link_metrics_lock_depth_.load(std::memory_order_relaxed);
    const uint64_t owner = link_metrics_lock_owner_token_.load(std::memory_order_relaxed);
    if (depth == 0) {
        assert(owner == 0 || owner == token);
        link_metrics_lock_owner_token_.store(token, std::memory_order_relaxed);
    } else {
        assert(owner == token);
    }
    link_metrics_lock_depth_.fetch_add(1, std::memory_order_relaxed);
#endif
}

void MetricsState::DebugOnLinkMetricsUnlocked() {
#ifndef NDEBUG
    const uint64_t token = CurrentThreadToken();
    const uint32_t depth = link_metrics_lock_depth_.load(std::memory_order_relaxed);
    const uint64_t owner = link_metrics_lock_owner_token_.load(std::memory_order_relaxed);
    assert(depth > 0);
    assert(owner == token);
    const uint32_t new_depth = link_metrics_lock_depth_.fetch_sub(1, std::memory_order_relaxed) - 1;
    if (new_depth == 0) {
        link_metrics_lock_owner_token_.store(0, std::memory_order_relaxed);
    }
#endif
}

MetricsState::MetricsState() {
    for (size_t i = 0; i < kMaxInputSources; ++i) {
        input_source_connected[i].store(0, std::memory_order_relaxed);
        input_source_listening[i].store(0, std::memory_order_relaxed);
        input_source_bond_mode[i].store(0, std::memory_order_relaxed);
    }
    for (size_t i = 0; i < kMaxOutputSources; ++i) {
        output_source_connected[i].store(0, std::memory_order_relaxed);
        output_source_listening[i].store(0, std::memory_order_relaxed);
        output_source_bond_mode[i].store(0, std::memory_order_relaxed);
        output_listener_clients_active[i].store(0, std::memory_order_relaxed);
        output_listener_clients_accepted_total[i].store(0, std::memory_order_relaxed);
        output_listener_clients_dropped_timeout_total[i].store(0, std::memory_order_relaxed);
        output_listener_clients_dropped_disconnected_total[i].store(0, std::memory_order_relaxed);
        output_listener_clients_dropped_error_total[i].store(0, std::memory_order_relaxed);
        output_listener_accept_rejected_max_clients_total[i].store(0, std::memory_order_relaxed);
    }
    for (size_t side = 0; side < kFailureSides; ++side) {
        for (size_t i = 0; i < kTimeoutTypes; ++i) {
            timeouts_total[side][i].store(0, std::memory_order_relaxed);
        }
        for (size_t i = 0; i < kReasonCodes; ++i) {
            disconnects_total[side][i].store(0, std::memory_order_relaxed);
        }
        reconnect_attempts_total[side].store(0, std::memory_order_relaxed);
        active_attempt_id[side].store(0, std::memory_order_relaxed);
    }
}

namespace {

using json = nlohmann::json;

json BuildConnectedClientIpsJson(const json& connected_members);
json BuildConnectedClientIpsJson(const std::vector<std::string>& connected_endpoints);

const char* InputProtocolName(InputEndpointKind kind) {
    switch (kind) {
        case InputEndpointKind::kSrtListener:
        case InputEndpointKind::kSrtCaller:
            return "srt";
        case InputEndpointKind::kUdpListener:
        case InputEndpointKind::kUdpCaller:
            return "udp";
        case InputEndpointKind::kStdin:
            return "stdin";
    }
    return "unknown";
}

const char* OutputProtocolName(OutputEndpointKind kind) {
    switch (kind) {
        case OutputEndpointKind::kSrtCaller:
        case OutputEndpointKind::kSrtListener:
            return "srt";
        case OutputEndpointKind::kUdpCaller:
        case OutputEndpointKind::kUdpListener:
            return "udp";
        case OutputEndpointKind::kStdout:
            return "stdout";
    }
    return "unknown";
}

const char* InputModeName(InputEndpointKind kind) {
    switch (kind) {
        case InputEndpointKind::kSrtListener:
        case InputEndpointKind::kUdpListener:
            return "listener";
        case InputEndpointKind::kSrtCaller:
        case InputEndpointKind::kUdpCaller:
            return "caller";
        case InputEndpointKind::kStdin:
            return "stream";
    }
    return "unknown";
}

const char* OutputModeName(OutputEndpointKind kind) {
    switch (kind) {
        case OutputEndpointKind::kSrtCaller:
        case OutputEndpointKind::kUdpCaller:
            return "caller";
        case OutputEndpointKind::kSrtListener:
        case OutputEndpointKind::kUdpListener:
            return "listener";
        case OutputEndpointKind::kStdout:
            return "stream";
    }
    return "unknown";
}

const char* GroupTypeName(SRT_GROUP_TYPE group_type) {
    if (group_type == SRT_GTYPE_BROADCAST) return "broadcast";
    if (group_type == SRT_GTYPE_BACKUP) return "backup";
    return "undefined";
}

const char* SockStateName(int state) {
    if (state == SRTS_INIT) return "init";
    if (state == SRTS_OPENED) return "opened";
    if (state == SRTS_LISTENING) return "listening";
    if (state == SRTS_CONNECTING) return "connecting";
    if (state == SRTS_CONNECTED) return "connected";
    if (state == SRTS_BROKEN) return "broken";
    if (state == SRTS_CLOSING) return "closing";
    if (state == SRTS_CLOSED) return "closed";
    if (state == SRTS_NONEXIST) return "nonexist";
    return "unknown";
}

const char* GroupMemberStateName(int state) {
    if (state == SRT_GST_IDLE) return "idle";
    if (state == SRT_GST_RUNNING) return "running";
    if (state == SRT_GST_BROKEN) return "broken";
    return "unknown";
}

bool TryReadSockOptInt(SRTSOCKET sock, SRT_SOCKOPT opt, int* out_value) {
    if (sock == SRT_INVALID_SOCK || out_value == nullptr) {
        return false;
    }
    int value = 0;
    int len = sizeof(value);
    if (srt_getsockflag(sock, opt, &value, &len) == SRT_ERROR) {
        return false;
    }
    *out_value = value;
    return true;
}

bool TryReadSocketRttMs(SRTSOCKET sock, int64_t* out_rtt_ms) {
    if (sock == SRT_INVALID_SOCK || out_rtt_ms == nullptr) {
        return false;
    }
    SRT_TRACEBSTATS sock_stats {};
    if (srt_bstats(sock, &sock_stats, 0) == SRT_ERROR) {
        return false;
    }
    *out_rtt_ms = static_cast<int64_t>(sock_stats.msRTT);
    return true;
}

struct RuntimeLatencySnapshot {
    bool has_latency = false;
    int latency_ms = 0;
    bool has_rcvlatency = false;
    int rcvlatency_ms = 0;
    bool has_peerlatency = false;
    int peerlatency_ms = 0;
    bool has_negotiated_latency = false;
    int negotiated_latency_ms = 0;
};

RuntimeLatencySnapshot ReadRuntimeLatencySnapshot(SRTSOCKET sock) {
    RuntimeLatencySnapshot snapshot {};
    if (sock == SRT_INVALID_SOCK) {
        return snapshot;
    }
    snapshot.has_latency = TryReadSockOptInt(sock, SRTO_LATENCY, &snapshot.latency_ms);
    snapshot.has_rcvlatency = TryReadSockOptInt(sock, SRTO_RCVLATENCY, &snapshot.rcvlatency_ms);
    snapshot.has_peerlatency = TryReadSockOptInt(sock, SRTO_PEERLATENCY, &snapshot.peerlatency_ms);
    snapshot.has_negotiated_latency = snapshot.has_rcvlatency || snapshot.has_peerlatency;
    if (snapshot.has_rcvlatency && snapshot.has_peerlatency) {
        snapshot.negotiated_latency_ms = std::max(snapshot.rcvlatency_ms, snapshot.peerlatency_ms);
    } else if (snapshot.has_rcvlatency) {
        snapshot.negotiated_latency_ms = snapshot.rcvlatency_ms;
    } else if (snapshot.has_peerlatency) {
        snapshot.negotiated_latency_ms = snapshot.peerlatency_ms;
    }
    return snapshot;
}

int EffectiveLatencyMsOrMinusOne(const RuntimeLatencySnapshot& snapshot) {
    if (snapshot.has_negotiated_latency) {
        return snapshot.negotiated_latency_ms;
    }
    if (snapshot.has_latency) {
        return snapshot.latency_ms;
    }
    return -1;
}

bool TryReadRuntimeTranstype(SRTSOCKET sock, std::string* out_transtype) {
    if (sock == SRT_INVALID_SOCK || out_transtype == nullptr) {
        return false;
    }
    int transtype = 0;
    if (!TryReadSockOptInt(sock, SRTO_TRANSTYPE, &transtype)) {
        return false;
    }
    if (transtype == SRTT_LIVE) {
        *out_transtype = "live";
        return true;
    }
    if (transtype == SRTT_FILE) {
        *out_transtype = "file";
        return true;
    }
    *out_transtype = "unknown";
    return true;
}

bool TryResolveConfiguredTranstype(const std::vector<SrtUri>& uris, std::string* out_transtype) {
    if (out_transtype == nullptr) {
        return false;
    }
    for (const auto& uri : uris) {
        const auto it = uri.query.find("transtype");
        if (it == uri.query.end() || it->second.empty()) {
            continue;
        }
        *out_transtype = it->second;
        return true;
    }
    return false;
}

json BuildSessionTranstypeJson(SRTSOCKET sock, const std::vector<SrtUri>& uris) {
    std::string value;
    if (TryReadRuntimeTranstype(sock, &value)) {
        return json{{"value", value}, {"source", "runtime"}};
    }
    if (TryResolveConfiguredTranstype(uris, &value)) {
        return json{{"value", value}, {"source", "configured"}};
    }
    return json{{"value", "live"}, {"source", "default"}};
}

SRT_GROUP_TYPE ReadRuntimeGroupType(SRTSOCKET sock) {
    if (sock == SRT_INVALID_SOCK) {
        return SRT_GTYPE_UNDEFINED;
    }
    int group_type = 0;
    if (TryReadSockOptInt(sock, SRTO_GROUPTYPE, &group_type)) {
        return static_cast<SRT_GROUP_TYPE>(group_type);
    }
    const SRTSOCKET group_sock = srt_groupof(sock);
    if (group_sock == SRT_INVALID_SOCK) {
        return SRT_GTYPE_UNDEFINED;
    }
    if (TryReadSockOptInt(group_sock, SRTO_GROUPTYPE, &group_type)) {
        return static_cast<SRT_GROUP_TYPE>(group_type);
    }
    return SRT_GTYPE_UNDEFINED;
}

json BuildQueryMapJson(const std::map<std::string, std::string>& query) {
    json query_json = json::object();
    for (const auto& [key, value] : query) {
        query_json[key] = value;
    }
    return query_json;
}

json BuildSrtMembersJson(const std::vector<SrtUri>& uris) {
    json members = json::array();
    for (const auto& uri : uris) {
        members.push_back({
            {"host", uri.host},
            {"port", uri.port},
            {"query", BuildQueryMapJson(uri.query)},
        });
    }
    return members;
}

json BuildUdpEndpointJson(const UdpUri& uri) {
    return json{
        {"host", uri.host},
        {"port", uri.port},
        {"query", BuildQueryMapJson(uri.query)},
    };
}

json BuildRuntimeSrtOptionsJson(SRTSOCKET sock) {
    if (sock == SRT_INVALID_SOCK) {
        return nullptr;
    }
    const RuntimeLatencySnapshot latency = ReadRuntimeLatencySnapshot(sock);
    int oheadbw = 0;
    int peeridletimeo = 0;
    int conntimeo = 0;
    int rcvbuf = 0;
    int sndbuf = 0;
    int pbkeylen = 0;
    int grouptype = 0;
    const bool has_oheadbw = TryReadSockOptInt(sock, SRTO_OHEADBW, &oheadbw);
    const bool has_peeridletimeo = TryReadSockOptInt(sock, SRTO_PEERIDLETIMEO, &peeridletimeo);
    const bool has_conntimeo = TryReadSockOptInt(sock, SRTO_CONNTIMEO, &conntimeo);
    const bool has_rcvbuf = TryReadSockOptInt(sock, SRTO_RCVBUF, &rcvbuf);
    const bool has_sndbuf = TryReadSockOptInt(sock, SRTO_SNDBUF, &sndbuf);
    const bool has_pbkeylen = TryReadSockOptInt(sock, SRTO_PBKEYLEN, &pbkeylen);
    std::string runtime_transtype;
    const bool has_transtype = TryReadRuntimeTranstype(sock, &runtime_transtype);
    const bool has_grouptype = TryReadSockOptInt(sock, SRTO_GROUPTYPE, &grouptype);
    json out = {
        {"latency_ms", latency.has_latency ? json(latency.latency_ms) : json(nullptr)},
        {"rcvlatency_ms", latency.has_rcvlatency ? json(latency.rcvlatency_ms) : json(nullptr)},
        {"peerlatency_ms", latency.has_peerlatency ? json(latency.peerlatency_ms) : json(nullptr)},
        {"negotiated_latency_ms", latency.has_negotiated_latency ? json(latency.negotiated_latency_ms) : json(nullptr)},
        {"oheadbw_pct", has_oheadbw ? json(oheadbw) : json(nullptr)},
        {"peeridletimeo_ms", has_peeridletimeo ? json(peeridletimeo) : json(nullptr)},
        {"conntimeo_ms", has_conntimeo ? json(conntimeo) : json(nullptr)},
        {"rcvbuf_bytes", has_rcvbuf ? json(rcvbuf) : json(nullptr)},
        {"sndbuf_bytes", has_sndbuf ? json(sndbuf) : json(nullptr)},
        {"pbkeylen", has_pbkeylen ? json(pbkeylen) : json(nullptr)},
        {"group_type", has_grouptype ? json(GroupTypeName(static_cast<SRT_GROUP_TYPE>(grouptype))) : json(nullptr)},
        {"transtype", has_transtype ? json(runtime_transtype) : json(nullptr)},
    };
    return out;
}

bool TryExtractEndpoint(const sockaddr_storage& addr, std::string* out_host, int* out_port) {
    if (out_host == nullptr || out_port == nullptr) {
        return false;
    }
    char host_buf[INET6_ADDRSTRLEN] = {};
    if (addr.ss_family == AF_INET) {
        const auto* v4 = reinterpret_cast<const sockaddr_in*>(&addr);
        if (::inet_ntop(AF_INET, &(v4->sin_addr), host_buf, sizeof(host_buf)) == nullptr) {
            return false;
        }
        *out_host = host_buf;
        *out_port = static_cast<int>(ntohs(v4->sin_port));
        return true;
    }
    if (addr.ss_family == AF_INET6) {
        const auto* v6 = reinterpret_cast<const sockaddr_in6*>(&addr);
        if (::inet_ntop(AF_INET6, &(v6->sin6_addr), host_buf, sizeof(host_buf)) == nullptr) {
            return false;
        }
        *out_host = host_buf;
        *out_port = static_cast<int>(ntohs(v6->sin6_port));
        return true;
    }
    return false;
}

std::string EndpointStringFromHostPort(const std::string& host, int port) {
    if (host.empty() || port <= 0) {
        return "unknown";
    }
    return host + ":" + std::to_string(port);
}

bool TryGetSocketEndpoint(SRTSOCKET sock, bool local, std::string* endpoint) {
    if (endpoint == nullptr || sock == SRT_INVALID_SOCK || sock == 0) {
        return false;
    }
    sockaddr_storage addr {};
    int addr_len = static_cast<int>(sizeof(addr));
    const int rc = local
        ? srt_getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &addr_len)
        : srt_getpeername(sock, reinterpret_cast<sockaddr*>(&addr), &addr_len);
    if (rc == SRT_ERROR) {
        return false;
    }
    std::string host;
    int port = 0;
    if (!TryExtractEndpoint(addr, &host, &port)) {
        return false;
    }
    *endpoint = EndpointStringFromHostPort(host, port);
    return true;
}

json BuildConnectedMembersJson(SRTSOCKET session_sock);

json BuildLastFailureJson(const LastFailureSnapshot& snapshot) {
    if (snapshot.timestamp_unix_seconds <= 0) {
        return nullptr;
    }
    return json{
        {"timestamp", snapshot.timestamp_unix_seconds},
        {"reason_code", snapshot.reason_code},
        {"reason_class", snapshot.reason_class},
        {"reason_detail", snapshot.reason_detail},
        {"incident_id", snapshot.incident_id.empty() ? json(nullptr) : json(snapshot.incident_id)},
        {"attempt_id", snapshot.attempt_id == 0 ? json(nullptr) : json(snapshot.attempt_id)},
        {"source", snapshot.source},
    };
}

json BuildInputSessionSpecJson(const InputEndpointSpec& spec,
                               const MetricsState& metrics,
                               size_t input_index,
                               bool runtime_active) {
    bool connected = false;
    bool listening = false;
    if (input_index < MetricsState::kMaxInputSources) {
        connected = metrics.input_source_connected[input_index].load(std::memory_order_relaxed) == 1;
        listening = metrics.input_source_listening[input_index].load(std::memory_order_relaxed) == 1;
    } else {
        connected = metrics.input_connected.load(std::memory_order_relaxed) == 1;
        listening = metrics.input_listening.load(std::memory_order_relaxed) == 1;
    }
    const SRTSOCKET socket_id = runtime_active
        ? static_cast<SRTSOCKET>(metrics.input_session_socket_id.load(std::memory_order_relaxed))
        : SRT_INVALID_SOCK;
    const SRTSOCKET group_socket_id =
        (runtime_active && socket_id != SRT_INVALID_SOCK) ? srt_groupof(socket_id) : SRT_INVALID_SOCK;
    json configured = {
        {"bonded", spec.bonded},
        {"group_type", GroupTypeName(spec.group_type)},
    };
    if (spec.kind == InputEndpointKind::kSrtListener || spec.kind == InputEndpointKind::kSrtCaller) {
        configured["members"] = BuildSrtMembersJson(spec.uris);
    } else if (spec.kind == InputEndpointKind::kUdpListener || spec.kind == InputEndpointKind::kUdpCaller) {
        configured["endpoint"] = BuildUdpEndpointJson(spec.udp_uri);
    }
    json effective_srt = nullptr;
    json runtime_transtype = nullptr;
    json session_transtype = nullptr;
    if (runtime_active &&
        (spec.kind == InputEndpointKind::kSrtListener || spec.kind == InputEndpointKind::kSrtCaller)) {
        effective_srt = BuildRuntimeSrtOptionsJson(socket_id);
        std::string runtime_value;
        if (TryReadRuntimeTranstype(socket_id, &runtime_value)) {
            runtime_transtype = runtime_value;
        }
        session_transtype = BuildSessionTranstypeJson(socket_id, spec.uris);
    }
    json connected_members = json::array();
    if (runtime_active &&
        (spec.kind == InputEndpointKind::kSrtListener || spec.kind == InputEndpointKind::kSrtCaller)) {
        connected_members = BuildConnectedMembersJson(socket_id);
    }
    const SRT_GROUP_TYPE runtime_group_type = runtime_active ? ReadRuntimeGroupType(socket_id) : spec.group_type;
    const bool runtime_bonded = runtime_active
        ? (runtime_group_type == SRT_GTYPE_BROADCAST ||
           runtime_group_type == SRT_GTYPE_BACKUP ||
           connected_members.size() > 1)
        : spec.bonded;
    LastFailureSnapshot last_failure;
    if (runtime_active) {
        std::lock_guard<std::mutex> causality_lock(metrics.causality_mutex);
        last_failure = metrics.input_last_failure;
    }
    return json{
        {"protocol", InputProtocolName(spec.kind)},
        {"mode", InputModeName(spec.kind)},
        {"connected", connected},
        {"listening", listening},
        {"socket_id", socket_id == SRT_INVALID_SOCK ? json(nullptr) : json(socket_id)},
        {"group_socket_id", group_socket_id == SRT_INVALID_SOCK ? json(nullptr) : json(group_socket_id)},
        {"configured", configured},
        {"runtime", json{
            {"bonded", runtime_bonded},
            {"group_type", GroupTypeName(runtime_group_type)},
        }},
        {"effective_srt", effective_srt},
        {"runtime_transtype", runtime_transtype},
        {"session_transtype", session_transtype},
        {"connected_members", connected_members},
        {"last_failure", BuildLastFailureJson(last_failure)},
    };
}

json BuildOutputSessionSpecJson(const OutputEndpointSpec& spec,
                                const MetricsState& metrics,
                                size_t output_index) {
    bool connected = metrics.output_connected.load(std::memory_order_relaxed) == 1;
    bool listening = false;
    int64_t connected_clients = 0;
    if (output_index < MetricsState::kMaxOutputSources) {
        connected = metrics.output_source_connected[output_index].load(std::memory_order_relaxed) == 1;
        listening = metrics.output_source_listening[output_index].load(std::memory_order_relaxed) == 1;
        connected_clients = metrics.output_listener_clients_active[output_index].load(std::memory_order_relaxed);
    }
    SRTSOCKET socket_id = static_cast<SRTSOCKET>(metrics.output_transport_socket_id.load(std::memory_order_relaxed));
    if (output_index > 0) {
        socket_id = SRT_INVALID_SOCK;
    }
    const SRTSOCKET group_socket_id = (socket_id != SRT_INVALID_SOCK) ? srt_groupof(socket_id) : SRT_INVALID_SOCK;
    json configured = {
        {"bonded", spec.bonded},
        {"group_type", GroupTypeName(spec.group_type)},
    };
    if (spec.kind == OutputEndpointKind::kSrtListener || spec.kind == OutputEndpointKind::kSrtCaller) {
        configured["members"] = BuildSrtMembersJson(spec.uris);
        if (spec.kind == OutputEndpointKind::kSrtListener) {
            configured["fanout"] = spec.listener_fanout_enabled;
            configured["max_clients"] = spec.listener_max_clients;
        }
    } else if (spec.kind == OutputEndpointKind::kUdpListener || spec.kind == OutputEndpointKind::kUdpCaller) {
        configured["endpoint"] = BuildUdpEndpointJson(spec.udp_uri);
    }
    json effective_srt = nullptr;
    json runtime_transtype = nullptr;
    json session_transtype = nullptr;
    if (spec.kind == OutputEndpointKind::kSrtListener || spec.kind == OutputEndpointKind::kSrtCaller) {
        effective_srt = BuildRuntimeSrtOptionsJson(socket_id);
        std::string runtime_value;
        if (TryReadRuntimeTranstype(socket_id, &runtime_value)) {
            runtime_transtype = runtime_value;
        }
        session_transtype = BuildSessionTranstypeJson(socket_id, spec.uris);
    }
    json connected_members = json::array();
    if (spec.kind == OutputEndpointKind::kSrtListener || spec.kind == OutputEndpointKind::kSrtCaller) {
        connected_members = BuildConnectedMembersJson(socket_id);
    }
    json connected_client_ips = json(nullptr);
    if (spec.kind == OutputEndpointKind::kSrtListener) {
        std::vector<std::string> connected_endpoints;
        {
            std::lock_guard<std::mutex> session_lock(metrics.session_specs_mutex);
            if (output_index < MetricsState::kMaxOutputSources) {
                connected_endpoints = metrics.output_listener_connected_endpoints[output_index];
            }
        }
        connected_client_ips = BuildConnectedClientIpsJson(connected_endpoints);
        if (connected_client_ips.empty()) {
            connected_client_ips = BuildConnectedClientIpsJson(connected_members);
        }
    }
    const SRT_GROUP_TYPE runtime_group_type = ReadRuntimeGroupType(socket_id);
    const bool runtime_bonded = (runtime_group_type == SRT_GTYPE_BROADCAST ||
                                 runtime_group_type == SRT_GTYPE_BACKUP ||
                                 connected_members.size() > 1);
    LastFailureSnapshot last_failure;
    {
        std::lock_guard<std::mutex> causality_lock(metrics.causality_mutex);
        last_failure = metrics.output_last_failure;
    }
    return json{
        {"protocol", OutputProtocolName(spec.kind)},
        {"mode", OutputModeName(spec.kind)},
        {"connected", connected},
        {"listening", listening},
        {"connected_clients", spec.kind == OutputEndpointKind::kSrtListener ? json(connected_clients) : json(nullptr)},
        {"connected_client_ips", connected_client_ips},
        {"socket_id", socket_id == SRT_INVALID_SOCK ? json(nullptr) : json(socket_id)},
        {"group_socket_id", group_socket_id == SRT_INVALID_SOCK ? json(nullptr) : json(group_socket_id)},
        {"configured", configured},
        {"runtime", json{
            {"bonded", runtime_bonded},
            {"group_type", GroupTypeName(runtime_group_type)},
        }},
        {"effective_srt", effective_srt},
        {"runtime_transtype", runtime_transtype},
        {"session_transtype", session_transtype},
        {"connected_members", connected_members},
        {"last_failure", BuildLastFailureJson(last_failure)},
    };
}

std::string BuildSessionSpecsJson(const std::vector<InputEndpointSpec>& input_specs,
                                  const std::vector<OutputEndpointSpec>& output_specs,
                                  const Config& cfg,
                                  const MetricsState& metrics) {
    json inputs = json::array();
    int64_t active_input_index = metrics.active_input_index.load(std::memory_order_relaxed);
    if (active_input_index <= 0) {
        active_input_index = 1;
    }
    for (size_t i = 0; i < input_specs.size(); ++i) {
        const bool runtime_active = static_cast<int64_t>(i + 1) == active_input_index;
        json input_json = BuildInputSessionSpecJson(input_specs[i], metrics, i, runtime_active);
        input_json["input_index"] = static_cast<int64_t>(i + 1);
        input_json["active"] = runtime_active;
        inputs.push_back(std::move(input_json));
    }
    json outputs = json::array();
    for (size_t i = 0; i < output_specs.size(); ++i) {
        json output_json = BuildOutputSessionSpecJson(output_specs[i], metrics, i);
        output_json["output_index"] = static_cast<int64_t>(i + 1);
        outputs.push_back(std::move(output_json));
    }

    const int64_t primary_input_index = cfg.primary_input_index.has_value()
        ? static_cast<int64_t>(*cfg.primary_input_index + 1)
        : 0;
    const char* switch_policy = cfg.primary_input_index.has_value()
        ? InputSwitchPolicyName(InputSwitchPolicy::kPreferredPrimary)
        : InputSwitchPolicyName(InputSwitchPolicy::kRoundRobin);
    json out = {
        {"inputs", inputs},
        {"active_input_index", active_input_index},
        {"primary_input_index", primary_input_index == 0 ? json(nullptr) : json(primary_input_index)},
        {"switch_policy", switch_policy},
        {"switch_mode", SwitchModeName(cfg.switch_mode)},
        {"output", output_specs.empty() ? json(nullptr) : BuildOutputSessionSpecJson(output_specs.front(), metrics, 0)},
        {"outputs", outputs},
    };
    return out.dump();
}

void MaybeUpdateRttMetric(SRTSOCKET sock, std::atomic<int64_t>* out_rtt_ms) {
    if (sock == SRT_INVALID_SOCK) {
        out_rtt_ms->store(-1, std::memory_order_relaxed);
        return;
    }

    SRT_TRACEBSTATS sock_stats {};
    if (srt_bstats(sock, &sock_stats, 0) == SRT_ERROR) {
        return;
    }
    out_rtt_ms->store(static_cast<int64_t>(sock_stats.msRTT), std::memory_order_relaxed);
}

int ReadBondModeForSocket(SRTSOCKET sock) {
    if (sock == SRT_INVALID_SOCK) {
        return 0;
    }
    int group_type = static_cast<int>(SRT_GTYPE_UNDEFINED);
    int opt_len = sizeof(group_type);
    if (srt_getsockflag(sock, SRTO_GROUPTYPE, &group_type, &opt_len) == SRT_ERROR) {
        return 0;
    }
    if (group_type == static_cast<int>(SRT_GTYPE_BROADCAST)) {
        return 1;
    }
    if (group_type == static_cast<int>(SRT_GTYPE_BACKUP)) {
        return 2;
    }
    return 0;
}

void UpdateInputBondModeMetric(SRTSOCKET input_session_sock, MetricsState* metrics) {
    if (input_session_sock == SRT_INVALID_SOCK) {
        metrics->input_bond_mode.store(0, std::memory_order_relaxed);
        return;
    }
    int mode = ReadBondModeForSocket(input_session_sock);
    if (mode == 0) {
        const SRTSOCKET input_group_sock = srt_groupof(input_session_sock);
        mode = ReadBondModeForSocket(input_group_sock);
    }
    metrics->input_bond_mode.store(mode, std::memory_order_relaxed);
}

void UpdateOutputBondModeMetric(SRTSOCKET output_sock, MetricsState* metrics) {
    if (output_sock == SRT_INVALID_SOCK) {
        metrics->output_bond_mode.store(0, std::memory_order_relaxed);
        return;
    }
    int mode = ReadBondModeForSocket(output_sock);
    if (mode == 0) {
        const SRTSOCKET output_group_sock = srt_groupof(output_sock);
        mode = ReadBondModeForSocket(output_group_sock);
    }
    metrics->output_bond_mode.store(mode, std::memory_order_relaxed);
}

bool IsHealthyGroupMember(const SRT_SOCKGROUPDATA& member) {
    if (member.memberstate == SRT_GST_BROKEN) {
        return false;
    }
    return member.sockstate != SRTS_BROKEN &&
           member.sockstate != SRTS_CLOSING &&
           member.sockstate != SRTS_CLOSED &&
           member.sockstate != SRTS_NONEXIST;
}

bool IsConnectedGroupMember(const SRT_SOCKGROUPDATA& member) {
    if (member.memberstate == SRT_GST_RUNNING) {
        return true;
    }
    return member.sockstate == SRTS_CONNECTED;
}

enum class MemberLinkState {
    kDown = 0,
    kUp = 1,
    kRunning = 2,
    kBroken = 3,
};

MemberLinkState DeriveMemberLinkState(int sock_state, int member_state, int connected) {
    if (member_state == SRT_GST_BROKEN ||
        sock_state == SRTS_BROKEN ||
        sock_state == SRTS_CLOSING ||
        sock_state == SRTS_CLOSED ||
        sock_state == SRTS_NONEXIST) {
        return MemberLinkState::kBroken;
    }
    if (member_state == SRT_GST_RUNNING) {
        return MemberLinkState::kRunning;
    }
    if (connected == 1 || sock_state == SRTS_CONNECTED) {
        return MemberLinkState::kUp;
    }
    return MemberLinkState::kDown;
}

const char* MemberLinkStateName(MemberLinkState state) {
    switch (state) {
        case MemberLinkState::kDown:
            return "down";
        case MemberLinkState::kUp:
            return "up";
        case MemberLinkState::kRunning:
            return "running";
        case MemberLinkState::kBroken:
            return "broken";
    }
    return "down";
}

bool HasUsablePeerAddress(const SRT_SOCKGROUPDATA& member) {
    return member.peeraddr.ss_family == AF_INET || member.peeraddr.ss_family == AF_INET6;
}

bool IsUsableGroupMemberSnapshot(const SRT_SOCKGROUPDATA& member) {
    if (member.id != SRT_INVALID_SOCK && member.id != 0) {
        return true;
    }
    if (HasUsablePeerAddress(member)) {
        return true;
    }
    if (member.token != 0) {
        return true;
    }
    return false;
}

void EmitMemberTransitionEvents(const Logger& logger,
                                MetricsState* metrics,
                                const std::string& active_incident_id) {
    struct TransitionEvent {
        const char* side = "input";
        size_t member_index = 0;  // 1-based stable slot index
        int64_t socket_id = SRT_INVALID_SOCK;
        std::string peer_host;
        int peer_port = 0;
        MemberLinkState prior_state = MemberLinkState::kDown;
        MemberLinkState new_state = MemberLinkState::kDown;
        int prior_connected = 0;
        int new_connected = 0;
    };

    std::vector<TransitionEvent> events;
    {
        MetricsState::LinkMetricsGuard lock(*metrics);
        auto inspect_side = [&](const char* side_name, auto& slots) {
                for (size_t i = 0; i < MetricsState::kMaxTrackedMembers; ++i) {
                    if (slots[i].member_identity_key == 0) {
                        slots[i].member_status_last_logged = -1;
                        slots[i].member_connected_last_logged = -1;
                        continue;
                    }
                    const int current_connected = slots[i].member_connected;
                    const int current_sock_state = slots[i].member_sock_state;
                    const int current_group_state = slots[i].member_group_state;
                    const MemberLinkState current_state =
                        DeriveMemberLinkState(current_sock_state, current_group_state, current_connected);
                    const int current_state_i = static_cast<int>(current_state);
                    const int previous_state_i = slots[i].member_status_last_logged;
                    const int previous_connected = slots[i].member_connected_last_logged;

                    const bool transitioned = previous_state_i >= 0 &&
                        (previous_state_i != current_state_i || previous_connected != current_connected);
                    const bool first_seen_connected = previous_state_i < 0 && current_connected == 1;
                    if (transitioned || first_seen_connected) {
                        TransitionEvent event;
                        event.side = side_name;
                        event.member_index = i + 1;
                        event.socket_id = slots[i].member_id;
                        event.peer_host = slots[i].member_peer_host;
                        event.peer_port = slots[i].member_peer_port;
                        event.prior_state = previous_state_i >= 0
                            ? static_cast<MemberLinkState>(previous_state_i)
                            : MemberLinkState::kDown;
                        event.new_state = current_state;
                        event.prior_connected = previous_connected >= 0 ? previous_connected : 0;
                        event.new_connected = current_connected;
                        events.push_back(std::move(event));
                    }
                    slots[i].member_status_last_logged = current_state_i;
                    slots[i].member_connected_last_logged = current_connected;
                }
            };

        inspect_side("input", metrics->input_tracked.slots);
        inspect_side("output", metrics->output_tracked.slots);
    }

    for (const auto& event : events) {
        std::string remote_endpoint = EndpointStringFromHostPort(event.peer_host, event.peer_port);
        if (remote_endpoint == "unknown") {
            (void)TryGetSocketEndpoint(static_cast<SRTSOCKET>(event.socket_id), false, &remote_endpoint);
        }
        std::string local_endpoint = "unknown";
        (void)TryGetSocketEndpoint(static_cast<SRTSOCKET>(event.socket_id), true, &local_endpoint);

        logger.Log(LogLevel::kDebug,
                   "member-transition",
                   std::string("side=") + event.side,
                   "member_index=" + std::to_string(event.member_index),
                   "member_socket_id=" + std::to_string(event.socket_id),
                   "local_endpoint=" + local_endpoint,
                   "remote_endpoint=" + remote_endpoint,
                   std::string("prior_state=") + MemberLinkStateName(event.prior_state),
                   std::string("new_state=") + MemberLinkStateName(event.new_state),
                   "prior_connected=" + std::to_string(event.prior_connected),
                   "new_connected=" + std::to_string(event.new_connected),
                   "incident_id=" + (active_incident_id.empty() ? std::string("none") : active_incident_id));

        const bool became_connected = event.prior_connected != 1 && event.new_connected == 1;
        const bool became_disconnected = event.prior_connected == 1 && event.new_connected != 1;
        if (became_connected) {
            const std::string event_name = std::string(event.side) + "-member-connected";
            logger.Log(LogLevel::kInfo,
                       event_name.c_str(),
                       "member_index=" + std::to_string(event.member_index),
                       "member_socket_id=" + std::to_string(event.socket_id),
                       "local_endpoint=" + local_endpoint,
                       "remote_endpoint=" + remote_endpoint,
                       "incident_id=" + (active_incident_id.empty() ? std::string("none") : active_incident_id));
        } else if (became_disconnected) {
            const std::string event_name = std::string(event.side) + "-member-disconnected";
            logger.Log(LogLevel::kInfo,
                       event_name.c_str(),
                       "member_index=" + std::to_string(event.member_index),
                       "member_socket_id=" + std::to_string(event.socket_id),
                       "local_endpoint=" + local_endpoint,
                       "remote_endpoint=" + remote_endpoint,
                       "incident_id=" + (active_incident_id.empty() ? std::string("none") : active_incident_id));
        }
    }
}

bool TryGetPeerAddrForSocket(SRTSOCKET sock, sockaddr_storage* out_peer_addr) {
    if (sock == SRT_INVALID_SOCK || out_peer_addr == nullptr) {
        return false;
    }
    sockaddr_storage peer_addr {};
    int peer_len = static_cast<int>(sizeof(peer_addr));
    if (srt_getpeername(sock, reinterpret_cast<sockaddr*>(&peer_addr), &peer_len) == SRT_ERROR) {
        return false;
    }
    if (peer_len <= 0) {
        return false;
    }
    *out_peer_addr = peer_addr;
    return true;
}

std::vector<SRT_SOCKGROUPDATA> BuildSingleSocketFallbackSnapshot(SRTSOCKET sock) {
    std::vector<SRT_SOCKGROUPDATA> members;
    if (sock == SRT_INVALID_SOCK) {
        return members;
    }

    SRT_SOCKGROUPDATA member {};
    member.id = sock;
    member.sockstate = SRTS_CONNECTED;
    member.memberstate = SRT_GST_RUNNING;
    if (!TryGetPeerAddrForSocket(sock, &member.peeraddr)) {
        member.peeraddr.ss_family = AF_UNSPEC;
    }
    members.push_back(member);
    return members;
}

bool TryReadGroupMembers(SRTSOCKET group_or_session_sock, std::vector<SRT_SOCKGROUPDATA>* out_members) {
    if (group_or_session_sock == SRT_INVALID_SOCK || out_members == nullptr) {
        return false;
    }
    SRT_SOCKGROUPDATA members[MetricsState::kMaxTrackedMembers] {};
    size_t capacity = MetricsState::kMaxTrackedMembers;
    const int sz = srt_group_data(group_or_session_sock, members, &capacity);
    if (sz <= 0) {
        return false;
    }
    out_members->clear();
    out_members->reserve(static_cast<size_t>(sz));
    for (int i = 0; i < sz; ++i) {
        if (!IsUsableGroupMemberSnapshot(members[i])) {
            continue;
        }
        out_members->push_back(members[i]);
    }
    return !out_members->empty();
}

json BuildConnectedMembersJson(SRTSOCKET session_sock) {
    json members_json = json::array();
    if (session_sock == SRT_INVALID_SOCK) {
        return members_json;
    }

    SRTSOCKET group_sock = srt_groupof(session_sock);
    if (group_sock == SRT_INVALID_SOCK) {
        group_sock = session_sock;
    }

    std::vector<SRT_SOCKGROUPDATA> members;
    if (!TryReadGroupMembers(group_sock, &members)) {
        if (!TryReadGroupMembers(session_sock, &members)) {
            members = BuildSingleSocketFallbackSnapshot(session_sock);
        }
    }

    for (const auto& member : members) {
        if (!IsConnectedGroupMember(member)) {
            continue;
        }
        const bool has_socket_id = member.id != SRT_INVALID_SOCK && member.id != 0;
        int64_t member_rtt_ms = -1;
        const bool has_member_rtt = has_socket_id && TryReadSocketRttMs(member.id, &member_rtt_ms);
        std::string peer_host;
        int peer_port = 0;
        const bool has_peer_endpoint = TryExtractEndpoint(member.peeraddr, &peer_host, &peer_port);

        json member_json = {
            {"socket_id", has_socket_id ? json(member.id) : json(nullptr)},
            {"token", member.token != 0 ? json(member.token) : json(nullptr)},
            {"sock_state", SockStateName(member.sockstate)},
            {"sock_state_code", static_cast<int>(member.sockstate)},
            {"member_state", GroupMemberStateName(member.memberstate)},
            {"member_state_code", static_cast<int>(member.memberstate)},
            {"peer_host", has_peer_endpoint ? json(peer_host) : json(nullptr)},
            {"peer_port", has_peer_endpoint ? json(peer_port) : json(nullptr)},
            {"rtt_ms", has_member_rtt ? json(member_rtt_ms) : json(nullptr)},
        };
        members_json.push_back(std::move(member_json));
    }

    return members_json;
}

json BuildConnectedClientIpsJson(const json& connected_members) {
    json client_ips = json::array();
    std::unordered_set<std::string> seen_endpoints;
    for (const auto& member : connected_members) {
        if (!member.is_object() || !member.contains("peer_host")) {
            continue;
        }
        const auto& peer_host = member["peer_host"];
        if (!peer_host.is_string()) {
            continue;
        }
        const std::string host = peer_host.get<std::string>();
        if (host.empty()) {
            continue;
        }
        std::string endpoint = host;
        if (member.contains("peer_port") && member["peer_port"].is_number_integer()) {
            endpoint += ":" + std::to_string(member["peer_port"].get<int>());
        }
        if (seen_endpoints.insert(endpoint).second) {
            client_ips.push_back(endpoint);
        }
    }
    return client_ips;
}

json BuildConnectedClientIpsJson(const std::vector<std::string>& connected_endpoints) {
    json client_ips = json::array();
    std::unordered_set<std::string> seen_endpoints;
    for (const auto& endpoint : connected_endpoints) {
        if (endpoint.empty()) {
            continue;
        }
        if (seen_endpoints.insert(endpoint).second) {
            client_ips.push_back(endpoint);
        }
    }
    return client_ips;
}

void UpdateInputLinkHealthFromGroupMembers(const std::vector<SRT_SOCKGROUPDATA>& group_members,
                                           MetricsState* metrics) {
    int64_t healthy_count = 0;
    int64_t running_count = 0;
    for (const auto& member : group_members) {
        if (IsHealthyGroupMember(member)) {
            healthy_count++;
        }
        if (member.memberstate == SRT_GST_RUNNING || member.sockstate == SRTS_CONNECTED) {
            running_count++;
        }
    }
    metrics->input_links_total.store(static_cast<int64_t>(group_members.size()), std::memory_order_relaxed);
    metrics->input_links_healthy.store(healthy_count, std::memory_order_relaxed);
    metrics->input_links_running.store(running_count, std::memory_order_relaxed);
}

void SaveInputMemberSnapshot(const std::vector<SRT_SOCKGROUPDATA>& group_members, MetricsState* metrics);
void SaveOutputMemberSnapshot(const std::vector<SRT_SOCKGROUPDATA>& group_members, MetricsState* metrics);

void UpdateInputLinkHealthFallbackSingleSocket(SRTSOCKET input_session_sock, MetricsState* metrics) {
    if (input_session_sock == SRT_INVALID_SOCK) {
        metrics->input_links_total.store(0, std::memory_order_relaxed);
        metrics->input_links_healthy.store(0, std::memory_order_relaxed);
        metrics->input_links_running.store(0, std::memory_order_relaxed);
        ClearTrackedMembersForDisconnectedSocket(LinkSide::kInput, metrics);
        return;
    }
    auto members = BuildSingleSocketFallbackSnapshot(input_session_sock);
    UpdateInputLinkHealthFromGroupMembers(members, metrics);
    SaveInputMemberSnapshot(members, metrics);
}

void SaveInputMemberSnapshot(const std::vector<SRT_SOCKGROUPDATA>& group_members, MetricsState* metrics) {
    SaveMemberSnapshot(LinkSide::kInput, group_members, metrics);
}

void UpdateOutputLinkHealthFromGroupMembers(const std::vector<SRT_SOCKGROUPDATA>& group_members,
                                            MetricsState* metrics) {
    int64_t healthy_count = 0;
    int64_t running_count = 0;
    for (const auto& member : group_members) {
        if (IsHealthyGroupMember(member)) {
            healthy_count++;
        }
        if (member.memberstate == SRT_GST_RUNNING || member.sockstate == SRTS_CONNECTED) {
            running_count++;
        }
    }
    metrics->output_links_total.store(static_cast<int64_t>(group_members.size()), std::memory_order_relaxed);
    metrics->output_links_healthy.store(healthy_count, std::memory_order_relaxed);
    metrics->output_links_running.store(running_count, std::memory_order_relaxed);
}

void SaveOutputMemberSnapshot(const std::vector<SRT_SOCKGROUPDATA>& group_members, MetricsState* metrics) {
    SaveMemberSnapshot(LinkSide::kOutput, group_members, metrics);
}

void UpdateOutputLinkHealthFallbackSingleSocket(SRTSOCKET output_sock, MetricsState* metrics) {
    if (output_sock == SRT_INVALID_SOCK) {
        metrics->output_links_total.store(0, std::memory_order_relaxed);
        metrics->output_links_healthy.store(0, std::memory_order_relaxed);
        metrics->output_links_running.store(0, std::memory_order_relaxed);
        ClearTrackedMembersForDisconnectedSocket(LinkSide::kOutput, metrics);
        return;
    }
    auto members = BuildSingleSocketFallbackSnapshot(output_sock);
    UpdateOutputLinkHealthFromGroupMembers(members, metrics);
    SaveOutputMemberSnapshot(members, metrics);
}

enum class CompactDirection {
    kInput,
    kOutput,
    kBoth,
};

bool ParseCompactDirection(const httplib::Request& req, CompactDirection* out_direction) {
    if (out_direction == nullptr) {
        return false;
    }
    if (!req.has_param("direction")) {
        *out_direction = CompactDirection::kBoth;
        return true;
    }
    const std::string value = req.get_param_value("direction");
    if (value == "input") {
        *out_direction = CompactDirection::kInput;
        return true;
    }
    if (value == "output") {
        *out_direction = CompactDirection::kOutput;
        return true;
    }
    if (value == "both") {
        *out_direction = CompactDirection::kBoth;
        return true;
    }
    return false;
}

CompactResult CompactInputSlotsLocked(MetricsState* metrics) {
    return CompactSlotsLocked(LinkSide::kInput, metrics);
}

CompactResult CompactOutputSlotsLocked(MetricsState* metrics) {
    return CompactSlotsLocked(LinkSide::kOutput, metrics);
}

}  // namespace

std::string BuildInputLinkStatusCompact(const MetricsState& metrics) {
    return BuildLinkStatusCompact(LinkSide::kInput, metrics);
}

std::string BuildOutputLinkStatusCompact(const MetricsState& metrics) {
    return BuildLinkStatusCompact(LinkSide::kOutput, metrics);
}

void MarkAllTrackedInputLinksDisconnected(MetricsState* metrics) {
    MarkAllTrackedLinksDisconnected(LinkSide::kInput, metrics);
}

void MarkAllTrackedOutputLinksDisconnected(MetricsState* metrics) {
    MarkAllTrackedLinksDisconnected(LinkSide::kOutput, metrics);
}

void ResetInputTrackingMetrics(MetricsState* metrics) {
    if (metrics == nullptr) return;
    metrics->input_links_total.store(0, std::memory_order_relaxed);
    metrics->input_links_healthy.store(0, std::memory_order_relaxed);
    metrics->input_links_running.store(0, std::memory_order_relaxed);
    MarkAllTrackedInputLinksDisconnected(metrics);
    metrics->input_transport_members_total.store(0, std::memory_order_relaxed);
}

void ResetOutputTrackingMetrics(MetricsState* metrics) {
    if (metrics == nullptr) return;
    metrics->output_links_total.store(0, std::memory_order_relaxed);
    metrics->output_links_healthy.store(0, std::memory_order_relaxed);
    metrics->output_links_running.store(0, std::memory_order_relaxed);
    MarkAllTrackedOutputLinksDisconnected(metrics);
    metrics->output_transport_members_total.store(0, std::memory_order_relaxed);
}

namespace {

void UpdateInputLinkHealthMetrics(SRTSOCKET input_session_sock, MetricsState* metrics) {
    if (input_session_sock == SRT_INVALID_SOCK) {
        metrics->input_links_total.store(0, std::memory_order_relaxed);
        metrics->input_links_healthy.store(0, std::memory_order_relaxed);
        metrics->input_links_running.store(0, std::memory_order_relaxed);
        MarkAllTrackedInputLinksDisconnected(metrics);
        metrics->input_transport_members_total.store(0, std::memory_order_relaxed);
        return;
    }

    SRTSOCKET group_sock = srt_groupof(input_session_sock);
    if (group_sock == SRT_INVALID_SOCK) {
        group_sock = input_session_sock;
    }
    std::vector<SRT_SOCKGROUPDATA> group_members;
    if (!TryReadGroupMembers(group_sock, &group_members)) {
        if (!TryReadGroupMembers(input_session_sock, &group_members)) {
            UpdateInputLinkHealthFallbackSingleSocket(input_session_sock, metrics);
            return;
        }
    }

    if (group_members.empty()) {
        UpdateInputLinkHealthFallbackSingleSocket(input_session_sock, metrics);
        return;
    }
    UpdateInputLinkHealthFromGroupMembers(group_members, metrics);
    SaveInputMemberSnapshot(group_members, metrics);
}

void UpdateOutputLinkHealthMetrics(SRTSOCKET output_sock, MetricsState* metrics) {
    if (output_sock == SRT_INVALID_SOCK) {
        metrics->output_links_total.store(0, std::memory_order_relaxed);
        metrics->output_links_healthy.store(0, std::memory_order_relaxed);
        metrics->output_links_running.store(0, std::memory_order_relaxed);
        MarkAllTrackedOutputLinksDisconnected(metrics);
        metrics->output_transport_members_total.store(0, std::memory_order_relaxed);
        return;
    }

    SRTSOCKET group_sock = srt_groupof(output_sock);
    if (group_sock == SRT_INVALID_SOCK) {
        group_sock = output_sock;
    }
    std::vector<SRT_SOCKGROUPDATA> group_members;
    if (!TryReadGroupMembers(group_sock, &group_members)) {
        if (!TryReadGroupMembers(output_sock, &group_members)) {
            UpdateOutputLinkHealthFallbackSingleSocket(output_sock, metrics);
            return;
        }
    }

    if (group_members.empty()) {
        UpdateOutputLinkHealthFallbackSingleSocket(output_sock, metrics);
        return;
    }
    UpdateOutputLinkHealthFromGroupMembers(group_members, metrics);
    SaveOutputMemberSnapshot(group_members, metrics);
}

}  // namespace

void UpdateInputLinkHealthFromMsgCtrl(const SRT_MSGCTRL& rx_ctrl, MetricsState* metrics) {
    if (rx_ctrl.grpdata == nullptr || rx_ctrl.grpdata_size <= 0) {
        return;
    }
    const size_t capped = std::min(static_cast<size_t>(rx_ctrl.grpdata_size), MetricsState::kMaxTrackedMembers);
    std::vector<SRT_SOCKGROUPDATA> group_members;
    group_members.reserve(capped);
    for (size_t i = 0; i < capped; ++i) {
        const auto& member = rx_ctrl.grpdata[i];
        if (!IsUsableGroupMemberSnapshot(member)) continue;
        group_members.push_back(member);
    }
    if (group_members.empty()) {
        return;
    }
    UpdateInputLinkHealthFromGroupMembers(group_members, metrics);
    SaveInputMemberSnapshot(group_members, metrics);
}

void UpdateOutputLinkHealthFromMsgCtrl(const SRT_MSGCTRL& tx_ctrl, MetricsState* metrics) {
    if (tx_ctrl.grpdata == nullptr || tx_ctrl.grpdata_size <= 0) {
        return;
    }
    const size_t capped = std::min(static_cast<size_t>(tx_ctrl.grpdata_size), MetricsState::kMaxTrackedMembers);
    std::vector<SRT_SOCKGROUPDATA> group_members;
    group_members.reserve(capped);
    for (size_t i = 0; i < capped; ++i) {
        const auto& member = tx_ctrl.grpdata[i];
        if (!IsUsableGroupMemberSnapshot(member)) continue;
        group_members.push_back(member);
    }
    if (group_members.empty()) {
        return;
    }
    UpdateOutputLinkHealthFromGroupMembers(group_members, metrics);
    SaveOutputMemberSnapshot(group_members, metrics);
}

namespace {

std::vector<SRTSOCKET> GetInputMemberSocketsSnapshot(const MetricsState& metrics) {
    MetricsState::LinkMetricsGuard lock(metrics);
    std::vector<SRTSOCKET> sockets;
    int64_t count = metrics.input_links_snapshot_count.load(std::memory_order_relaxed);
    if (count < 0) count = 0;
    const size_t capped = std::min(static_cast<size_t>(count), MetricsState::kMaxTrackedMembers);
    sockets.reserve(capped);
    for (size_t i = 0; i < capped; ++i) {
        if (metrics.input_tracked.slots[i].member_connected != 1) {
            continue;
        }
        const auto id = static_cast<SRTSOCKET>(metrics.input_tracked.slots[i].member_id);
        if (id == SRT_INVALID_SOCK || id == 0) {
            continue;
        }
        sockets.push_back(id);
    }
    return sockets;
}

std::vector<SRTSOCKET> GetOutputMemberSocketsSnapshot(const MetricsState& metrics) {
    MetricsState::LinkMetricsGuard lock(metrics);
    std::vector<SRTSOCKET> sockets;
    int64_t count = metrics.output_links_snapshot_count.load(std::memory_order_relaxed);
    if (count < 0) count = 0;
    const size_t capped = std::min(static_cast<size_t>(count), MetricsState::kMaxTrackedMembers);
    sockets.reserve(capped);
    for (size_t i = 0; i < capped; ++i) {
        if (metrics.output_tracked.slots[i].member_connected != 1) {
            continue;
        }
        const auto id = static_cast<SRTSOCKET>(metrics.output_tracked.slots[i].member_id);
        if (id == SRT_INVALID_SOCK || id == 0) {
            continue;
        }
        sockets.push_back(id);
    }
    return sockets;
}

uint64_t CounterDeltaWithReset(uint64_t prev, uint64_t curr) {
    if (curr >= prev) {
        return curr - prev;
    }
    return curr;
}

uint64_t UpdateCounterTotalAndLast(uint64_t current, uint64_t total, uint64_t* last) {
    total += CounterDeltaWithReset(*last, current);
    *last = current;
    return total;
}

void ResetInputLinkSlotCurrentCountersLocked(MetricsState* metrics, size_t index) {
    metrics->AssertLinkMetricsLocked("ResetInputLinkSlotCurrentCountersLocked");
    metrics->input_tracked.slots[index].link_rx_bytes_current = 0;
    metrics->input_tracked.slots[index].link_tx_bytes_current = 0;
    metrics->input_tracked.slots[index].link_packet_belated_current = 0;
    metrics->input_tracked.slots[index].link_rtt_ms = -1;
}

void ResetOutputLinkSlotCurrentCountersLocked(MetricsState* metrics, size_t index) {
    metrics->AssertLinkMetricsLocked("ResetOutputLinkSlotCurrentCountersLocked");
    metrics->output_tracked.slots[index].link_rx_bytes_current = 0;
    metrics->output_tracked.slots[index].link_tx_bytes_current = 0;
    metrics->output_tracked.slots[index].link_rtt_ms = -1;
}

using SocketStatsSnapshot = std::unordered_map<SRTSOCKET, SRT_TRACEBSTATS>;

SocketStatsSnapshot CollectSocketStatsSnapshot(const std::vector<SRTSOCKET>& sockets) {
    SocketStatsSnapshot stats_by_socket;
    stats_by_socket.reserve(sockets.size());
    for (const auto sock : sockets) {
        if (sock == SRT_INVALID_SOCK || sock == 0) {
            continue;
        }
        SRT_TRACEBSTATS stats {};
        // Use non-clearing reads so all metrics in this interval see the same counters.
        if (srt_bstats(sock, &stats, 0) == SRT_ERROR) {
            continue;
        }
        stats_by_socket.emplace(sock, stats);
    }
    return stats_by_socket;
}

void UpdateInputLinkTrafficPerSlot(MetricsState* metrics, const SocketStatsSnapshot& input_stats_by_socket) {
    MetricsState::LinkMetricsGuard lock(*metrics);
    int64_t count = metrics->input_links_snapshot_count.load(std::memory_order_relaxed);
    if (count < 0) count = 0;
    const size_t capped = std::min(static_cast<size_t>(count), MetricsState::kMaxTrackedMembers);

    for (size_t i = 0; i < capped; ++i) {
        const auto identity_key = metrics->input_tracked.slots[i].member_identity_key;
        if (identity_key == 0) {
            ResetInputLinkSlotCurrentCountersLocked(metrics, i);
            continue;
        }

        const auto member_sock = static_cast<SRTSOCKET>(metrics->input_tracked.slots[i].member_id);
        const auto member_connected = metrics->input_tracked.slots[i].member_connected == 1;
        if (!member_connected || member_sock == SRT_INVALID_SOCK || member_sock == 0) {
            ResetInputLinkSlotCurrentCountersLocked(metrics, i);
            continue;
        }

        const auto stats_it = input_stats_by_socket.find(member_sock);
        if (stats_it == input_stats_by_socket.end()) {
            continue;
        }
        const auto& stats = stats_it->second;

        const auto rx_current = static_cast<uint64_t>(stats.byteRecvTotal);
        const auto tx_current = static_cast<uint64_t>(stats.byteSentTotal);
        const auto belated_current = static_cast<uint64_t>(stats.pktRcvBelated);
        auto rx_last = metrics->input_tracked.slots[i].link_rx_bytes_last;
        auto tx_last = metrics->input_tracked.slots[i].link_tx_bytes_last;
        auto belated_last = metrics->input_tracked.slots[i].link_packet_belated_last;
        const auto rx_total = UpdateCounterTotalAndLast(
            rx_current,
            metrics->input_tracked.slots[i].link_rx_bytes_total,
            &rx_last);
        const auto tx_total = UpdateCounterTotalAndLast(
            tx_current,
            metrics->input_tracked.slots[i].link_tx_bytes_total,
            &tx_last);
        const auto belated_total = UpdateCounterTotalAndLast(
            belated_current,
            metrics->input_tracked.slots[i].link_packet_belated_total,
            &belated_last);
        const auto rtt_ms = static_cast<int64_t>(stats.msRTT);
        metrics->input_tracked.slots[i].link_rx_bytes_total = rx_total;
        metrics->input_tracked.slots[i].link_tx_bytes_total = tx_total;
        metrics->input_tracked.slots[i].link_packet_belated_total = belated_total;
        metrics->input_tracked.slots[i].link_rx_bytes_current = rx_current;
        metrics->input_tracked.slots[i].link_tx_bytes_current = tx_current;
        metrics->input_tracked.slots[i].link_packet_belated_current = belated_current;
        metrics->input_tracked.slots[i].link_rx_bytes_last = rx_last;
        metrics->input_tracked.slots[i].link_tx_bytes_last = tx_last;
        metrics->input_tracked.slots[i].link_packet_belated_last = belated_last;
        metrics->input_tracked.slots[i].link_rtt_ms = rtt_ms;
    }
}

void UpdateOutputLinkTrafficPerSlot(MetricsState* metrics, const SocketStatsSnapshot& output_stats_by_socket) {
    MetricsState::LinkMetricsGuard lock(*metrics);
    int64_t count = metrics->output_links_snapshot_count.load(std::memory_order_relaxed);
    if (count < 0) count = 0;
    const size_t capped = std::min(static_cast<size_t>(count), MetricsState::kMaxTrackedMembers);

    for (size_t i = 0; i < capped; ++i) {
        const auto identity_key = metrics->output_tracked.slots[i].member_identity_key;
        if (identity_key == 0) {
            ResetOutputLinkSlotCurrentCountersLocked(metrics, i);
            continue;
        }

        const auto member_sock = static_cast<SRTSOCKET>(metrics->output_tracked.slots[i].member_id);
        const auto member_connected = metrics->output_tracked.slots[i].member_connected == 1;
        if (!member_connected || member_sock == SRT_INVALID_SOCK || member_sock == 0) {
            ResetOutputLinkSlotCurrentCountersLocked(metrics, i);
            continue;
        }

        const auto stats_it = output_stats_by_socket.find(member_sock);
        if (stats_it == output_stats_by_socket.end()) {
            continue;
        }
        const auto& stats = stats_it->second;

        const auto rx_current = static_cast<uint64_t>(stats.byteRecvTotal);
        const auto tx_current = static_cast<uint64_t>(stats.byteSentTotal);
        auto rx_last = metrics->output_tracked.slots[i].link_rx_bytes_last;
        auto tx_last = metrics->output_tracked.slots[i].link_tx_bytes_last;
        const auto rx_total = UpdateCounterTotalAndLast(
            rx_current,
            metrics->output_tracked.slots[i].link_rx_bytes_total,
            &rx_last);
        const auto tx_total = UpdateCounterTotalAndLast(
            tx_current,
            metrics->output_tracked.slots[i].link_tx_bytes_total,
            &tx_last);
        const auto rtt_ms = static_cast<int64_t>(stats.msRTT);
        metrics->output_tracked.slots[i].link_rx_bytes_total = rx_total;
        metrics->output_tracked.slots[i].link_tx_bytes_total = tx_total;
        metrics->output_tracked.slots[i].link_rx_bytes_current = rx_current;
        metrics->output_tracked.slots[i].link_tx_bytes_current = tx_current;
        metrics->output_tracked.slots[i].link_rx_bytes_last = rx_last;
        metrics->output_tracked.slots[i].link_tx_bytes_last = tx_last;
        metrics->output_tracked.slots[i].link_rtt_ms = rtt_ms;
    }
}

void UpdateTransportTrafficMetrics(SRTSOCKET input_session_sock, SRTSOCKET output_sock, MetricsState* metrics) {
    uint64_t input_group_packet_drop_total = metrics->input_group_packet_drop_total.load(std::memory_order_relaxed);
    uint64_t input_group_byte_drop_total = metrics->input_group_byte_drop_total.load(std::memory_order_relaxed);
    int64_t input_group_drop_sockets_tracked = 0;
    uint64_t input_group_packet_drop_current = 0;
    uint64_t input_group_byte_drop_current = 0;
    std::unordered_set<SRTSOCKET> input_group_sockets_with_stats;
    input_group_sockets_with_stats.reserve(1);
    if (input_session_sock != SRT_INVALID_SOCK && input_session_sock != 0) {
        SRTSOCKET input_group_sock = srt_groupof(input_session_sock);
        if (input_group_sock == SRT_INVALID_SOCK || input_group_sock == 0) {
            input_group_sock = input_session_sock;
        }
        SRT_TRACEBSTATS input_group_stats {};
        bool has_input_group_stats = (srt_bstats(input_group_sock, &input_group_stats, 0) == 0);
        if (!has_input_group_stats && input_group_sock != input_session_sock) {
            has_input_group_stats = (srt_bstats(input_session_sock, &input_group_stats, 0) == 0);
            if (has_input_group_stats) {
                input_group_sock = input_session_sock;
            }
        }
        if (has_input_group_stats) {
            input_group_sockets_with_stats.insert(input_group_sock);
            input_group_drop_sockets_tracked = 1;
            input_group_packet_drop_current = static_cast<uint64_t>(input_group_stats.pktRcvDropTotal);
            input_group_byte_drop_current = static_cast<uint64_t>(input_group_stats.byteRcvDropTotal);
            auto& prev = metrics->input_group_drop_last_by_socket[input_group_sock];
            input_group_packet_drop_total = UpdateCounterTotalAndLast(
                input_group_packet_drop_current,
                input_group_packet_drop_total,
                &prev.packet_drop_total);
            input_group_byte_drop_total = UpdateCounterTotalAndLast(
                input_group_byte_drop_current,
                input_group_byte_drop_total,
                &prev.byte_drop_total);
        }
    }
    std::unordered_map<SRTSOCKET, GroupDropCounterSnapshot> next_input_group_last;
    next_input_group_last.reserve(input_group_sockets_with_stats.size());
    for (const auto sock : input_group_sockets_with_stats) {
        next_input_group_last.emplace(sock, metrics->input_group_drop_last_by_socket[sock]);
    }
    metrics->input_group_drop_last_by_socket.swap(next_input_group_last);
    metrics->input_group_packet_drop_total.store(input_group_packet_drop_total, std::memory_order_relaxed);
    metrics->input_group_byte_drop_total.store(input_group_byte_drop_total, std::memory_order_relaxed);
    metrics->input_group_drop_sockets_tracked.store(input_group_drop_sockets_tracked, std::memory_order_relaxed);
    metrics->input_group_packet_drop_current.store(input_group_packet_drop_current, std::memory_order_relaxed);
    metrics->input_group_byte_drop_current.store(input_group_byte_drop_current, std::memory_order_relaxed);

    const auto member_sockets = GetInputMemberSocketsSnapshot(*metrics);
    const auto input_stats_by_socket = CollectSocketStatsSnapshot(member_sockets);
    UpdateInputLinkTrafficPerSlot(metrics, input_stats_by_socket);
    std::unordered_set<SRTSOCKET> sockets_with_stats;
    sockets_with_stats.reserve(member_sockets.size());
    uint64_t input_byte_recv_total = metrics->input_transport_byte_recv_total.load(std::memory_order_relaxed);
    uint64_t input_byte_recv_unique_total = metrics->input_transport_byte_recv_unique_total.load(std::memory_order_relaxed);
    uint64_t input_byte_retrans_total = metrics->input_transport_byte_retrans_total.load(std::memory_order_relaxed);
    uint64_t input_byte_loss_total = metrics->input_transport_byte_loss_total.load(std::memory_order_relaxed);
    uint64_t input_packet_belated_total = metrics->input_transport_packet_belated_total.load(std::memory_order_relaxed);
    uint64_t input_byte_recv_current = 0;
    uint64_t input_byte_recv_unique_current = 0;
    uint64_t input_byte_retrans_current = 0;
    uint64_t input_byte_loss_current = 0;
    uint64_t input_packet_belated_current = 0;

    for (const auto& [member_sock, in_stats] : input_stats_by_socket) {
        sockets_with_stats.insert(member_sock);

        input_byte_recv_current += static_cast<uint64_t>(in_stats.byteRecvTotal);
        input_byte_recv_unique_current += static_cast<uint64_t>(in_stats.byteRecvUniqueTotal);
        input_byte_retrans_current += static_cast<uint64_t>(in_stats.byteRetransTotal);
        input_byte_loss_current += static_cast<uint64_t>(in_stats.byteRcvLossTotal);
        input_packet_belated_current += static_cast<uint64_t>(in_stats.pktRcvBelated);

        auto& prev = metrics->input_transport_last_by_socket[member_sock];
        input_byte_recv_total = UpdateCounterTotalAndLast(
            static_cast<uint64_t>(in_stats.byteRecvTotal),
            input_byte_recv_total,
            &prev.byte_recv_total);
        input_byte_recv_unique_total = UpdateCounterTotalAndLast(
            static_cast<uint64_t>(in_stats.byteRecvUniqueTotal),
            input_byte_recv_unique_total,
            &prev.byte_recv_unique_total);
        input_byte_retrans_total = UpdateCounterTotalAndLast(
            static_cast<uint64_t>(in_stats.byteRetransTotal),
            input_byte_retrans_total,
            &prev.byte_retrans_total);
        input_byte_loss_total = UpdateCounterTotalAndLast(
            static_cast<uint64_t>(in_stats.byteRcvLossTotal),
            input_byte_loss_total,
            &prev.byte_loss_total);
        input_packet_belated_total = UpdateCounterTotalAndLast(
            static_cast<uint64_t>(in_stats.pktRcvBelated),
            input_packet_belated_total,
            &prev.packet_belated_total);
    }

    std::unordered_map<SRTSOCKET, TransportCounterSnapshot> next_last;
    next_last.reserve(sockets_with_stats.size());
    for (const auto sock : sockets_with_stats) {
        next_last.emplace(sock, metrics->input_transport_last_by_socket[sock]);
    }
    metrics->input_transport_last_by_socket.swap(next_last);

    metrics->input_transport_members_total.store(static_cast<int64_t>(sockets_with_stats.size()), std::memory_order_relaxed);
    metrics->input_transport_byte_recv_total.store(input_byte_recv_total, std::memory_order_relaxed);
    metrics->input_transport_byte_recv_unique_total.store(input_byte_recv_unique_total, std::memory_order_relaxed);
    metrics->input_transport_byte_retrans_total.store(input_byte_retrans_total, std::memory_order_relaxed);
    metrics->input_transport_byte_loss_total.store(input_byte_loss_total, std::memory_order_relaxed);
    metrics->input_transport_packet_belated_total.store(input_packet_belated_total, std::memory_order_relaxed);
    metrics->input_transport_byte_recv_current.store(input_byte_recv_current, std::memory_order_relaxed);
    metrics->input_transport_byte_recv_unique_current.store(input_byte_recv_unique_current, std::memory_order_relaxed);
    metrics->input_transport_byte_retrans_current.store(input_byte_retrans_current, std::memory_order_relaxed);
    metrics->input_transport_byte_loss_current.store(input_byte_loss_current, std::memory_order_relaxed);
    metrics->input_transport_packet_belated_current.store(input_packet_belated_current, std::memory_order_relaxed);
    metrics->input_group_packet_drop_total.store(input_group_packet_drop_total, std::memory_order_relaxed);
    metrics->input_group_byte_drop_total.store(input_group_byte_drop_total, std::memory_order_relaxed);
    metrics->input_group_drop_sockets_tracked.store(input_group_drop_sockets_tracked, std::memory_order_relaxed);
    metrics->input_group_packet_drop_current.store(input_group_packet_drop_current, std::memory_order_relaxed);
    metrics->input_group_byte_drop_current.store(input_group_byte_drop_current, std::memory_order_relaxed);

    std::vector<SRTSOCKET> output_member_sockets = GetOutputMemberSocketsSnapshot(*metrics);
    if (output_member_sockets.empty() && output_sock != SRT_INVALID_SOCK) {
        output_member_sockets.push_back(output_sock);
    }
    const auto output_stats_by_socket = CollectSocketStatsSnapshot(output_member_sockets);
    UpdateOutputLinkTrafficPerSlot(metrics, output_stats_by_socket);
    std::unordered_set<SRTSOCKET> output_sockets_with_stats;
    output_sockets_with_stats.reserve(output_member_sockets.size());
    uint64_t output_byte_sent_total = metrics->output_transport_byte_sent_total.load(std::memory_order_relaxed);
    uint64_t output_byte_sent_unique_total = metrics->output_transport_byte_sent_unique_total.load(std::memory_order_relaxed);
    uint64_t output_byte_retrans_total = metrics->output_transport_byte_retrans_total.load(std::memory_order_relaxed);
    uint64_t output_byte_drop_total = metrics->output_transport_byte_drop_total.load(std::memory_order_relaxed);
    uint64_t output_byte_sent_current = 0;
    uint64_t output_byte_sent_unique_current = 0;
    uint64_t output_byte_retrans_current = 0;
    uint64_t output_byte_drop_current = 0;

    for (const auto& [member_sock, output_stats] : output_stats_by_socket) {
        output_sockets_with_stats.insert(member_sock);
        output_byte_sent_current += static_cast<uint64_t>(output_stats.byteSentTotal);
        output_byte_sent_unique_current += static_cast<uint64_t>(output_stats.byteSentUniqueTotal);
        output_byte_retrans_current += static_cast<uint64_t>(output_stats.byteRetransTotal);
        output_byte_drop_current += static_cast<uint64_t>(output_stats.byteSndDropTotal);

        auto& prev = metrics->output_transport_last_by_socket[member_sock];
        output_byte_sent_total = UpdateCounterTotalAndLast(
            static_cast<uint64_t>(output_stats.byteSentTotal),
            output_byte_sent_total,
            &prev.byte_sent_total);
        output_byte_sent_unique_total = UpdateCounterTotalAndLast(
            static_cast<uint64_t>(output_stats.byteSentUniqueTotal),
            output_byte_sent_unique_total,
            &prev.byte_sent_unique_total);
        output_byte_retrans_total = UpdateCounterTotalAndLast(
            static_cast<uint64_t>(output_stats.byteRetransTotal),
            output_byte_retrans_total,
            &prev.byte_retrans_total);
        output_byte_drop_total = UpdateCounterTotalAndLast(
            static_cast<uint64_t>(output_stats.byteSndDropTotal),
            output_byte_drop_total,
            &prev.byte_drop_total);
    }

    std::unordered_map<SRTSOCKET, TransportCounterSnapshot> next_output_last;
    next_output_last.reserve(output_sockets_with_stats.size());
    for (const auto sock : output_sockets_with_stats) {
        next_output_last.emplace(sock, metrics->output_transport_last_by_socket[sock]);
    }
    metrics->output_transport_last_by_socket.swap(next_output_last);

    metrics->output_transport_members_total.store(static_cast<int64_t>(output_sockets_with_stats.size()), std::memory_order_relaxed);
    metrics->output_transport_byte_sent_total.store(output_byte_sent_total, std::memory_order_relaxed);
    metrics->output_transport_byte_sent_unique_total.store(output_byte_sent_unique_total, std::memory_order_relaxed);
    metrics->output_transport_byte_retrans_total.store(output_byte_retrans_total, std::memory_order_relaxed);
    metrics->output_transport_byte_drop_total.store(output_byte_drop_total, std::memory_order_relaxed);
    metrics->output_transport_byte_sent_current.store(output_byte_sent_current, std::memory_order_relaxed);
    metrics->output_transport_byte_sent_unique_current.store(output_byte_sent_unique_current, std::memory_order_relaxed);
    metrics->output_transport_byte_retrans_current.store(output_byte_retrans_current, std::memory_order_relaxed);
    metrics->output_transport_byte_drop_current.store(output_byte_drop_current, std::memory_order_relaxed);
}

template <typename SlotArr>
bool TryComputeMaxConnectedLinkRttGeneric(int64_t snapshot_count,
                                          const SlotArr& slots,
                                          int64_t* out_max_rtt_ms) {
    if (out_max_rtt_ms == nullptr) {
        return false;
    }
    if (snapshot_count < 0) snapshot_count = 0;
    const size_t capped = std::min(static_cast<size_t>(snapshot_count), MetricsState::kMaxTrackedMembers);
    int64_t max_rtt = -1;
    for (size_t i = 0; i < capped; ++i) {
        if (slots[i].member_connected != 1) {
            continue;
        }
        const int64_t rtt_ms = slots[i].link_rtt_ms;
        if (rtt_ms < 0) {
            continue;
        }
        max_rtt = std::max(max_rtt, rtt_ms);
    }
    if (max_rtt < 0) {
        return false;
    }
    *out_max_rtt_ms = max_rtt;
    return true;
}

bool TryComputeMaxConnectedInputLinkRtt(const MetricsState& metrics, int64_t* out_max_rtt_ms) {
    MetricsState::LinkMetricsGuard lock(metrics);
    const int64_t count = metrics.input_links_snapshot_count.load(std::memory_order_relaxed);
    return TryComputeMaxConnectedLinkRttGeneric(count,
                                                metrics.input_tracked.slots,
                                                out_max_rtt_ms);
}

bool TryComputeMaxConnectedOutputLinkRtt(const MetricsState& metrics, int64_t* out_max_rtt_ms) {
    MetricsState::LinkMetricsGuard lock(metrics);
    const int64_t count = metrics.output_links_snapshot_count.load(std::memory_order_relaxed);
    return TryComputeMaxConnectedLinkRttGeneric(count,
                                                metrics.output_tracked.slots,
                                                out_max_rtt_ms);
}

}  // namespace

std::string RenderPrometheusMetrics(const MetricsState& metrics) {
    MetricsState::LinkMetricsGuard lock(metrics);
    const auto total_rx_bytes = metrics.total_rx_bytes.load(std::memory_order_relaxed);
    const auto total_tx_bytes = metrics.total_tx_bytes.load(std::memory_order_relaxed);
    const auto total_rx_msgs = metrics.total_rx_msgs.load(std::memory_order_relaxed);
    const auto total_tx_msgs = metrics.total_tx_msgs.load(std::memory_order_relaxed);
    const auto total_send_failures = metrics.total_send_failures.load(std::memory_order_relaxed);
    const auto reconnect_count = metrics.reconnect_count.load(std::memory_order_relaxed);

    const auto rx_bytes_per_sec = metrics.rx_bytes_per_sec.load(std::memory_order_relaxed);
    const auto tx_bytes_per_sec = metrics.tx_bytes_per_sec.load(std::memory_order_relaxed);
    const auto rx_msgs_per_sec = metrics.rx_msgs_per_sec.load(std::memory_order_relaxed);
    const auto tx_msgs_per_sec = metrics.tx_msgs_per_sec.load(std::memory_order_relaxed);
    const auto interval_send_failures = metrics.interval_send_failures.load(std::memory_order_relaxed);

    const auto input_listening = metrics.input_listening.load(std::memory_order_relaxed);
    const auto input_connected = metrics.input_connected.load(std::memory_order_relaxed);
    const auto output_connected = metrics.output_connected.load(std::memory_order_relaxed);
    const auto input_links_total = metrics.input_links_total.load(std::memory_order_relaxed);
    const auto input_links_healthy = metrics.input_links_healthy.load(std::memory_order_relaxed);
    const auto input_links_running = metrics.input_links_running.load(std::memory_order_relaxed);
    const auto output_links_total = metrics.output_links_total.load(std::memory_order_relaxed);
    const auto output_links_healthy = metrics.output_links_healthy.load(std::memory_order_relaxed);
    const auto output_links_running = metrics.output_links_running.load(std::memory_order_relaxed);
    int64_t input_links_snapshot_count = metrics.input_links_snapshot_count.load(std::memory_order_relaxed);
    if (input_links_snapshot_count < 0) {
        input_links_snapshot_count = 0;
    }
    const size_t input_links_snapshot_capped =
        std::min(static_cast<size_t>(input_links_snapshot_count), MetricsState::kMaxTrackedMembers);
    int64_t output_links_snapshot_count = metrics.output_links_snapshot_count.load(std::memory_order_relaxed);
    if (output_links_snapshot_count < 0) {
        output_links_snapshot_count = 0;
    }
    const size_t output_links_snapshot_capped =
        std::min(static_cast<size_t>(output_links_snapshot_count), MetricsState::kMaxTrackedMembers);
    const auto input_transport_byte_recv_total = metrics.input_transport_byte_recv_total.load(std::memory_order_relaxed);
    const auto input_transport_byte_recv_unique_total = metrics.input_transport_byte_recv_unique_total.load(std::memory_order_relaxed);
    const auto input_transport_byte_retrans_total = metrics.input_transport_byte_retrans_total.load(std::memory_order_relaxed);
    const auto input_transport_byte_loss_total = metrics.input_transport_byte_loss_total.load(std::memory_order_relaxed);
    const auto input_transport_packet_belated_total = metrics.input_transport_packet_belated_total.load(std::memory_order_relaxed);
    const auto input_transport_members_total = metrics.input_transport_members_total.load(std::memory_order_relaxed);
    const auto input_transport_byte_recv_current = metrics.input_transport_byte_recv_current.load(std::memory_order_relaxed);
    const auto input_transport_byte_recv_unique_current = metrics.input_transport_byte_recv_unique_current.load(std::memory_order_relaxed);
    const auto input_transport_byte_retrans_current = metrics.input_transport_byte_retrans_current.load(std::memory_order_relaxed);
    const auto input_transport_byte_loss_current = metrics.input_transport_byte_loss_current.load(std::memory_order_relaxed);
    const auto input_transport_packet_belated_current = metrics.input_transport_packet_belated_current.load(std::memory_order_relaxed);
    const auto input_group_packet_drop_total = metrics.input_group_packet_drop_total.load(std::memory_order_relaxed);
    const auto input_group_byte_drop_total = metrics.input_group_byte_drop_total.load(std::memory_order_relaxed);
    const auto input_group_drop_sockets_tracked = metrics.input_group_drop_sockets_tracked.load(std::memory_order_relaxed);
    const auto input_group_packet_drop_current = metrics.input_group_packet_drop_current.load(std::memory_order_relaxed);
    const auto input_group_byte_drop_current = metrics.input_group_byte_drop_current.load(std::memory_order_relaxed);
    const auto output_transport_byte_sent_total = metrics.output_transport_byte_sent_total.load(std::memory_order_relaxed);
    const auto output_transport_byte_sent_unique_total = metrics.output_transport_byte_sent_unique_total.load(std::memory_order_relaxed);
    const auto output_transport_byte_retrans_total = metrics.output_transport_byte_retrans_total.load(std::memory_order_relaxed);
    const auto output_transport_byte_drop_total = metrics.output_transport_byte_drop_total.load(std::memory_order_relaxed);
    const auto output_transport_members_total = metrics.output_transport_members_total.load(std::memory_order_relaxed);
    const auto output_transport_byte_sent_current = metrics.output_transport_byte_sent_current.load(std::memory_order_relaxed);
    const auto output_transport_byte_sent_unique_current = metrics.output_transport_byte_sent_unique_current.load(std::memory_order_relaxed);
    const auto output_transport_byte_retrans_current = metrics.output_transport_byte_retrans_current.load(std::memory_order_relaxed);
    const auto output_transport_byte_drop_current = metrics.output_transport_byte_drop_current.load(std::memory_order_relaxed);
    const auto path_ready = (input_connected == 1 && output_connected == 1) ? 1 : 0;
    const auto input_sources_total = metrics.input_sources_total.load(std::memory_order_relaxed);
    const auto output_sources_total = metrics.output_sources_total.load(std::memory_order_relaxed);
    const auto active_input_index = metrics.active_input_index.load(std::memory_order_relaxed);
    const auto input_switches_total = metrics.input_switches_total.load(std::memory_order_relaxed);
    const auto primary_input_index = metrics.primary_input_index.load(std::memory_order_relaxed);
    const auto switch_policy = metrics.switch_policy.load(std::memory_order_relaxed);
    const auto switch_mode = metrics.switch_mode.load(std::memory_order_relaxed);
    const auto input_bond_mode = metrics.input_bond_mode.load(std::memory_order_relaxed);
    const auto output_bond_mode = metrics.output_bond_mode.load(std::memory_order_relaxed);

    const auto input_rtt_ms = metrics.input_rtt_ms.load(std::memory_order_relaxed);
    const auto output_rtt_ms = metrics.output_rtt_ms.load(std::memory_order_relaxed);
    const auto input_effective_latency_ms = metrics.input_effective_latency_ms.load(std::memory_order_relaxed);
    const auto output_effective_latency_ms = metrics.output_effective_latency_ms.load(std::memory_order_relaxed);
    const auto last_rx_unix_seconds = metrics.last_rx_unix_ms.load(std::memory_order_relaxed) / 1000;
    const auto last_tx_unix_seconds = metrics.last_tx_unix_ms.load(std::memory_order_relaxed) / 1000;
    const auto last_input_failure_unix_seconds = metrics.last_input_failure_unix_seconds.load(std::memory_order_relaxed);
    const auto last_output_failure_unix_seconds = metrics.last_output_failure_unix_seconds.load(std::memory_order_relaxed);
    const auto incident_active = metrics.incident_active.load(std::memory_order_relaxed);
    const auto incident_open_unix_ms = metrics.incident_open_unix_ms.load(std::memory_order_relaxed);
    const int64_t incident_age_seconds =
        (incident_active == 1 && incident_open_unix_ms > 0)
            ? std::max<int64_t>(0, (UnixNowMs() - incident_open_unix_ms) / 1000)
            : 0;

    std::ostringstream out;
    auto emit_u64 = [&](const char* name, const char* type, const char* help, uint64_t value) {
        out << "# HELP " << name << " " << help << "\n";
        out << "# TYPE " << name << " " << type << "\n";
        out << name << " " << value << "\n";
    };
    auto emit_i64 = [&](const char* name, const char* type, const char* help, int64_t value) {
        out << "# HELP " << name << " " << help << "\n";
        out << "# TYPE " << name << " " << type << "\n";
        out << name << " " << value << "\n";
    };

    struct U64MetricDef {
        const char* name;
        const char* type;
        const char* help;
        uint64_t value;
    };
    const std::array<U64MetricDef, 12> u64_metric_defs{{
        {"srt_relay_rx_bytes_total", "counter", "Total bytes received by relay input.", total_rx_bytes},
        {"srt_relay_tx_bytes_total", "counter", "Total bytes sent by relay output.", total_tx_bytes},
        {"srt_relay_rx_messages_total", "counter", "Total messages received by relay input.", total_rx_msgs},
        {"srt_relay_tx_messages_total", "counter", "Total messages sent by relay output.", total_tx_msgs},
        {"srt_relay_send_failures_total", "counter", "Total failed output send attempts.", total_send_failures},
        {"srt_relay_reconnects_total", "counter", "Total reconnect attempts after output failures.", reconnect_count},
        {"srt_relay_rx_bytes_per_sec", "gauge", "Received bytes per second over last stats interval.", rx_bytes_per_sec},
        {"srt_relay_tx_bytes_per_sec", "gauge", "Sent bytes per second over last stats interval.", tx_bytes_per_sec},
        {"srt_relay_rx_messages_per_sec", "gauge", "Received messages per second over last stats interval.", rx_msgs_per_sec},
        {"srt_relay_tx_messages_per_sec", "gauge", "Sent messages per second over last stats interval.", tx_msgs_per_sec},
        {"srt_relay_send_failures_interval", "gauge", "Output send failures over last stats interval.", interval_send_failures},
        {"srt_relay_input_switches_total", "counter", "Total input source switch events.", input_switches_total},
    }};
    for (const auto& metric : u64_metric_defs) {
        emit_u64(metric.name, metric.type, metric.help, metric.value);
    }

    struct I64MetricDef {
        const char* name;
        const char* type;
        const char* help;
        int64_t value;
    };
    const std::array<I64MetricDef, 14> i64_metric_defs{{
        {"srt_relay_input_listening", "gauge", "Relay input listener is up (1/0).", input_listening},
        {"srt_relay_input_connected", "gauge", "Relay input session is connected (1/0).", input_connected},
        {"srt_relay_output_connected", "gauge", "Relay output session is connected (1/0).", output_connected},
        {"srt_relay_path_ready", "gauge", "Relay data path input+output connected (1/0).", path_ready},
        {"srt_relay_input_sources_total", "gauge", "Configured independent input source count.", input_sources_total},
        {"srt_relay_output_sources_total", "gauge", "Configured independent output sink count.", output_sources_total},
        {"srt_relay_active_input_index", "gauge", "1-based active input source index.", active_input_index},
        {"srt_relay_primary_input_index", "gauge", "Configured primary input index (1-based, 0 when unset).", primary_input_index},
        {"srt_relay_input_links_total", "gauge", "Total number of input links in the active SRT group.", input_links_total},
        {"srt_relay_input_links_healthy", "gauge", "Number of healthy input links in the active SRT group.", input_links_healthy},
        {"srt_relay_input_links_running", "gauge", "Number of currently running/active input links in the active SRT group.", input_links_running},
        {"srt_relay_output_links_total", "gauge", "Total number of output links in the active SRT group.", output_links_total},
        {"srt_relay_output_links_healthy", "gauge", "Number of healthy output links in the active SRT group.", output_links_healthy},
        {"srt_relay_output_links_running", "gauge", "Number of currently running/active output links in the active SRT group.", output_links_running},
    }};
    for (const auto& metric : i64_metric_defs) {
        emit_i64(metric.name, metric.type, metric.help, metric.value);
    }

    auto emit_input_link_metric_u64 = [&](const char* name,
                                          const char* type,
                                          const char* help,
                                          const auto& value_for_slot) {
        out << "# HELP " << name << " " << help << "\n";
        out << "# TYPE " << name << " " << type << "\n";
        for (size_t i = 0; i < input_links_snapshot_capped; ++i) {
            const auto& slot = metrics.input_tracked.slots[i];
            const auto identity_key = slot.member_identity_key;
            if (identity_key == 0) {
                continue;
            }
            const auto socket_id = static_cast<SRTSOCKET>(slot.member_id);
            out << name << "{link_index=\"" << (i + 1)
                << "\",socket_id=\"" << socket_id << "\"} "
                << value_for_slot(slot) << "\n";
        }
    };
    auto emit_input_link_metric_i64 = [&](const char* name,
                                          const char* type,
                                          const char* help,
                                          const auto& value_for_slot) {
        out << "# HELP " << name << " " << help << "\n";
        out << "# TYPE " << name << " " << type << "\n";
        for (size_t i = 0; i < input_links_snapshot_capped; ++i) {
            const auto& slot = metrics.input_tracked.slots[i];
            const auto identity_key = slot.member_identity_key;
            if (identity_key == 0) {
                continue;
            }
            const auto socket_id = static_cast<SRTSOCKET>(slot.member_id);
            out << name << "{link_index=\"" << (i + 1)
                << "\",socket_id=\"" << socket_id << "\"} "
                << value_for_slot(slot) << "\n";
        }
    };
    auto emit_input_link_metric_i32 = [&](const char* name,
                                          const char* type,
                                          const char* help,
                                          const auto& value_for_slot) {
        out << "# HELP " << name << " " << help << "\n";
        out << "# TYPE " << name << " " << type << "\n";
        for (size_t i = 0; i < input_links_snapshot_capped; ++i) {
            const auto& slot = metrics.input_tracked.slots[i];
            const auto identity_key = slot.member_identity_key;
            if (identity_key == 0) {
                continue;
            }
            const auto socket_id = static_cast<SRTSOCKET>(slot.member_id);
            out << name << "{link_index=\"" << (i + 1)
                << "\",socket_id=\"" << socket_id << "\"} "
                << value_for_slot(slot) << "\n";
        }
    };
    auto emit_output_link_metric_u64 = [&](const char* name,
                                           const char* type,
                                           const char* help,
                                           const auto& value_for_slot) {
        out << "# HELP " << name << " " << help << "\n";
        out << "# TYPE " << name << " " << type << "\n";
        for (size_t i = 0; i < output_links_snapshot_capped; ++i) {
            const auto& slot = metrics.output_tracked.slots[i];
            const auto identity_key = slot.member_identity_key;
            if (identity_key == 0) {
                continue;
            }
            const auto socket_id = static_cast<SRTSOCKET>(slot.member_id);
            out << name << "{link_index=\"" << (i + 1)
                << "\",socket_id=\"" << socket_id << "\"} "
                << value_for_slot(slot) << "\n";
        }
    };
    auto emit_output_link_metric_i64 = [&](const char* name,
                                           const char* type,
                                           const char* help,
                                           const auto& value_for_slot) {
        out << "# HELP " << name << " " << help << "\n";
        out << "# TYPE " << name << " " << type << "\n";
        for (size_t i = 0; i < output_links_snapshot_capped; ++i) {
            const auto& slot = metrics.output_tracked.slots[i];
            const auto identity_key = slot.member_identity_key;
            if (identity_key == 0) {
                continue;
            }
            const auto socket_id = static_cast<SRTSOCKET>(slot.member_id);
            out << name << "{link_index=\"" << (i + 1)
                << "\",socket_id=\"" << socket_id << "\"} "
                << value_for_slot(slot) << "\n";
        }
    };
    auto emit_output_link_metric_i32 = [&](const char* name,
                                           const char* type,
                                           const char* help,
                                           const auto& value_for_slot) {
        out << "# HELP " << name << " " << help << "\n";
        out << "# TYPE " << name << " " << type << "\n";
        for (size_t i = 0; i < output_links_snapshot_capped; ++i) {
            const auto& slot = metrics.output_tracked.slots[i];
            const auto identity_key = slot.member_identity_key;
            if (identity_key == 0) {
                continue;
            }
            const auto socket_id = static_cast<SRTSOCKET>(slot.member_id);
            out << name << "{link_index=\"" << (i + 1)
                << "\",socket_id=\"" << socket_id << "\"} "
                << value_for_slot(slot) << "\n";
        }
    };

    emit_input_link_metric_i32("srt_relay_input_link_connected", "gauge",
                               "Per-input-link connection state in the active SRT group (1/0).",
                               [](const auto& slot) { return slot.member_connected; });
    emit_input_link_metric_u64("srt_relay_input_link_rx_bytes_total", "counter",
                               "Monotonic per-link transport RX bytes for each stable input link slot.",
                               [](const auto& slot) { return slot.link_rx_bytes_total; });
    emit_input_link_metric_u64("srt_relay_input_link_tx_bytes_total", "counter",
                               "Monotonic per-link transport TX bytes for each stable input link slot.",
                               [](const auto& slot) { return slot.link_tx_bytes_total; });
    emit_input_link_metric_u64("srt_relay_input_link_rx_bytes_current", "gauge",
                               "Current per-link byteRecvTotal for each stable input link slot.",
                               [](const auto& slot) { return slot.link_rx_bytes_current; });
    emit_input_link_metric_u64("srt_relay_input_link_tx_bytes_current", "gauge",
                               "Current per-link byteSentTotal for each stable input link slot.",
                               [](const auto& slot) { return slot.link_tx_bytes_current; });
    emit_input_link_metric_u64("srt_relay_input_link_packet_belated_total", "counter",
                               "Monotonic per-link belated packets ignored for playout for each stable input link slot.",
                               [](const auto& slot) { return slot.link_packet_belated_total; });
    emit_input_link_metric_u64("srt_relay_input_link_packet_belated_current", "gauge",
                               "Current per-link pktRcvBelated for each stable input link slot.",
                               [](const auto& slot) { return slot.link_packet_belated_current; });
    emit_input_link_metric_i64("srt_relay_input_link_rtt_ms", "gauge",
                               "Per-link RTT in milliseconds for each stable input link slot.",
                               [](const auto& slot) { return slot.link_rtt_ms; });
    emit_output_link_metric_i32("srt_relay_output_link_connected", "gauge",
                                "Per-output-link connection state in the active SRT group (1/0).",
                                [](const auto& slot) { return slot.member_connected; });
    emit_output_link_metric_u64("srt_relay_output_link_rx_bytes_total", "counter",
                                "Monotonic per-link transport RX bytes for each stable output link slot.",
                                [](const auto& slot) { return slot.link_rx_bytes_total; });
    emit_output_link_metric_u64("srt_relay_output_link_tx_bytes_total", "counter",
                                "Monotonic per-link transport TX bytes for each stable output link slot.",
                                [](const auto& slot) { return slot.link_tx_bytes_total; });
    emit_output_link_metric_u64("srt_relay_output_link_rx_bytes_current", "gauge",
                                "Current per-link byteRecvTotal for each stable output link slot.",
                                [](const auto& slot) { return slot.link_rx_bytes_current; });
    emit_output_link_metric_u64("srt_relay_output_link_tx_bytes_current", "gauge",
                                "Current per-link byteSentTotal for each stable output link slot.",
                                [](const auto& slot) { return slot.link_tx_bytes_current; });
    emit_output_link_metric_i64("srt_relay_output_link_rtt_ms", "gauge",
                                "Per-link RTT in milliseconds for each stable output link slot.",
                                [](const auto& slot) { return slot.link_rtt_ms; });

    emit_u64("srt_relay_input_transport_byte_recv_total", "counter", "Monotonic input transport bytes received across tracked SRT member sockets (includes duplicate traffic).", input_transport_byte_recv_total);
    emit_u64("srt_relay_input_transport_byte_recv_unique_total", "counter", "Monotonic input transport unique bytes received across tracked SRT member sockets.", input_transport_byte_recv_unique_total);
    emit_u64("srt_relay_input_transport_byte_retrans_total", "counter", "Monotonic input transport retransmitted bytes across tracked SRT member sockets.", input_transport_byte_retrans_total);
    emit_u64("srt_relay_input_transport_byte_loss_total", "counter", "Monotonic input transport reported lost bytes across tracked SRT member sockets.", input_transport_byte_loss_total);
    emit_u64("srt_relay_input_transport_packet_belated_total", "counter", "Monotonic input transport belated packets (sum of pktRcvBelated) across tracked SRT member sockets.", input_transport_packet_belated_total);
    emit_i64("srt_relay_input_transport_members_tracked", "gauge", "Number of input SRT member sockets tracked for transport metrics.", input_transport_members_total);
    emit_u64("srt_relay_input_transport_byte_recv_current", "gauge", "Current summed byteRecvTotal across tracked SRT member sockets.", input_transport_byte_recv_current);
    emit_u64("srt_relay_input_transport_byte_recv_unique_current", "gauge", "Current summed byteRecvUniqueTotal across tracked SRT member sockets.", input_transport_byte_recv_unique_current);
    emit_u64("srt_relay_input_transport_byte_retrans_current", "gauge", "Current summed byteRetransTotal across tracked SRT member sockets.", input_transport_byte_retrans_current);
    emit_u64("srt_relay_input_transport_byte_loss_current", "gauge", "Current summed byteRcvLossTotal across tracked SRT member sockets.", input_transport_byte_loss_current);
    emit_u64("srt_relay_input_transport_packet_belated_current", "gauge", "Current summed pktRcvBelated across tracked SRT member sockets.", input_transport_packet_belated_current);
    emit_u64("srt_relay_input_group_packet_drop_total", "counter", "Monotonic input bonded group too-late packet drops (pktRcvDropTotal) on the active input group/session socket.", input_group_packet_drop_total);
    emit_u64("srt_relay_input_group_byte_drop_total", "counter", "Monotonic input bonded group too-late byte drops (byteRcvDropTotal) on the active input group/session socket.", input_group_byte_drop_total);
    emit_i64("srt_relay_input_group_drop_sockets_tracked", "gauge", "Number of input group/session sockets tracked for group-drop metrics.", input_group_drop_sockets_tracked);
    emit_u64("srt_relay_input_group_packet_drop_current", "gauge", "Current pktRcvDropTotal on the active input group/session socket.", input_group_packet_drop_current);
    emit_u64("srt_relay_input_group_byte_drop_current", "gauge", "Current byteRcvDropTotal on the active input group/session socket.", input_group_byte_drop_current);

    emit_u64("srt_relay_output_transport_byte_sent_total", "counter", "Monotonic output transport bytes sent on relay output SRT socket (includes retransmissions).", output_transport_byte_sent_total);
    emit_u64("srt_relay_output_transport_byte_sent_unique_total", "counter", "Monotonic output transport unique bytes sent on relay output SRT socket.", output_transport_byte_sent_unique_total);
    emit_u64("srt_relay_output_transport_byte_retrans_total", "counter", "Monotonic output transport retransmitted bytes on relay output SRT socket.", output_transport_byte_retrans_total);
    emit_u64("srt_relay_output_transport_byte_drop_total", "counter", "Monotonic output transport dropped bytes on relay output SRT socket.", output_transport_byte_drop_total);
    emit_i64("srt_relay_output_transport_members_tracked", "gauge", "Number of output SRT member sockets tracked for transport metrics.", output_transport_members_total);
    emit_u64("srt_relay_output_transport_byte_sent_current", "gauge", "Current byteSentTotal on relay output SRT socket.", output_transport_byte_sent_current);
    emit_u64("srt_relay_output_transport_byte_sent_unique_current", "gauge", "Current byteSentUniqueTotal on relay output SRT socket.", output_transport_byte_sent_unique_current);
    emit_u64("srt_relay_output_transport_byte_retrans_current", "gauge", "Current byteRetransTotal on relay output SRT socket.", output_transport_byte_retrans_current);
    emit_u64("srt_relay_output_transport_byte_drop_current", "gauge", "Current byteSndDropTotal on relay output SRT socket.", output_transport_byte_drop_current);

    emit_i64("srt_relay_input_rtt_ms", "gauge",
             "Input RTT in milliseconds; in bonded mode this is the maximum RTT among connected member links.",
             input_rtt_ms);
    emit_i64("srt_relay_output_rtt_ms", "gauge",
             "Output RTT in milliseconds; in bonded mode this is the maximum RTT among connected member links.",
             output_rtt_ms);
    emit_i64("srt_relay_input_effective_latency_ms",
             "gauge",
             "Input effective latency in milliseconds (negotiated latency when available, otherwise socket latency, else -1).",
             input_effective_latency_ms);
    emit_i64("srt_relay_output_effective_latency_ms",
             "gauge",
             "Output effective latency in milliseconds (negotiated latency when available, otherwise socket latency, else -1).",
             output_effective_latency_ms);
    out << "# HELP srt_relay_input_bond_mode Input bonded mode for active input session (1 for current mode, 0 otherwise).\n";
    out << "# TYPE srt_relay_input_bond_mode gauge\n";
    out << "srt_relay_input_bond_mode{mode=\"unknown\"} " << (input_bond_mode == 0 ? 1 : 0) << "\n";
    out << "srt_relay_input_bond_mode{mode=\"broadcast\"} " << (input_bond_mode == 1 ? 1 : 0) << "\n";
    out << "srt_relay_input_bond_mode{mode=\"backup\"} " << (input_bond_mode == 2 ? 1 : 0) << "\n";
    out << "# HELP srt_relay_output_bond_mode Output bonded mode for active output session (1 for current mode, 0 otherwise).\n";
    out << "# TYPE srt_relay_output_bond_mode gauge\n";
    out << "srt_relay_output_bond_mode{mode=\"unknown\"} " << (output_bond_mode == 0 ? 1 : 0) << "\n";
    out << "srt_relay_output_bond_mode{mode=\"broadcast\"} " << (output_bond_mode == 1 ? 1 : 0) << "\n";
    out << "srt_relay_output_bond_mode{mode=\"backup\"} " << (output_bond_mode == 2 ? 1 : 0) << "\n";
    out << "# HELP srt_relay_switch_policy Active input switch policy mode.\n";
    out << "# TYPE srt_relay_switch_policy gauge\n";
    out << "srt_relay_switch_policy{policy=\"round_robin\"} " << (switch_policy == 0 ? 1 : 0) << "\n";
    out << "srt_relay_switch_policy{policy=\"preferred_primary\"} " << (switch_policy == 1 ? 1 : 0) << "\n";
    out << "# HELP srt_relay_switch_mode Active input switch execution mode.\n";
    out << "# TYPE srt_relay_switch_mode gauge\n";
    out << "srt_relay_switch_mode{mode=\"serial\"} " << (switch_mode == 0 ? 1 : 0) << "\n";
    out << "srt_relay_switch_mode{mode=\"delayed\"} " << (switch_mode == 1 ? 1 : 0) << "\n";
    out << "# HELP srt_relay_input_source_connected Input source connection state by index.\n";
    out << "# TYPE srt_relay_input_source_connected gauge\n";
    out << "# HELP srt_relay_input_source_listening Input source listening state by index.\n";
    out << "# TYPE srt_relay_input_source_listening gauge\n";
    out << "# HELP srt_relay_input_source_bond_mode Configured bond mode by input index.\n";
    out << "# TYPE srt_relay_input_source_bond_mode gauge\n";
    const size_t source_count_capped = std::min(static_cast<size_t>(std::max<int64_t>(0, input_sources_total)),
                                                MetricsState::kMaxInputSources);
    for (size_t i = 0; i < source_count_capped; ++i) {
        const int connected_state = metrics.input_source_connected[i].load(std::memory_order_relaxed);
        const int listening_state = metrics.input_source_listening[i].load(std::memory_order_relaxed);
        const int bond_mode = metrics.input_source_bond_mode[i].load(std::memory_order_relaxed);
        out << "srt_relay_input_source_connected{input_index=\"" << (i + 1) << "\"} " << connected_state << "\n";
        out << "srt_relay_input_source_listening{input_index=\"" << (i + 1) << "\"} " << listening_state << "\n";
        out << "srt_relay_input_source_bond_mode{input_index=\"" << (i + 1)
            << "\",mode=\"unknown\"} " << (bond_mode == 0 ? 1 : 0) << "\n";
        out << "srt_relay_input_source_bond_mode{input_index=\"" << (i + 1)
            << "\",mode=\"broadcast\"} " << (bond_mode == 1 ? 1 : 0) << "\n";
        out << "srt_relay_input_source_bond_mode{input_index=\"" << (i + 1)
            << "\",mode=\"backup\"} " << (bond_mode == 2 ? 1 : 0) << "\n";
    }
    out << "# HELP srt_relay_output_source_connected Output sink connection state by index.\n";
    out << "# TYPE srt_relay_output_source_connected gauge\n";
    out << "# HELP srt_relay_output_source_listening Output sink listening state by index.\n";
    out << "# TYPE srt_relay_output_source_listening gauge\n";
    out << "# HELP srt_relay_output_source_bond_mode Configured bond mode by output index.\n";
    out << "# TYPE srt_relay_output_source_bond_mode gauge\n";
    out << "# HELP srt_relay_output_listener_clients_active Active downstream client count for output listener fanout by index.\n";
    out << "# TYPE srt_relay_output_listener_clients_active gauge\n";
    out << "# HELP srt_relay_output_listener_clients_accepted_total Total accepted downstream clients for output listener fanout by index.\n";
    out << "# TYPE srt_relay_output_listener_clients_accepted_total counter\n";
    out << "# HELP srt_relay_output_listener_clients_dropped_total Total dropped downstream clients for output listener fanout by index and reason.\n";
    out << "# TYPE srt_relay_output_listener_clients_dropped_total counter\n";
    out << "# HELP srt_relay_output_listener_accept_rejected_total Total output listener fanout accept rejections by index and reason.\n";
    out << "# TYPE srt_relay_output_listener_accept_rejected_total counter\n";
    const size_t output_source_count_capped = std::min(static_cast<size_t>(std::max<int64_t>(0, output_sources_total)),
                                                       MetricsState::kMaxOutputSources);
    for (size_t i = 0; i < output_source_count_capped; ++i) {
        const int connected_state = metrics.output_source_connected[i].load(std::memory_order_relaxed);
        const int listening_state = metrics.output_source_listening[i].load(std::memory_order_relaxed);
        const int bond_mode = metrics.output_source_bond_mode[i].load(std::memory_order_relaxed);
        const int64_t listener_clients_active =
            metrics.output_listener_clients_active[i].load(std::memory_order_relaxed);
        const uint64_t listener_clients_accepted_total =
            metrics.output_listener_clients_accepted_total[i].load(std::memory_order_relaxed);
        const uint64_t listener_clients_dropped_timeout_total =
            metrics.output_listener_clients_dropped_timeout_total[i].load(std::memory_order_relaxed);
        const uint64_t listener_clients_dropped_disconnected_total =
            metrics.output_listener_clients_dropped_disconnected_total[i].load(std::memory_order_relaxed);
        const uint64_t listener_clients_dropped_error_total =
            metrics.output_listener_clients_dropped_error_total[i].load(std::memory_order_relaxed);
        const uint64_t listener_accept_rejected_max_clients_total =
            metrics.output_listener_accept_rejected_max_clients_total[i].load(std::memory_order_relaxed);
        out << "srt_relay_output_source_connected{output_index=\"" << (i + 1) << "\"} " << connected_state << "\n";
        out << "srt_relay_output_source_listening{output_index=\"" << (i + 1) << "\"} " << listening_state << "\n";
        out << "srt_relay_output_source_bond_mode{output_index=\"" << (i + 1)
            << "\",mode=\"unknown\"} " << (bond_mode == 0 ? 1 : 0) << "\n";
        out << "srt_relay_output_source_bond_mode{output_index=\"" << (i + 1)
            << "\",mode=\"broadcast\"} " << (bond_mode == 1 ? 1 : 0) << "\n";
        out << "srt_relay_output_source_bond_mode{output_index=\"" << (i + 1)
            << "\",mode=\"backup\"} " << (bond_mode == 2 ? 1 : 0) << "\n";
        out << "srt_relay_output_listener_clients_active{output_index=\"" << (i + 1)
            << "\"} " << listener_clients_active << "\n";
        out << "srt_relay_output_listener_clients_accepted_total{output_index=\"" << (i + 1)
            << "\"} " << listener_clients_accepted_total << "\n";
        out << "srt_relay_output_listener_clients_dropped_total{output_index=\"" << (i + 1)
            << "\",reason=\"timeout\"} " << listener_clients_dropped_timeout_total << "\n";
        out << "srt_relay_output_listener_clients_dropped_total{output_index=\"" << (i + 1)
            << "\",reason=\"disconnected\"} " << listener_clients_dropped_disconnected_total << "\n";
        out << "srt_relay_output_listener_clients_dropped_total{output_index=\"" << (i + 1)
            << "\",reason=\"error\"} " << listener_clients_dropped_error_total << "\n";
        out << "srt_relay_output_listener_accept_rejected_total{output_index=\"" << (i + 1)
            << "\",reason=\"max_clients\"} " << listener_accept_rejected_max_clients_total << "\n";
    }
    emit_i64("srt_relay_last_rx_unix_seconds", "gauge", "Unix timestamp of last received packet.", last_rx_unix_seconds);
    emit_i64("srt_relay_last_tx_unix_seconds", "gauge", "Unix timestamp of last forwarded packet.", last_tx_unix_seconds);
    emit_i64("srt_relay_last_input_failure_unix_seconds", "gauge", "Unix timestamp of last input-side failure.", last_input_failure_unix_seconds);
    emit_i64("srt_relay_last_output_failure_unix_seconds", "gauge", "Unix timestamp of last output-side failure.", last_output_failure_unix_seconds);
    emit_i64("srt_relay_incident_active", "gauge", "Whether a causal incident is active (1/0).", incident_active);
    emit_i64("srt_relay_incident_age_seconds", "gauge", "Age of active incident in seconds.", incident_age_seconds);
    out << "# HELP srt_relay_timeouts_total Total timeout events by side and timeout type.\n";
    out << "# TYPE srt_relay_timeouts_total counter\n";
    for (FailureSide side : {FailureSide::kInput, FailureSide::kOutput}) {
        for (TimeoutType timeout_type : AllTimeoutTypes()) {
            out << "srt_relay_timeouts_total{side=\"" << FailureSideName(side)
                << "\",timeout_type=\"" << TimeoutTypeName(timeout_type) << "\"} "
                << metrics.timeouts_total[SideIndex(side)][TimeoutTypeIndex(timeout_type)].load(std::memory_order_relaxed)
                << "\n";
        }
    }
    out << "# HELP srt_relay_disconnects_total Total disconnect/failure events by side, class, and code.\n";
    out << "# TYPE srt_relay_disconnects_total counter\n";
    for (FailureSide side : {FailureSide::kInput, FailureSide::kOutput}) {
        for (ReasonCode code : AllReasonCodes()) {
            const ReasonClass reason_class = ReasonClassFromCode(code);
            out << "srt_relay_disconnects_total{side=\"" << FailureSideName(side)
                << "\",reason_class=\"" << ReasonClassName(reason_class)
                << "\",reason_code=\"" << ReasonCodeName(code) << "\"} "
                << metrics.disconnects_total[SideIndex(side)][ReasonCodeIndex(code)].load(std::memory_order_relaxed)
                << "\n";
        }
    }
    out << "# HELP srt_relay_reconnect_attempts_total Total reconnect attempts by side.\n";
    out << "# TYPE srt_relay_reconnect_attempts_total counter\n";
    for (FailureSide side : {FailureSide::kInput, FailureSide::kOutput}) {
        out << "srt_relay_reconnect_attempts_total{side=\"" << FailureSideName(side) << "\"} "
            << metrics.reconnect_attempts_total[SideIndex(side)].load(std::memory_order_relaxed)
            << "\n";
    }

    return out.str();
}

MetricsServer::MetricsServer(const Config& cfg,
                             const Logger& logger,
                             MetricsState& metrics,
                             const std::vector<InputEndpointSpec>& input_specs,
                             const std::vector<OutputEndpointSpec>& output_specs)
    : cfg_(cfg), logger_(logger), metrics_(metrics), input_specs_(&input_specs), output_specs_(&output_specs) {}

MetricsServer::~MetricsServer() { Stop(); }

void MetricsServer::Start() {
    if (!cfg_.metrics_enabled) {
        return;
    }
    server_.Get("/metrics", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content(RenderPrometheusMetrics(metrics_),
                        "text/plain; version=0.0.4; charset=utf-8");
    });
    server_.Get("/healthz", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content("ok\n", "text/plain");
    });
    server_.Get("/session/specs", [&](const httplib::Request&, httplib::Response& res) {
        if (input_specs_ == nullptr || output_specs_ == nullptr) {
            res.status = 500;
            res.set_content("{\"error\":\"session specs unavailable\"}\n",
                            "application/json; charset=utf-8");
            return;
        }
        res.set_content(BuildSessionSpecsJson(*input_specs_, *output_specs_, cfg_, metrics_) + "\n",
                        "application/json; charset=utf-8");
    });
    server_.Post("/metrics/links/compact", [&](const httplib::Request& req, httplib::Response& res) {
        CompactDirection direction = CompactDirection::kBoth;
        if (!ParseCompactDirection(req, &direction)) {
            res.status = 400;
            res.set_content(
                "{\"error\":\"invalid direction; expected input, output, or both\"}\n",
                "application/json; charset=utf-8");
            return;
        }

        CompactResponse response {};
        {
            MetricsState::LinkMetricsGuard lock(metrics_);
            switch (direction) {
                case CompactDirection::kInput:
                    response.direction = "input";
                    response.include_input = true;
                    response.input = CompactInputSlotsLocked(&metrics_);
                    break;
                case CompactDirection::kOutput:
                    response.direction = "output";
                    response.include_output = true;
                    response.output = CompactOutputSlotsLocked(&metrics_);
                    break;
                case CompactDirection::kBoth:
                    response.direction = "both";
                    response.include_input = true;
                    response.include_output = true;
                    response.input = CompactInputSlotsLocked(&metrics_);
                    response.output = CompactOutputSlotsLocked(&metrics_);
                    break;
            }
        }

        logger_.Log(LogLevel::kInfo,
                    "metrics-links-compacted",
                    "direction=" + response.direction,
                    "input_before=" + std::to_string(response.input.before_slots),
                    "input_after=" + std::to_string(response.input.after_slots),
                    "output_before=" + std::to_string(response.output.before_slots),
                    "output_after=" + std::to_string(response.output.after_slots));
        res.set_content(BuildCompactResponseJson(response) + "\n",
                        "application/json; charset=utf-8");
    });

    thread_ = std::thread([this]() {
        if (!server_.bind_to_port(cfg_.metrics_host, cfg_.metrics_port)) {
            logger_.Log(LogLevel::kError,
                        "metrics-bind-failed",
                        "host=" + cfg_.metrics_host,
                        "port=" + std::to_string(cfg_.metrics_port));
            return;
        }
        logger_.Log(LogLevel::kInfo,
                    "metrics-listening",
                    "host=" + cfg_.metrics_host,
                    "port=" + std::to_string(cfg_.metrics_port));
        server_.listen_after_bind();
        logger_.Log(LogLevel::kInfo, "metrics-stopped");
    });
}

void MetricsServer::Stop() {
    if (!cfg_.metrics_enabled) {
        return;
    }
    server_.stop();
    if (thread_.joinable()) {
        thread_.join();
    }
}

namespace {

struct IntervalRates {
    uint64_t rx_bps = 0;
    uint64_t tx_bps = 0;
    uint64_t rx_mps = 0;
    uint64_t tx_mps = 0;
};

IntervalRates ComputeIntervalRates(const RelayStats& stats, int64_t elapsed_ms) {
    const double sec = static_cast<double>(elapsed_ms) / 1000.0;
    return IntervalRates{
        static_cast<uint64_t>(stats.interval_rx_bytes / sec),
        static_cast<uint64_t>(stats.interval_tx_bytes / sec),
        static_cast<uint64_t>(stats.interval_rx_msgs / sec),
        static_cast<uint64_t>(stats.interval_tx_msgs / sec),
    };
}

SRTSOCKET ResolveOutputStatsSocket(SRTSOCKET output_sock, OutputMetricsMode output_metrics_mode) {
    return output_metrics_mode == OutputMetricsMode::kSrtSocket ? output_sock : SRT_INVALID_SOCK;
}

void CollectTickMetrics(MetricsState* metrics,
                        SRTSOCKET input_session_sock,
                        SRTSOCKET output_stats_sock) {
    MaybeUpdateRttMetric(input_session_sock, &metrics->input_rtt_ms);
    if (output_stats_sock != SRT_INVALID_SOCK) {
        MaybeUpdateRttMetric(output_stats_sock, &metrics->output_rtt_ms);
    } else {
        metrics->output_rtt_ms.store(0, std::memory_order_relaxed);
    }

    UpdateInputBondModeMetric(input_session_sock, metrics);
    UpdateOutputBondModeMetric(output_stats_sock, metrics);
    UpdateInputLinkHealthMetrics(input_session_sock, metrics);
    UpdateOutputLinkHealthMetrics(output_stats_sock, metrics);
    UpdateTransportTrafficMetrics(input_session_sock, output_stats_sock, metrics);

    int64_t max_input_link_rtt_ms = -1;
    if (TryComputeMaxConnectedInputLinkRtt(*metrics, &max_input_link_rtt_ms)) {
        metrics->input_rtt_ms.store(max_input_link_rtt_ms, std::memory_order_relaxed);
    }
    int64_t max_output_link_rtt_ms = -1;
    if (TryComputeMaxConnectedOutputLinkRtt(*metrics, &max_output_link_rtt_ms)) {
        metrics->output_rtt_ms.store(max_output_link_rtt_ms, std::memory_order_relaxed);
    }
}

void RefreshEffectiveLatencyMetrics(MetricsState* metrics, SRTSOCKET input_session_sock, SRTSOCKET output_stats_sock) {
    const RuntimeLatencySnapshot input_latency_snapshot = ReadRuntimeLatencySnapshot(input_session_sock);
    const RuntimeLatencySnapshot output_latency_snapshot = ReadRuntimeLatencySnapshot(output_stats_sock);
    metrics->input_effective_latency_ms.store(EffectiveLatencyMsOrMinusOne(input_latency_snapshot), std::memory_order_relaxed);
    metrics->output_effective_latency_ms.store(EffectiveLatencyMsOrMinusOne(output_latency_snapshot), std::memory_order_relaxed);
}

const char* BondModeName(int bond_mode) {
    if (bond_mode == 1) return "broadcast";
    if (bond_mode == 2) return "backup";
    return "unknown";
}

std::string FormatWithThousands(uint64_t value) {
    std::string digits = std::to_string(value);
    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3) {
        digits.insert(static_cast<size_t>(i), ",");
    }
    return digits;
}

std::string FormatSignedWithThousands(int64_t value) {
    if (value < 0) {
        return "-" + FormatWithThousands(static_cast<uint64_t>(-value));
    }
    return FormatWithThousands(static_cast<uint64_t>(value));
}

std::string FormatMbpsFromBytesPerSec(uint64_t bytes_per_sec) {
    const double mbps = (static_cast<double>(bytes_per_sec) * 8.0) / 1'000'000.0;
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(3) << mbps;
    return oss.str();
}

void PublishIntervalMetrics(const IntervalRates& rates, RelayStats* stats, MetricsState* metrics) {
    metrics->rx_bytes_per_sec.store(rates.rx_bps, std::memory_order_relaxed);
    metrics->tx_bytes_per_sec.store(rates.tx_bps, std::memory_order_relaxed);
    metrics->rx_msgs_per_sec.store(rates.rx_mps, std::memory_order_relaxed);
    metrics->tx_msgs_per_sec.store(rates.tx_mps, std::memory_order_relaxed);
    metrics->interval_send_failures.store(stats->interval_send_failures, std::memory_order_relaxed);
}

}  // namespace

void MaybeLogStats(const Config& cfg,
                   const Logger& logger,
                   RelayStats* stats,
                   const RelayState& state,
                   MetricsState* metrics,
                   SRTSOCKET input_session_sock,
                   SRTSOCKET output_sock,
                   OutputMetricsMode output_metrics_mode,
                   std::chrono::steady_clock::time_point* last_stats_at) {
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - *last_stats_at).count();
    if (elapsed_ms < cfg.stats_interval_ms) {
        return;
    }

    const IntervalRates rates = ComputeIntervalRates(*stats, elapsed_ms);
    const SRTSOCKET output_stats_sock = ResolveOutputStatsSocket(output_sock, output_metrics_mode);

    CollectTickMetrics(metrics, input_session_sock, output_stats_sock);
    RefreshEffectiveLatencyMetrics(metrics, input_session_sock, output_stats_sock);
    PublishIntervalMetrics(rates, stats, metrics);

    const auto input_rtt_ms = metrics->input_rtt_ms.load(std::memory_order_relaxed);
    const auto output_rtt_ms = metrics->output_rtt_ms.load(std::memory_order_relaxed);
    const auto input_links_total = metrics->input_links_total.load(std::memory_order_relaxed);
    const auto input_links_healthy = metrics->input_links_healthy.load(std::memory_order_relaxed);
    const auto input_links_running = metrics->input_links_running.load(std::memory_order_relaxed);
    const auto output_links_total = metrics->output_links_total.load(std::memory_order_relaxed);
    const auto output_links_healthy = metrics->output_links_healthy.load(std::memory_order_relaxed);
    const auto output_links_running = metrics->output_links_running.load(std::memory_order_relaxed);
    const auto input_transport_byte_recv_total = metrics->input_transport_byte_recv_total.load(std::memory_order_relaxed);
    const auto input_transport_byte_retrans_total = metrics->input_transport_byte_retrans_total.load(std::memory_order_relaxed);
    const auto input_transport_byte_loss_total = metrics->input_transport_byte_loss_total.load(std::memory_order_relaxed);
    const auto input_group_packet_drop_total = metrics->input_group_packet_drop_total.load(std::memory_order_relaxed);
    const auto input_group_byte_drop_total = metrics->input_group_byte_drop_total.load(std::memory_order_relaxed);
    const auto input_transport_packet_belated_total = metrics->input_transport_packet_belated_total.load(std::memory_order_relaxed);
    const auto output_transport_byte_sent_total = metrics->output_transport_byte_sent_total.load(std::memory_order_relaxed);
    const auto output_transport_byte_retrans_total = metrics->output_transport_byte_retrans_total.load(std::memory_order_relaxed);
    const auto output_transport_byte_drop_total = metrics->output_transport_byte_drop_total.load(std::memory_order_relaxed);
    const char* input_bond_mode_name = BondModeName(metrics->input_bond_mode.load(std::memory_order_relaxed));
    const char* output_bond_mode_name = BondModeName(metrics->output_bond_mode.load(std::memory_order_relaxed));
    const auto input_link_status = BuildInputLinkStatusCompact(*metrics);
    const auto output_link_status = BuildOutputLinkStatusCompact(*metrics);
    const auto input_effective_latency_ms = metrics->input_effective_latency_ms.load(std::memory_order_relaxed);
    const auto output_effective_latency_ms = metrics->output_effective_latency_ms.load(std::memory_order_relaxed);

    const auto input_transport_byte_retrans_interval = CounterDeltaWithReset(
        stats->last_input_transport_byte_retrans_total,
        input_transport_byte_retrans_total);
    const auto input_transport_byte_loss_interval = CounterDeltaWithReset(
        stats->last_input_transport_byte_loss_total,
        input_transport_byte_loss_total);
    const auto input_group_byte_drop_interval = CounterDeltaWithReset(
        stats->last_input_group_byte_drop_total,
        input_group_byte_drop_total);
    const auto input_transport_packet_belated_interval = CounterDeltaWithReset(
        stats->last_input_transport_packet_belated_total,
        input_transport_packet_belated_total);
    const auto input_transport_byte_recv_interval = CounterDeltaWithReset(
        stats->last_input_transport_byte_recv_total,
        input_transport_byte_recv_total);
    const auto output_transport_byte_retrans_interval = CounterDeltaWithReset(
        stats->last_output_transport_byte_retrans_total,
        output_transport_byte_retrans_total);
    const auto output_transport_byte_drop_interval = CounterDeltaWithReset(
        stats->last_output_transport_byte_drop_total,
        output_transport_byte_drop_total);

    logger.Log(LogLevel::kInfo,
               "stats-throughput",
               "Total forwarded: rx " + FormatWithThousands(stats->total_rx_bytes) + " bytes, tx " +
                   FormatWithThousands(stats->total_tx_bytes) + " bytes, rx " +
                   FormatWithThousands(stats->total_rx_msgs) + " packets, tx " +
                   FormatWithThousands(stats->total_tx_msgs) + " packets",
               "| Interval forwarded: rx " + FormatWithThousands(stats->interval_rx_bytes) + " bytes, tx " +
                   FormatWithThousands(stats->interval_tx_bytes) + " bytes, rx " +
                   FormatWithThousands(stats->interval_rx_msgs) + " packets, tx " +
                   FormatWithThousands(stats->interval_tx_msgs) + " packets",
               "| Rates: rx " + FormatMbpsFromBytesPerSec(rates.rx_bps) + " Mbps, tx " +
                   FormatMbpsFromBytesPerSec(rates.tx_bps) + " Mbps, rx " +
                   FormatWithThousands(rates.rx_mps) + " pps, tx " +
                   FormatWithThousands(rates.tx_mps) + " pps",
               "| Failures: send: " + FormatWithThousands(stats->interval_send_failures) +
                   " (interval), reconnects: " + FormatWithThousands(stats->reconnect_count) + " (total)");

    logger.Log(LogLevel::kInfo,
               "stats-input-transport",
               "Total recv: " + FormatWithThousands(input_transport_byte_recv_total) +
                   " bytes, retransmit: " + FormatWithThousands(input_transport_byte_retrans_total) +
                   " bytes, lost: " + FormatWithThousands(input_transport_byte_loss_total) +
                   " bytes, dropped: " + FormatWithThousands(input_group_byte_drop_total) + " bytes (" +
                   FormatWithThousands(input_group_packet_drop_total) + " packets), belated: " +
                   FormatWithThousands(input_transport_packet_belated_total) + " packets",
               "| Interval recv: " + FormatWithThousands(input_transport_byte_recv_interval) +
                   " bytes, retransmit: " + FormatWithThousands(input_transport_byte_retrans_interval) +
                   " bytes, lost: " + FormatWithThousands(input_transport_byte_loss_interval) +
                   " bytes, dropped: " + FormatWithThousands(input_group_byte_drop_interval) + " bytes, belated: " +
                   FormatWithThousands(input_transport_packet_belated_interval) + " packets");

    logger.Log(LogLevel::kInfo,
               "stats-output-transport",
               "Total sent: " + FormatWithThousands(output_transport_byte_sent_total) +
                   " bytes, retransmit: " + FormatWithThousands(output_transport_byte_retrans_total) +
                   " bytes, dropped: " + FormatWithThousands(output_transport_byte_drop_total) + " bytes",
               "| Interval sent: " + FormatWithThousands(stats->interval_tx_bytes) +
                   " bytes, retransmit: " + FormatWithThousands(output_transport_byte_retrans_interval) +
                   " bytes, dropped: " + FormatWithThousands(output_transport_byte_drop_interval) + " bytes");

    logger.Log(LogLevel::kInfo,
               "stats-link-health",
               "Link health: input " + FormatSignedWithThousands(input_links_running) + "/" +
                   FormatSignedWithThousands(input_links_total) + " running (" +
                   FormatSignedWithThousands(input_links_healthy) + " healthy), output " +
                   FormatSignedWithThousands(output_links_running) + "/" +
                   FormatSignedWithThousands(output_links_total) + " running (" +
                   FormatSignedWithThousands(output_links_healthy) + " healthy)",
               "| Latency/RTT: input delay: " + FormatSignedWithThousands(input_effective_latency_ms) +
                   " ms, output delay: " + FormatSignedWithThousands(output_effective_latency_ms) +
                   " ms, input RTT: " + FormatSignedWithThousands(input_rtt_ms) +
                   " ms, output RTT: " + FormatSignedWithThousands(output_rtt_ms) + " ms",
               std::string("| Modes: input: ") + input_bond_mode_name + ", output: " + output_bond_mode_name,
               std::string("| State: input: ") + (state.input_connected ? "connected" : (state.input_listening ? "listening" : "down")) +
                   ", output: " + (state.output_connected ? "connected" : "down"),
               "| Slot map: input[" + input_link_status + "] output[" + output_link_status + "]");

    stats->last_input_transport_byte_retrans_total = input_transport_byte_retrans_total;
    stats->last_input_transport_byte_loss_total = input_transport_byte_loss_total;
    stats->last_input_group_byte_drop_total = input_group_byte_drop_total;
    stats->last_input_transport_packet_belated_total = input_transport_packet_belated_total;
    stats->last_input_transport_byte_recv_total = input_transport_byte_recv_total;
    stats->last_output_transport_byte_retrans_total = output_transport_byte_retrans_total;
    stats->last_output_transport_byte_drop_total = output_transport_byte_drop_total;

    stats->interval_rx_bytes = 0;
    stats->interval_tx_bytes = 0;
    stats->interval_rx_msgs = 0;
    stats->interval_tx_msgs = 0;
    stats->interval_send_failures = 0;
    *last_stats_at = now;
}

void MaybeEmitMemberConnectionEvents(const Logger& logger,
                                     MetricsState* metrics,
                                     SRTSOCKET input_session_sock,
                                     SRTSOCKET output_sock,
                                     OutputMetricsMode output_metrics_mode,
                                     const std::string& active_incident_id,
                                     std::chrono::steady_clock::time_point* last_member_events_at) {
    constexpr int64_t kMemberEventIntervalMs = 1000;
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - *last_member_events_at).count();
    if (elapsed_ms < kMemberEventIntervalMs) {
        return;
    }

    const SRTSOCKET output_stats_sock = ResolveOutputStatsSocket(output_sock, output_metrics_mode);
    UpdateInputLinkHealthMetrics(input_session_sock, metrics);
    UpdateOutputLinkHealthMetrics(output_stats_sock, metrics);
    EmitMemberTransitionEvents(logger, metrics, active_incident_id);
    *last_member_events_at = now;
}

}  // namespace srtrelay
