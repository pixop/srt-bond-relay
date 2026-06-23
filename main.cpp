#include <algorithm>
#include <arpa/inet.h>
#include <dlfcn.h>
#include <netdb.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <array>
#include <chrono>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <initializer_list>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <srt.h>
#include <httplib.h>

namespace {

std::atomic<bool> g_shutdown_requested{false};

void OnSignal(int) {
    g_shutdown_requested.store(true);
}

std::string NowIso8601() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t tt = system_clock::to_time_t(now);
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tm_utc {};
    gmtime_r(&tt, &tm_utc);

    std::ostringstream oss;
    oss << std::put_time(&tm_utc, "%Y-%m-%dT%H:%M:%S")
        << "." << std::setfill('0') << std::setw(3) << ms.count() << "Z";
    return oss.str();
}

enum class LogLevel {
    kDebug = 0,
    kInfo = 1,
    kWarn = 2,
    kError = 3,
};

LogLevel ParseLogLevel(const std::string& value) {
    if (value == "debug") return LogLevel::kDebug;
    if (value == "info") return LogLevel::kInfo;
    if (value == "warn") return LogLevel::kWarn;
    if (value == "error") return LogLevel::kError;
    throw std::runtime_error("invalid --log-level: " + value);
}

const char* LogLevelName(LogLevel level) {
    switch (level) {
        case LogLevel::kDebug: return "debug";
        case LogLevel::kInfo: return "info";
        case LogLevel::kWarn: return "warn";
        case LogLevel::kError: return "error";
    }
    return "unknown";
}

struct Logger {
    LogLevel min_level = LogLevel::kInfo;

    template <typename... Parts>
    void Log(LogLevel level, const char* event, Parts&&... parts) const {
        if (static_cast<int>(level) < static_cast<int>(min_level)) {
            return;
        }
        std::ostringstream oss;
        oss << "ts=" << NowIso8601()
            << " level=" << LogLevelName(level)
            << " event=" << event;
        ((oss << " " << std::forward<Parts>(parts)), ...);
        std::cout << oss.str() << std::endl;
    }
};

std::string SrtLastErrorString() {
    const char* err = srt_getlasterror_str();
    return err ? std::string(err) : "unknown-srt-error";
}

int SrtLastErrorCode() {
    int syserr = 0;
    return srt_getlasterror(&syserr);
}

bool IsSrtTimeoutError() {
    return SrtLastErrorCode() == SRT_ETIMEOUT;
}

struct Config {
    std::string input_uri;
    std::string output_uri;
    int stats_interval_ms = 1000;
    int reconnect_delay_ms = 1000;
    int max_message_size = 1456;
    LogLevel log_level = LogLevel::kInfo;
    bool exit_on_input_failure = false;
    bool exit_on_output_failure = false;
    bool verify_linkage = false;
    int io_timeout_ms = 1000;
    bool metrics_enabled = true;
    std::string metrics_host = "127.0.0.1";
    int metrics_port = 9464;
};

void PrintUsage() {
    std::cout
        << "Usage:\n"
        << "  srt-bond-relay \\\n"
        << "    --input '<bonded-srt-input-uri-or-group>' \\\n"
        << "    --output 'srt://127.0.0.1:5000?mode=caller&transtype=live&latency=20' \\\n"
        << "    --stats-interval-ms 1000 \\\n"
        << "    --reconnect-delay-ms 1000\n\n"
        << "Optional flags:\n"
        << "  --max-message-size 1456\n"
        << "  --log-level info|debug|warn|error\n"
        << "  --exit-on-input-failure true|false\n"
        << "  --exit-on-output-failure true|false\n"
        << "  --verify-linkage\n"
        << "  --io-timeout-ms 1000\n"
        << "  --metrics-enabled true|false\n"
        << "  --metrics-host 127.0.0.1\n"
        << "  --metrics-port 9464\n"
        << "  --help\n";
}

bool ParseBool(const std::string& value) {
    if (value == "true" || value == "1" || value == "yes") return true;
    if (value == "false" || value == "0" || value == "no") return false;
    throw std::runtime_error("invalid boolean value: " + value);
}

Config ParseArgs(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto require_value = [&](const char* key) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error(std::string("missing value for ") + key);
            }
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h") {
            PrintUsage();
            std::exit(0);
        } else if (arg == "--input") {
            cfg.input_uri = require_value("--input");
        } else if (arg == "--output") {
            cfg.output_uri = require_value("--output");
        } else if (arg == "--stats-interval-ms") {
            cfg.stats_interval_ms = std::stoi(require_value("--stats-interval-ms"));
        } else if (arg == "--reconnect-delay-ms") {
            cfg.reconnect_delay_ms = std::stoi(require_value("--reconnect-delay-ms"));
        } else if (arg == "--max-message-size") {
            cfg.max_message_size = std::stoi(require_value("--max-message-size"));
        } else if (arg == "--log-level") {
            cfg.log_level = ParseLogLevel(require_value("--log-level"));
        } else if (arg == "--exit-on-input-failure") {
            cfg.exit_on_input_failure = ParseBool(require_value("--exit-on-input-failure"));
        } else if (arg == "--exit-on-output-failure") {
            cfg.exit_on_output_failure = ParseBool(require_value("--exit-on-output-failure"));
        } else if (arg == "--verify-linkage") {
            cfg.verify_linkage = true;
        } else if (arg == "--io-timeout-ms") {
            cfg.io_timeout_ms = std::stoi(require_value("--io-timeout-ms"));
        } else if (arg == "--metrics-enabled") {
            cfg.metrics_enabled = ParseBool(require_value("--metrics-enabled"));
        } else if (arg == "--metrics-host") {
            cfg.metrics_host = require_value("--metrics-host");
        } else if (arg == "--metrics-port") {
            cfg.metrics_port = std::stoi(require_value("--metrics-port"));
        } else {
            throw std::runtime_error("unknown argument: " + arg);
        }
    }

    if (!cfg.verify_linkage) {
        if (cfg.input_uri.empty()) throw std::runtime_error("--input is required");
        if (cfg.output_uri.empty()) throw std::runtime_error("--output is required");
    }

    if (cfg.stats_interval_ms <= 0) throw std::runtime_error("--stats-interval-ms must be > 0");
    if (cfg.reconnect_delay_ms <= 0) throw std::runtime_error("--reconnect-delay-ms must be > 0");
    if (cfg.max_message_size <= 0) throw std::runtime_error("--max-message-size must be > 0");
    if (cfg.io_timeout_ms <= 0) throw std::runtime_error("--io-timeout-ms must be > 0");
    if (cfg.metrics_port <= 0 || cfg.metrics_port > 65535) {
        throw std::runtime_error("--metrics-port must be in range 1..65535");
    }
    if (cfg.metrics_host.empty()) {
        throw std::runtime_error("--metrics-host must not be empty");
    }
    return cfg;
}

struct SrtUri {
    std::string host;
    int port = 0;
    std::map<std::string, std::string> query;
};

std::string PercentDecode(std::string value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const char hex[] = {value[i + 1], value[i + 2], '\0'};
            char* end = nullptr;
            const long code = std::strtol(hex, &end, 16);
            if (end && *end == '\0') {
                out.push_back(static_cast<char>(code));
                i += 2;
                continue;
            }
        }
        if (value[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(value[i]);
        }
    }
    return out;
}

SrtUri ParseSrtUri(const std::string& uri) {
    const std::string prefix = "srt://";
    if (uri.rfind(prefix, 0) != 0) {
        throw std::runtime_error("only srt:// URIs are supported: " + uri);
    }

    const std::string rest = uri.substr(prefix.size());
    const size_t qpos = rest.find('?');
    const std::string authority = rest.substr(0, qpos);
    const std::string query = (qpos == std::string::npos) ? "" : rest.substr(qpos + 1);

    const size_t colon = authority.rfind(':');
    if (colon == std::string::npos) {
        throw std::runtime_error("SRT URI must include host:port: " + uri);
    }
    SrtUri parsed;
    parsed.host = authority.substr(0, colon);
    parsed.port = std::stoi(authority.substr(colon + 1));
    if (parsed.port <= 0 || parsed.port > 65535) {
        throw std::runtime_error("invalid port in URI: " + uri);
    }
    if (parsed.host.empty()) {
        parsed.host = "0.0.0.0";
    }

    size_t start = 0;
    while (start < query.size()) {
        const size_t amp = query.find('&', start);
        const std::string kv = query.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
        if (!kv.empty()) {
            const size_t eq = kv.find('=');
            std::string key = kv.substr(0, eq);
            std::string value = (eq == std::string::npos) ? "" : kv.substr(eq + 1);
            if (!key.empty()) {
                parsed.query[PercentDecode(key)] = PercentDecode(value);
            }
        }
        if (amp == std::string::npos) break;
        start = amp + 1;
    }
    return parsed;
}

const std::string* QueryFirstValue(const std::map<std::string, std::string>& query,
                                   std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        const auto it = query.find(key);
        if (it != query.end()) return &it->second;
    }
    return nullptr;
}

int ParseIntOptionValue(const std::string& value, const char* opt_name) {
    try {
        return std::stoi(value);
    } catch (const std::exception&) {
        throw std::runtime_error(std::string("invalid integer value for ") + opt_name + ": " + value);
    }
}

std::string QueryString(const std::map<std::string, std::string>& query, const std::string& key) {
    const auto it = query.find(key);
    return it == query.end() ? "" : it->second;
}

void ApplyIntSockOpt(SRTSOCKET sock, SRT_SOCKOPT opt, int value, const char* opt_name) {
    if (srt_setsockflag(sock, opt, &value, sizeof(value)) == SRT_ERROR) {
        throw std::runtime_error(std::string("failed to set ") + opt_name + ": " + SrtLastErrorString());
    }
}

void ApplyStringSockOpt(SRTSOCKET sock, SRT_SOCKOPT opt, const std::string& value, const char* opt_name) {
    if (value.empty()) return;
    if (srt_setsockflag(sock, opt, value.data(), static_cast<int>(value.size())) == SRT_ERROR) {
        throw std::runtime_error(std::string("failed to set ") + opt_name + ": " + SrtLastErrorString());
    }
}

void ApplyLingerSockOpt(SRTSOCKET sock, int seconds) {
    linger lin {};
    lin.l_linger = seconds;
    lin.l_onoff = (seconds > 0) ? 1 : 0;
    if (srt_setsockflag(sock, SRTO_LINGER, &lin, sizeof(lin)) == SRT_ERROR) {
        throw std::runtime_error(std::string("failed to set SRTO_LINGER: ") + SrtLastErrorString());
    }
}

void ApplyCommonSrtOptions(SRTSOCKET sock, const SrtUri& uri, const Logger& logger) {
    if (const std::string* v = QueryFirstValue(uri.query, {"passphrase"})) {
        ApplyStringSockOpt(sock, SRTO_PASSPHRASE, *v, "SRTO_PASSPHRASE");
    }
    if (const std::string* v = QueryFirstValue(uri.query, {"pbkeylen"})) {
        ApplyIntSockOpt(sock, SRTO_PBKEYLEN, ParseIntOptionValue(*v, "pbkeylen"), "SRTO_PBKEYLEN");
    }
    if (const std::string* v = QueryFirstValue(uri.query, {"latency"})) {
        ApplyIntSockOpt(sock, SRTO_LATENCY, ParseIntOptionValue(*v, "latency"), "SRTO_LATENCY");
    }
    if (const std::string* v = QueryFirstValue(uri.query, {"peeridletimeo"})) {
        ApplyIntSockOpt(sock, SRTO_PEERIDLETIMEO, ParseIntOptionValue(*v, "peeridletimeo"), "SRTO_PEERIDLETIMEO");
    }
    if (const std::string* v = QueryFirstValue(uri.query, {"conntimeo"})) {
        ApplyIntSockOpt(sock, SRTO_CONNTIMEO, ParseIntOptionValue(*v, "conntimeo"), "SRTO_CONNTIMEO");
    }
    if (const std::string* v = QueryFirstValue(uri.query, {"linger"})) {
        ApplyLingerSockOpt(sock, ParseIntOptionValue(*v, "linger"));
    }
    if (const std::string* v = QueryFirstValue(uri.query, {"rcvbuf"})) {
        ApplyIntSockOpt(sock, SRTO_RCVBUF, ParseIntOptionValue(*v, "rcvbuf"), "SRTO_RCVBUF");
    }
    if (const std::string* v = QueryFirstValue(uri.query, {"sndbuf"})) {
        ApplyIntSockOpt(sock, SRTO_SNDBUF, ParseIntOptionValue(*v, "sndbuf"), "SRTO_SNDBUF");
    }
    if (const std::string* v = QueryFirstValue(uri.query, {"oheadbw"})) {
        ApplyIntSockOpt(sock, SRTO_OHEADBW, ParseIntOptionValue(*v, "oheadbw"), "SRTO_OHEADBW");
    }
    if (const std::string* v = QueryFirstValue(uri.query, {"streamid"})) {
        ApplyStringSockOpt(sock, SRTO_STREAMID, *v, "SRTO_STREAMID");
    }
    if (const std::string* v = QueryFirstValue(uri.query, {"transtype"})) {
        int tt = SRTT_LIVE;
        if (*v == "live") {
            tt = SRTT_LIVE;
        } else if (*v == "file") {
            tt = SRTT_FILE;
        } else {
            throw std::runtime_error("unsupported transtype value: " + *v);
        }
        ApplyIntSockOpt(sock, SRTO_TRANSTYPE, tt, "SRTO_TRANSTYPE");
    }

    for (const auto& [key, value] : uri.query) {
        if (key == "mode" ||
            key == "passphrase" ||
            key == "pbkeylen" ||
            key == "latency" ||
            key == "peeridletimeo" ||
            key == "conntimeo" ||
            key == "linger" ||
            key == "rcvbuf" ||
            key == "sndbuf" ||
            key == "oheadbw" ||
            key == "streamid" ||
            key == "transtype") {
            continue;
        }
        logger.Log(LogLevel::kDebug, "uri-option-ignored", "key=" + key, "value=" + value);
    }
}

sockaddr_storage ResolveSockaddr(const std::string& host, int port, socklen_t* out_len) {
    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    addrinfo* result = nullptr;
    const std::string service = std::to_string(port);
    const int rc = getaddrinfo(host.c_str(), service.c_str(), &hints, &result);
    if (rc != 0 || result == nullptr) {
        throw std::runtime_error("getaddrinfo failed for " + host + ":" + service);
    }

    sockaddr_storage storage {};
    std::memcpy(&storage, result->ai_addr, result->ai_addrlen);
    *out_len = static_cast<socklen_t>(result->ai_addrlen);
    freeaddrinfo(result);
    return storage;
}

class SrtSocketHolder {
public:
    ~SrtSocketHolder() { Reset(); }
    SrtSocketHolder() = default;
    SrtSocketHolder(const SrtSocketHolder&) = delete;
    SrtSocketHolder& operator=(const SrtSocketHolder&) = delete;

    void Set(SRTSOCKET s) {
        Reset();
        sock_ = s;
    }

    void Reset() {
        if (sock_ != SRT_INVALID_SOCK) {
            srt_close(sock_);
            sock_ = SRT_INVALID_SOCK;
        }
    }

    SRTSOCKET Get() const { return sock_; }
    bool Valid() const { return sock_ != SRT_INVALID_SOCK; }

private:
    SRTSOCKET sock_ = SRT_INVALID_SOCK;
};

struct RelayStats {
    uint64_t total_rx_bytes = 0;
    uint64_t total_tx_bytes = 0;
    uint64_t total_rx_msgs = 0;
    uint64_t total_tx_msgs = 0;
    uint64_t total_send_failures = 0;
    uint64_t reconnect_count = 0;

    uint64_t interval_rx_bytes = 0;
    uint64_t interval_tx_bytes = 0;
    uint64_t interval_rx_msgs = 0;
    uint64_t interval_tx_msgs = 0;
    uint64_t interval_send_failures = 0;
};

struct RelayState {
    bool input_listening = false;
    bool input_connected = false;
    bool output_connected = false;
};

int64_t UnixNowMs() {
    const auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

struct TransportCounterSnapshot {
    uint64_t byte_recv_total = 0;
    uint64_t byte_recv_unique_total = 0;
    uint64_t byte_retrans_total = 0;
    uint64_t byte_loss_total = 0;
    uint64_t byte_sent_total = 0;
    uint64_t byte_sent_unique_total = 0;
    uint64_t byte_drop_total = 0;
};

struct MetricsState {
    std::atomic<uint64_t> total_rx_bytes{0};
    std::atomic<uint64_t> total_tx_bytes{0};
    std::atomic<uint64_t> total_rx_msgs{0};
    std::atomic<uint64_t> total_tx_msgs{0};
    std::atomic<uint64_t> total_send_failures{0};
    std::atomic<uint64_t> reconnect_count{0};

    std::atomic<uint64_t> rx_bytes_per_sec{0};
    std::atomic<uint64_t> tx_bytes_per_sec{0};
    std::atomic<uint64_t> rx_msgs_per_sec{0};
    std::atomic<uint64_t> tx_msgs_per_sec{0};
    std::atomic<uint64_t> interval_send_failures{0};

    std::atomic<int> input_listening{0};
    std::atomic<int> input_connected{0};
    std::atomic<int> output_connected{0};
    std::atomic<int64_t> input_links_total{0};
    std::atomic<int64_t> input_links_healthy{0};
    std::atomic<int64_t> input_links_running{0};
    std::atomic<int64_t> input_links_snapshot_count{0};

    std::atomic<uint64_t> input_transport_byte_recv_total{0};
    std::atomic<uint64_t> input_transport_byte_recv_unique_total{0};
    std::atomic<uint64_t> input_transport_byte_retrans_total{0};
    std::atomic<uint64_t> input_transport_byte_loss_total{0};
    std::atomic<int64_t> input_transport_members_total{0};
    std::atomic<uint64_t> input_transport_byte_recv_current{0};
    std::atomic<uint64_t> input_transport_byte_recv_unique_current{0};
    std::atomic<uint64_t> input_transport_byte_retrans_current{0};
    std::atomic<uint64_t> input_transport_byte_loss_current{0};

    std::atomic<uint64_t> output_transport_byte_sent_total{0};
    std::atomic<uint64_t> output_transport_byte_sent_unique_total{0};
    std::atomic<uint64_t> output_transport_byte_retrans_total{0};
    std::atomic<uint64_t> output_transport_byte_drop_total{0};
    std::atomic<uint64_t> output_transport_byte_sent_current{0};
    std::atomic<uint64_t> output_transport_byte_sent_unique_current{0};
    std::atomic<uint64_t> output_transport_byte_retrans_current{0};
    std::atomic<uint64_t> output_transport_byte_drop_current{0};

    std::atomic<int64_t> input_rtt_ms{-1};
    std::atomic<int64_t> output_rtt_ms{-1};

    std::atomic<int64_t> last_rx_unix_ms{0};
    std::atomic<int64_t> last_tx_unix_ms{0};

    static constexpr size_t kMaxTrackedMembers = 16;
    std::array<std::atomic<int64_t>, kMaxTrackedMembers> input_member_ids {};
    std::array<std::atomic<int>, kMaxTrackedMembers> input_member_connected {};
    std::array<std::atomic<uint64_t>, kMaxTrackedMembers> input_member_identity_keys {};

    std::unordered_map<SRTSOCKET, TransportCounterSnapshot> input_transport_last_by_socket;
    TransportCounterSnapshot output_transport_last;
    SRTSOCKET output_transport_last_socket = SRT_INVALID_SOCK;

    MetricsState() {
        for (size_t i = 0; i < kMaxTrackedMembers; ++i) {
            input_member_ids[i].store(static_cast<int64_t>(SRT_INVALID_SOCK), std::memory_order_relaxed);
            input_member_connected[i].store(0, std::memory_order_relaxed);
            input_member_identity_keys[i].store(0, std::memory_order_relaxed);
        }
    }
};

void MaybeUpdateRttMetric(SRTSOCKET sock, std::atomic<int64_t>* out_rtt_ms) {
    if (sock == SRT_INVALID_SOCK) {
        out_rtt_ms->store(-1, std::memory_order_relaxed);
        return;
    }

    SRT_TRACEBSTATS sock_stats {};
    if (srt_bstats(sock, &sock_stats, 1) == SRT_ERROR) {
        return;
    }
    out_rtt_ms->store(static_cast<int64_t>(sock_stats.msRTT), std::memory_order_relaxed);
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

uint64_t HashBytesFnv1a64(const void* data, size_t size, uint64_t seed = 1469598103934665603ULL) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint64_t hash = seed;
    for (size_t i = 0; i < size; ++i) {
        hash ^= static_cast<uint64_t>(bytes[i]);
        hash *= 1099511628211ULL;
    }
    return hash;
}

uint64_t InputMemberIdentityKey(const SRT_SOCKGROUPDATA& member) {
    // Use remote IP as stable physical-link identity. Socket IDs and source ports can change on reconnect.
    const auto family = member.peeraddr.ss_family;
    if (family == AF_INET) {
        const auto* v4 = reinterpret_cast<const sockaddr_in*>(&member.peeraddr);
        const uint64_t hash = HashBytesFnv1a64(&(v4->sin_addr), sizeof(v4->sin_addr));
        return hash == 0 ? 1 : hash;
    }
    if (family == AF_INET6) {
        const auto* v6 = reinterpret_cast<const sockaddr_in6*>(&member.peeraddr);
        const uint64_t hash = HashBytesFnv1a64(&(v6->sin6_addr), sizeof(v6->sin6_addr));
        return hash == 0 ? 1 : hash;
    }
    // Fallback when libsrt does not provide a peer address for this member snapshot.
    return static_cast<uint64_t>(member.id) == 0 ? 1 : static_cast<uint64_t>(member.id);
}

bool IsGroupSocketId(SRTSOCKET sock) {
    if (sock == SRT_INVALID_SOCK) {
        return false;
    }
    return (sock & SRTGROUP_MASK) != 0;
}

void UpdateInputLinkHealthFromGroupMembers(const std::vector<SRT_SOCKGROUPDATA>& group_members,
                                           MetricsState* metrics) {
    int64_t healthy_count = 0;
    int64_t running_count = 0;
    for (const auto& member : group_members) {
        if (IsHealthyGroupMember(member)) {
            healthy_count += 1;
        }
        if (member.memberstate == SRT_GST_RUNNING) {
            running_count += 1;
        }
    }

    metrics->input_links_total.store(static_cast<int64_t>(group_members.size()), std::memory_order_relaxed);
    metrics->input_links_healthy.store(healthy_count, std::memory_order_relaxed);
    metrics->input_links_running.store(running_count, std::memory_order_relaxed);
}

void SaveInputMemberSnapshot(const std::vector<SRT_SOCKGROUPDATA>& group_members, MetricsState* metrics) {
    std::array<SRTSOCKET, MetricsState::kMaxTrackedMembers> slot_ids {};
    std::array<int, MetricsState::kMaxTrackedMembers> slot_connected {};
    std::array<uint64_t, MetricsState::kMaxTrackedMembers> slot_identity_keys {};
    for (size_t i = 0; i < MetricsState::kMaxTrackedMembers; ++i) {
        slot_ids[i] = static_cast<SRTSOCKET>(metrics->input_member_ids[i].load(std::memory_order_relaxed));
        slot_connected[i] = metrics->input_member_connected[i].load(std::memory_order_relaxed);
        slot_identity_keys[i] = metrics->input_member_identity_keys[i].load(std::memory_order_relaxed);
    }

    struct CurrentMemberSlotData {
        SRTSOCKET id = SRT_INVALID_SOCK;
        int connected = 0;
    };
    std::unordered_map<uint64_t, CurrentMemberSlotData> current_by_identity_key;
    for (const auto& member : group_members) {
        current_by_identity_key[InputMemberIdentityKey(member)] = {
            member.id,
            IsConnectedGroupMember(member) ? 1 : 0
        };
    }

    for (size_t i = 0; i < MetricsState::kMaxTrackedMembers; ++i) {
        if (slot_identity_keys[i] == 0) {
            slot_connected[i] = 0;
            continue;
        }

        const auto it = current_by_identity_key.find(slot_identity_keys[i]);
        if (it != current_by_identity_key.end()) {
            slot_ids[i] = it->second.id;
            slot_connected[i] = it->second.connected;
            current_by_identity_key.erase(it);
        } else {
            // Keep slot identity stable when a link disappears so indexes do not collapse.
            slot_connected[i] = 0;
        }
    }

    std::vector<std::pair<uint64_t, CurrentMemberSlotData>> remaining;
    remaining.reserve(current_by_identity_key.size());
    for (const auto& entry : current_by_identity_key) {
        remaining.push_back(entry);
    }
    std::sort(remaining.begin(), remaining.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    for (const auto& entry : remaining) {
        size_t slot = MetricsState::kMaxTrackedMembers;
        for (size_t i = 0; i < MetricsState::kMaxTrackedMembers; ++i) {
            if (slot_identity_keys[i] == 0) {
                slot = i;
                break;
            }
        }
        if (slot == MetricsState::kMaxTrackedMembers) {
            break;
        }
        slot_identity_keys[slot] = entry.first;
        slot_ids[slot] = entry.second.id;
        slot_connected[slot] = entry.second.connected;
    }

    size_t span = 0;
    for (size_t i = 0; i < MetricsState::kMaxTrackedMembers; ++i) {
        metrics->input_member_ids[i].store(static_cast<int64_t>(slot_ids[i]), std::memory_order_relaxed);
        metrics->input_member_connected[i].store(slot_connected[i], std::memory_order_relaxed);
        metrics->input_member_identity_keys[i].store(slot_identity_keys[i], std::memory_order_relaxed);
        if (slot_identity_keys[i] != 0) {
            span = i + 1;
        }
    }
    metrics->input_links_snapshot_count.store(static_cast<int64_t>(span), std::memory_order_relaxed);
}

std::string BuildInputLinkStatusCompact(const MetricsState& metrics) {
    int64_t count = metrics.input_links_snapshot_count.load(std::memory_order_relaxed);
    if (count < 0) {
        count = 0;
    }
    const size_t capped = std::min(static_cast<size_t>(count), MetricsState::kMaxTrackedMembers);
    if (capped == 0) {
        return "none";
    }

    std::ostringstream out;
    bool emitted = false;
    for (size_t i = 0; i < capped; ++i) {
        const auto socket_id = static_cast<SRTSOCKET>(metrics.input_member_ids[i].load(std::memory_order_relaxed));
        if (socket_id == SRT_INVALID_SOCK) {
            continue;
        }
        if (emitted) {
            out << ",";
        }
        const auto is_connected = metrics.input_member_connected[i].load(std::memory_order_relaxed) == 1;
        out << "l" << (i + 1) << ":" << (is_connected ? "up" : "down");
        emitted = true;
    }
    if (!emitted) {
        return "none";
    }
    return out.str();
}

void MarkAllTrackedInputLinksDisconnected(MetricsState* metrics) {
    int64_t count = metrics->input_links_snapshot_count.load(std::memory_order_relaxed);
    if (count < 0) {
        count = 0;
    }
    const size_t capped = std::min(static_cast<size_t>(count), MetricsState::kMaxTrackedMembers);
    for (size_t i = 0; i < capped; ++i) {
        const auto identity_key = metrics->input_member_identity_keys[i].load(std::memory_order_relaxed);
        if (identity_key == 0) {
            continue;
        }
        metrics->input_member_connected[i].store(0, std::memory_order_relaxed);
    }
}

bool TryReadGroupMembers(SRTSOCKET group_sock, std::vector<SRT_SOCKGROUPDATA>* out_members) {
    if (group_sock == SRT_INVALID_SOCK) {
        return false;
    }

    // Start with a moderate buffer and grow if libsrt reports a larger group.
    std::vector<SRT_SOCKGROUPDATA> group_members(16);
    size_t member_count = group_members.size();
    int rc = srt_group_data(group_sock, group_members.data(), &member_count);
    if (rc == SRT_ERROR) {
        if (member_count > group_members.size()) {
            group_members.resize(member_count);
            rc = srt_group_data(group_sock, group_members.data(), &member_count);
        }
        if (rc == SRT_ERROR) {
            return false;
        }
    }

    group_members.resize(member_count);
    *out_members = std::move(group_members);
    return true;
}

void UpdateInputLinkHealthMetrics(SRTSOCKET input_session_sock, MetricsState* metrics) {
    if (input_session_sock == SRT_INVALID_SOCK) {
        metrics->input_links_total.store(0, std::memory_order_relaxed);
        metrics->input_links_healthy.store(0, std::memory_order_relaxed);
        metrics->input_links_running.store(0, std::memory_order_relaxed);
        MarkAllTrackedInputLinksDisconnected(metrics);
        return;
    }

    std::vector<SRT_SOCKGROUPDATA> group_members;
    bool have_group_members = false;

    // Some libsrt flows expose the accepted bonded session as a group socket directly,
    // while others expose a regular socket with an associated group.
    if (IsGroupSocketId(input_session_sock)) {
        have_group_members = TryReadGroupMembers(input_session_sock, &group_members);
    }
    if (!have_group_members) {
        const SRTSOCKET input_group_sock = srt_groupof(input_session_sock);
        have_group_members = TryReadGroupMembers(input_group_sock, &group_members);
    }
    if (!have_group_members) {
        return;
    }
    UpdateInputLinkHealthFromGroupMembers(group_members, metrics);
    SaveInputMemberSnapshot(group_members, metrics);
}

void UpdateInputLinkHealthFromMsgCtrl(const SRT_MSGCTRL& rx_ctrl, MetricsState* metrics) {
    if (rx_ctrl.grpdata == nullptr || rx_ctrl.grpdata_size == 0) {
        return;
    }
    std::vector<SRT_SOCKGROUPDATA> group_members(rx_ctrl.grpdata, rx_ctrl.grpdata + rx_ctrl.grpdata_size);
    UpdateInputLinkHealthFromGroupMembers(group_members, metrics);
    SaveInputMemberSnapshot(group_members, metrics);
}

std::vector<SRTSOCKET> GetInputMemberSocketsSnapshot(const MetricsState& metrics) {
    std::vector<SRTSOCKET> sockets;
    int64_t count = metrics.input_links_snapshot_count.load(std::memory_order_relaxed);
    if (count < 0) {
        count = 0;
    }
    const size_t capped = std::min(static_cast<size_t>(count), MetricsState::kMaxTrackedMembers);
    sockets.reserve(capped);
    for (size_t i = 0; i < capped; ++i) {
        if (metrics.input_member_connected[i].load(std::memory_order_relaxed) != 1) {
            continue;
        }
        const auto id = static_cast<SRTSOCKET>(metrics.input_member_ids[i].load(std::memory_order_relaxed));
        if (id != SRT_INVALID_SOCK) {
            sockets.push_back(id);
        }
    }
    std::sort(sockets.begin(), sockets.end());
    sockets.erase(std::unique(sockets.begin(), sockets.end()), sockets.end());
    return sockets;
}

uint64_t CounterDeltaWithReset(uint64_t previous, uint64_t current) {
    if (current >= previous) {
        return current - previous;
    }
    // Socket counter reset (reconnect/socket reuse): keep monotonicity by taking current value as new delta.
    return current;
}

void UpdateTransportTrafficMetrics(SRTSOCKET output_sock, MetricsState* metrics) {
    const auto member_sockets = GetInputMemberSocketsSnapshot(*metrics);

    uint64_t input_byte_recv_current = 0;
    uint64_t input_byte_recv_unique_current = 0;
    uint64_t input_byte_retrans_current = 0;
    uint64_t input_byte_loss_current = 0;
    uint64_t input_byte_recv_total = metrics->input_transport_byte_recv_total.load(std::memory_order_relaxed);
    uint64_t input_byte_recv_unique_total = metrics->input_transport_byte_recv_unique_total.load(std::memory_order_relaxed);
    uint64_t input_byte_retrans_total = metrics->input_transport_byte_retrans_total.load(std::memory_order_relaxed);
    uint64_t input_byte_loss_total = metrics->input_transport_byte_loss_total.load(std::memory_order_relaxed);

    std::vector<SRTSOCKET> sockets_with_stats;
    sockets_with_stats.reserve(member_sockets.size());

    for (const SRTSOCKET member_sock : member_sockets) {
        SRT_TRACEBSTATS member_stats {};
        if (srt_bstats(member_sock, &member_stats, 0) == SRT_ERROR) {
            continue;
        }

        sockets_with_stats.push_back(member_sock);
        input_byte_recv_current += member_stats.byteRecvTotal;
        input_byte_recv_unique_current += member_stats.byteRecvUniqueTotal;
        input_byte_retrans_current += member_stats.byteRetransTotal;
        input_byte_loss_current += member_stats.byteRcvLossTotal;

        auto& prev = metrics->input_transport_last_by_socket[member_sock];
        input_byte_recv_total += CounterDeltaWithReset(prev.byte_recv_total, member_stats.byteRecvTotal);
        input_byte_recv_unique_total += CounterDeltaWithReset(prev.byte_recv_unique_total, member_stats.byteRecvUniqueTotal);
        input_byte_retrans_total += CounterDeltaWithReset(prev.byte_retrans_total, member_stats.byteRetransTotal);
        input_byte_loss_total += CounterDeltaWithReset(prev.byte_loss_total, member_stats.byteRcvLossTotal);
        prev.byte_recv_total = member_stats.byteRecvTotal;
        prev.byte_recv_unique_total = member_stats.byteRecvUniqueTotal;
        prev.byte_retrans_total = member_stats.byteRetransTotal;
        prev.byte_loss_total = member_stats.byteRcvLossTotal;
    }

    std::unordered_map<SRTSOCKET, TransportCounterSnapshot> next_last;
    for (const SRTSOCKET sock : sockets_with_stats) {
        next_last.emplace(sock, metrics->input_transport_last_by_socket[sock]);
    }
    metrics->input_transport_last_by_socket.swap(next_last);

    metrics->input_transport_members_total.store(static_cast<int64_t>(sockets_with_stats.size()), std::memory_order_relaxed);
    metrics->input_transport_byte_recv_total.store(input_byte_recv_total, std::memory_order_relaxed);
    metrics->input_transport_byte_recv_unique_total.store(input_byte_recv_unique_total, std::memory_order_relaxed);
    metrics->input_transport_byte_retrans_total.store(input_byte_retrans_total, std::memory_order_relaxed);
    metrics->input_transport_byte_loss_total.store(input_byte_loss_total, std::memory_order_relaxed);
    metrics->input_transport_byte_recv_current.store(input_byte_recv_current, std::memory_order_relaxed);
    metrics->input_transport_byte_recv_unique_current.store(input_byte_recv_unique_current, std::memory_order_relaxed);
    metrics->input_transport_byte_retrans_current.store(input_byte_retrans_current, std::memory_order_relaxed);
    metrics->input_transport_byte_loss_current.store(input_byte_loss_current, std::memory_order_relaxed);

    if (output_sock == SRT_INVALID_SOCK) {
        metrics->output_transport_last_socket = SRT_INVALID_SOCK;
        metrics->output_transport_byte_sent_current.store(0, std::memory_order_relaxed);
        metrics->output_transport_byte_sent_unique_current.store(0, std::memory_order_relaxed);
        metrics->output_transport_byte_retrans_current.store(0, std::memory_order_relaxed);
        metrics->output_transport_byte_drop_current.store(0, std::memory_order_relaxed);
        return;
    }

    SRT_TRACEBSTATS output_stats {};
    if (srt_bstats(output_sock, &output_stats, 0) == SRT_ERROR) {
        return;
    }

    uint64_t output_byte_sent_total = metrics->output_transport_byte_sent_total.load(std::memory_order_relaxed);
    uint64_t output_byte_sent_unique_total = metrics->output_transport_byte_sent_unique_total.load(std::memory_order_relaxed);
    uint64_t output_byte_retrans_total = metrics->output_transport_byte_retrans_total.load(std::memory_order_relaxed);
    uint64_t output_byte_drop_total = metrics->output_transport_byte_drop_total.load(std::memory_order_relaxed);

    if (metrics->output_transport_last_socket != output_sock) {
        metrics->output_transport_last_socket = output_sock;
        metrics->output_transport_last.byte_sent_total = output_stats.byteSentTotal;
        metrics->output_transport_last.byte_sent_unique_total = output_stats.byteSentUniqueTotal;
        metrics->output_transport_last.byte_retrans_total = output_stats.byteRetransTotal;
        metrics->output_transport_last.byte_drop_total = output_stats.byteSndDropTotal;
    } else {
        output_byte_sent_total += CounterDeltaWithReset(metrics->output_transport_last.byte_sent_total, output_stats.byteSentTotal);
        output_byte_sent_unique_total += CounterDeltaWithReset(metrics->output_transport_last.byte_sent_unique_total, output_stats.byteSentUniqueTotal);
        output_byte_retrans_total += CounterDeltaWithReset(metrics->output_transport_last.byte_retrans_total, output_stats.byteRetransTotal);
        output_byte_drop_total += CounterDeltaWithReset(metrics->output_transport_last.byte_drop_total, output_stats.byteSndDropTotal);
        metrics->output_transport_last.byte_sent_total = output_stats.byteSentTotal;
        metrics->output_transport_last.byte_sent_unique_total = output_stats.byteSentUniqueTotal;
        metrics->output_transport_last.byte_retrans_total = output_stats.byteRetransTotal;
        metrics->output_transport_last.byte_drop_total = output_stats.byteSndDropTotal;
    }

    metrics->output_transport_byte_sent_total.store(output_byte_sent_total, std::memory_order_relaxed);
    metrics->output_transport_byte_sent_unique_total.store(output_byte_sent_unique_total, std::memory_order_relaxed);
    metrics->output_transport_byte_retrans_total.store(output_byte_retrans_total, std::memory_order_relaxed);
    metrics->output_transport_byte_drop_total.store(output_byte_drop_total, std::memory_order_relaxed);
    metrics->output_transport_byte_sent_current.store(output_stats.byteSentTotal, std::memory_order_relaxed);
    metrics->output_transport_byte_sent_unique_current.store(output_stats.byteSentUniqueTotal, std::memory_order_relaxed);
    metrics->output_transport_byte_retrans_current.store(output_stats.byteRetransTotal, std::memory_order_relaxed);
    metrics->output_transport_byte_drop_current.store(output_stats.byteSndDropTotal, std::memory_order_relaxed);
}

std::string RenderPrometheusMetrics(const MetricsState& metrics) {
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
    int64_t input_links_snapshot_count = metrics.input_links_snapshot_count.load(std::memory_order_relaxed);
    if (input_links_snapshot_count < 0) {
        input_links_snapshot_count = 0;
    }
    const size_t input_links_snapshot_capped =
        std::min(static_cast<size_t>(input_links_snapshot_count), MetricsState::kMaxTrackedMembers);
    const auto input_transport_byte_recv_total = metrics.input_transport_byte_recv_total.load(std::memory_order_relaxed);
    const auto input_transport_byte_recv_unique_total = metrics.input_transport_byte_recv_unique_total.load(std::memory_order_relaxed);
    const auto input_transport_byte_retrans_total = metrics.input_transport_byte_retrans_total.load(std::memory_order_relaxed);
    const auto input_transport_byte_loss_total = metrics.input_transport_byte_loss_total.load(std::memory_order_relaxed);
    const auto input_transport_members_total = metrics.input_transport_members_total.load(std::memory_order_relaxed);
    const auto input_transport_byte_recv_current = metrics.input_transport_byte_recv_current.load(std::memory_order_relaxed);
    const auto input_transport_byte_recv_unique_current = metrics.input_transport_byte_recv_unique_current.load(std::memory_order_relaxed);
    const auto input_transport_byte_retrans_current = metrics.input_transport_byte_retrans_current.load(std::memory_order_relaxed);
    const auto input_transport_byte_loss_current = metrics.input_transport_byte_loss_current.load(std::memory_order_relaxed);
    const auto output_transport_byte_sent_total = metrics.output_transport_byte_sent_total.load(std::memory_order_relaxed);
    const auto output_transport_byte_sent_unique_total = metrics.output_transport_byte_sent_unique_total.load(std::memory_order_relaxed);
    const auto output_transport_byte_retrans_total = metrics.output_transport_byte_retrans_total.load(std::memory_order_relaxed);
    const auto output_transport_byte_drop_total = metrics.output_transport_byte_drop_total.load(std::memory_order_relaxed);
    const auto output_transport_byte_sent_current = metrics.output_transport_byte_sent_current.load(std::memory_order_relaxed);
    const auto output_transport_byte_sent_unique_current = metrics.output_transport_byte_sent_unique_current.load(std::memory_order_relaxed);
    const auto output_transport_byte_retrans_current = metrics.output_transport_byte_retrans_current.load(std::memory_order_relaxed);
    const auto output_transport_byte_drop_current = metrics.output_transport_byte_drop_current.load(std::memory_order_relaxed);
    const auto path_ready = (input_connected == 1 && output_connected == 1) ? 1 : 0;

    const auto input_rtt_ms = metrics.input_rtt_ms.load(std::memory_order_relaxed);
    const auto output_rtt_ms = metrics.output_rtt_ms.load(std::memory_order_relaxed);
    const auto last_rx_unix_seconds = metrics.last_rx_unix_ms.load(std::memory_order_relaxed) / 1000;
    const auto last_tx_unix_seconds = metrics.last_tx_unix_ms.load(std::memory_order_relaxed) / 1000;

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

    emit_u64("srt_relay_rx_bytes_total", "counter", "Total bytes received by relay input.", total_rx_bytes);
    emit_u64("srt_relay_tx_bytes_total", "counter", "Total bytes sent by relay output.", total_tx_bytes);
    emit_u64("srt_relay_rx_messages_total", "counter", "Total messages received by relay input.", total_rx_msgs);
    emit_u64("srt_relay_tx_messages_total", "counter", "Total messages sent by relay output.", total_tx_msgs);
    emit_u64("srt_relay_send_failures_total", "counter", "Total failed output send attempts.", total_send_failures);
    emit_u64("srt_relay_reconnects_total", "counter", "Total reconnect attempts after output failures.", reconnect_count);

    emit_u64("srt_relay_rx_bytes_per_sec", "gauge", "Received bytes per second over last stats interval.", rx_bytes_per_sec);
    emit_u64("srt_relay_tx_bytes_per_sec", "gauge", "Sent bytes per second over last stats interval.", tx_bytes_per_sec);
    emit_u64("srt_relay_rx_messages_per_sec", "gauge", "Received messages per second over last stats interval.", rx_msgs_per_sec);
    emit_u64("srt_relay_tx_messages_per_sec", "gauge", "Sent messages per second over last stats interval.", tx_msgs_per_sec);
    emit_u64("srt_relay_send_failures_interval", "gauge", "Output send failures over last stats interval.", interval_send_failures);

    emit_i64("srt_relay_input_listening", "gauge", "Relay input listener is up (1/0).", input_listening);
    emit_i64("srt_relay_input_connected", "gauge", "Relay input session is connected (1/0).", input_connected);
    emit_i64("srt_relay_output_connected", "gauge", "Relay output session is connected (1/0).", output_connected);
    emit_i64("srt_relay_path_ready", "gauge", "Relay data path input+output connected (1/0).", path_ready);
    emit_i64("srt_relay_input_links_total", "gauge", "Total number of input links in the active SRT group.", input_links_total);
    emit_i64("srt_relay_input_links_healthy", "gauge", "Number of healthy input links in the active SRT group.", input_links_healthy);
    emit_i64("srt_relay_input_links_running", "gauge", "Number of currently running/active input links in the active SRT group.", input_links_running);
    out << "# HELP srt_relay_input_link_connected Per-input-link connection state in the active SRT group (1/0).\n";
    out << "# TYPE srt_relay_input_link_connected gauge\n";
    for (size_t i = 0; i < input_links_snapshot_capped; ++i) {
        const auto socket_id = static_cast<SRTSOCKET>(metrics.input_member_ids[i].load(std::memory_order_relaxed));
        if (socket_id == SRT_INVALID_SOCK) {
            continue;
        }
        const auto connected = metrics.input_member_connected[i].load(std::memory_order_relaxed);
        out << "srt_relay_input_link_connected{link_index=\"" << (i + 1)
            << "\",socket_id=\"" << socket_id << "\"} "
            << connected << "\n";
    }

    emit_u64("srt_relay_input_transport_byte_recv_total", "counter", "Monotonic input transport bytes received across tracked SRT member sockets (includes duplicate traffic).", input_transport_byte_recv_total);
    emit_u64("srt_relay_input_transport_byte_recv_unique_total", "counter", "Monotonic input transport unique bytes received across tracked SRT member sockets.", input_transport_byte_recv_unique_total);
    emit_u64("srt_relay_input_transport_byte_retrans_total", "counter", "Monotonic input transport retransmitted bytes across tracked SRT member sockets.", input_transport_byte_retrans_total);
    emit_u64("srt_relay_input_transport_byte_loss_total", "counter", "Monotonic input transport reported lost bytes across tracked SRT member sockets.", input_transport_byte_loss_total);
    emit_i64("srt_relay_input_transport_members_tracked", "gauge", "Number of input SRT member sockets tracked for transport metrics.", input_transport_members_total);
    emit_u64("srt_relay_input_transport_byte_recv_current", "gauge", "Current summed byteRecvTotal across tracked SRT member sockets.", input_transport_byte_recv_current);
    emit_u64("srt_relay_input_transport_byte_recv_unique_current", "gauge", "Current summed byteRecvUniqueTotal across tracked SRT member sockets.", input_transport_byte_recv_unique_current);
    emit_u64("srt_relay_input_transport_byte_retrans_current", "gauge", "Current summed byteRetransTotal across tracked SRT member sockets.", input_transport_byte_retrans_current);
    emit_u64("srt_relay_input_transport_byte_loss_current", "gauge", "Current summed byteRcvLossTotal across tracked SRT member sockets.", input_transport_byte_loss_current);

    emit_u64("srt_relay_output_transport_byte_sent_total", "counter", "Monotonic output transport bytes sent on relay output SRT socket (includes retransmissions).", output_transport_byte_sent_total);
    emit_u64("srt_relay_output_transport_byte_sent_unique_total", "counter", "Monotonic output transport unique bytes sent on relay output SRT socket.", output_transport_byte_sent_unique_total);
    emit_u64("srt_relay_output_transport_byte_retrans_total", "counter", "Monotonic output transport retransmitted bytes on relay output SRT socket.", output_transport_byte_retrans_total);
    emit_u64("srt_relay_output_transport_byte_drop_total", "counter", "Monotonic output transport dropped bytes on relay output SRT socket.", output_transport_byte_drop_total);
    emit_u64("srt_relay_output_transport_byte_sent_current", "gauge", "Current byteSentTotal on relay output SRT socket.", output_transport_byte_sent_current);
    emit_u64("srt_relay_output_transport_byte_sent_unique_current", "gauge", "Current byteSentUniqueTotal on relay output SRT socket.", output_transport_byte_sent_unique_current);
    emit_u64("srt_relay_output_transport_byte_retrans_current", "gauge", "Current byteRetransTotal on relay output SRT socket.", output_transport_byte_retrans_current);
    emit_u64("srt_relay_output_transport_byte_drop_current", "gauge", "Current byteSndDropTotal on relay output SRT socket.", output_transport_byte_drop_current);

    emit_i64("srt_relay_input_rtt_ms", "gauge", "Input socket RTT in milliseconds.", input_rtt_ms);
    emit_i64("srt_relay_output_rtt_ms", "gauge", "Output socket RTT in milliseconds.", output_rtt_ms);
    emit_i64("srt_relay_last_rx_unix_seconds", "gauge", "Unix timestamp of last received packet.", last_rx_unix_seconds);
    emit_i64("srt_relay_last_tx_unix_seconds", "gauge", "Unix timestamp of last forwarded packet.", last_tx_unix_seconds);

    return out.str();
}

class MetricsServer {
public:
    MetricsServer(const Config& cfg, const Logger& logger, const MetricsState& metrics)
        : cfg_(cfg), logger_(logger), metrics_(metrics) {}

    ~MetricsServer() { Stop(); }

    void Start() {
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

    void Stop() {
        if (!cfg_.metrics_enabled) {
            return;
        }
        server_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    const Config& cfg_;
    const Logger& logger_;
    const MetricsState& metrics_;
    httplib::Server server_;
    std::thread thread_;
};

bool VerifyLinkage(const Logger& logger) {
    void* startup_symbol = dlsym(RTLD_DEFAULT, "srt_startup");
    if (startup_symbol == nullptr) {
        logger.Log(LogLevel::kError, "verify-linkage-failed", "reason=dlsym-srt_startup-failed");
        return false;
    }

    Dl_info info {};
    if (dladdr(startup_symbol, &info) == 0 || info.dli_fname == nullptr) {
        logger.Log(LogLevel::kError, "verify-linkage-failed", "reason=dladdr-failed");
        return false;
    }

    const std::string libsrt_path = info.dli_fname;
    const uint32_t version = srt_getversion();
    logger.Log(LogLevel::kInfo, "verify-linkage",
               "libsrt_path=" + libsrt_path,
               "srt_version=" + std::to_string(version));

    const bool using_custom_prefix = libsrt_path.find("/opt/pixop-srt/") != std::string::npos;
    if (!using_custom_prefix) {
        logger.Log(LogLevel::kError, "verify-linkage-failed",
                   "reason=libsrt-not-from-custom-prefix",
                   "expected_prefix=/opt/pixop-srt");
        return false;
    }

    const SRTSOCKET probe_group = srt_create_group(SRT_GTYPE_BROADCAST);
    if (probe_group == SRT_INVALID_SOCK) {
        logger.Log(LogLevel::kError, "verify-linkage-failed",
                   "reason=bonding-api-unavailable",
                   "srt_error=" + SrtLastErrorString());
        return false;
    }
    srt_close(probe_group);
    logger.Log(LogLevel::kInfo, "verify-linkage-ok", "bonding_api=enabled");
    return true;
}

void MaybeLogStats(const Config& cfg,
                   const Logger& logger,
                   RelayStats* stats,
                   const RelayState& state,
                   MetricsState* metrics,
                   SRTSOCKET input_session_sock,
                   SRTSOCKET output_sock,
                   std::chrono::steady_clock::time_point* last_stats_at) {
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - *last_stats_at).count();
    if (elapsed_ms < cfg.stats_interval_ms) {
        return;
    }
    const double sec = static_cast<double>(elapsed_ms) / 1000.0;
    const auto rx_bps = static_cast<uint64_t>(stats->interval_rx_bytes / sec);
    const auto tx_bps = static_cast<uint64_t>(stats->interval_tx_bytes / sec);
    const auto rx_mps = static_cast<uint64_t>(stats->interval_rx_msgs / sec);
    const auto tx_mps = static_cast<uint64_t>(stats->interval_tx_msgs / sec);
    MaybeUpdateRttMetric(input_session_sock, &metrics->input_rtt_ms);
    MaybeUpdateRttMetric(output_sock, &metrics->output_rtt_ms);
    UpdateInputLinkHealthMetrics(input_session_sock, metrics);
    UpdateTransportTrafficMetrics(output_sock, metrics);
    const auto input_rtt_ms = metrics->input_rtt_ms.load(std::memory_order_relaxed);
    const auto output_rtt_ms = metrics->output_rtt_ms.load(std::memory_order_relaxed);
    const auto input_links_total = metrics->input_links_total.load(std::memory_order_relaxed);
    const auto input_links_healthy = metrics->input_links_healthy.load(std::memory_order_relaxed);
    const auto input_links_running = metrics->input_links_running.load(std::memory_order_relaxed);
    const auto input_transport_byte_recv_total = metrics->input_transport_byte_recv_total.load(std::memory_order_relaxed);
    const auto output_transport_byte_sent_total = metrics->output_transport_byte_sent_total.load(std::memory_order_relaxed);
    const auto input_link_status = BuildInputLinkStatusCompact(*metrics);

    metrics->rx_bytes_per_sec.store(rx_bps, std::memory_order_relaxed);
    metrics->tx_bytes_per_sec.store(tx_bps, std::memory_order_relaxed);
    metrics->rx_msgs_per_sec.store(rx_mps, std::memory_order_relaxed);
    metrics->tx_msgs_per_sec.store(tx_mps, std::memory_order_relaxed);
    metrics->interval_send_failures.store(stats->interval_send_failures, std::memory_order_relaxed);

    logger.Log(LogLevel::kInfo,
               "stats",
               "rx_bytes_per_sec=" + std::to_string(rx_bps),
               "tx_bytes_per_sec=" + std::to_string(tx_bps),
               "rx_msgs_per_sec=" + std::to_string(rx_mps),
               "tx_msgs_per_sec=" + std::to_string(tx_mps),
               "send_failures=" + std::to_string(stats->interval_send_failures),
               "reconnect_count=" + std::to_string(stats->reconnect_count),
               "input_rtt_ms=" + std::to_string(input_rtt_ms),
               "output_rtt_ms=" + std::to_string(output_rtt_ms),
               "input_links_total=" + std::to_string(input_links_total),
               "input_links_healthy=" + std::to_string(input_links_healthy),
               "input_links_running=" + std::to_string(input_links_running),
               "input_link_status=" + input_link_status,
               "input_transport_byte_recv_total=" + std::to_string(input_transport_byte_recv_total),
               "output_transport_byte_sent_total=" + std::to_string(output_transport_byte_sent_total),
               std::string("input_state=") + (state.input_connected ? "connected" : (state.input_listening ? "listening" : "down")),
               std::string("output_state=") + (state.output_connected ? "connected" : "down"));

    stats->interval_rx_bytes = 0;
    stats->interval_tx_bytes = 0;
    stats->interval_rx_msgs = 0;
    stats->interval_tx_msgs = 0;
    stats->interval_send_failures = 0;
    *last_stats_at = now;
}

SRTSOCKET CreateListeningInput(const SrtUri& input, const Config& cfg, const Logger& logger) {
    SRTSOCKET listener = srt_create_socket();
    if (listener == SRT_INVALID_SOCK) {
        throw std::runtime_error("failed to create input listener socket: " + SrtLastErrorString());
    }

    try {
        const int one = 1;
        ApplyIntSockOpt(listener, SRTO_REUSEADDR, one, "SRTO_REUSEADDR");
        ApplyIntSockOpt(listener, SRTO_GROUPCONNECT, one, "SRTO_GROUPCONNECT");
        ApplyIntSockOpt(listener, SRTO_RCVTIMEO, cfg.io_timeout_ms, "SRTO_RCVTIMEO");
        ApplyCommonSrtOptions(listener, input, logger);

        socklen_t addr_len = 0;
        sockaddr_storage addr = ResolveSockaddr(input.host, input.port, &addr_len);
        if (srt_bind(listener, reinterpret_cast<sockaddr*>(&addr), addr_len) == SRT_ERROR) {
            throw std::runtime_error("input bind failed: " + SrtLastErrorString());
        }
        if (srt_listen(listener, 8) == SRT_ERROR) {
            throw std::runtime_error("input listen failed: " + SrtLastErrorString());
        }
    } catch (...) {
        srt_close(listener);
        throw;
    }

    logger.Log(LogLevel::kInfo, "input-listening",
               "uri_host=" + input.host,
               "uri_port=" + std::to_string(input.port));
    return listener;
}

SRTSOCKET AcceptInputSession(SRTSOCKET listener, const Config& cfg, const Logger& logger) {
    // Timeout-based accept keeps Ctrl+C responsive without epoll/asynchronous loops.
    const SRTSOCKET listeners[] = {listener};
    SRTSOCKET accepted = srt_accept_bond(listeners, 1, static_cast<int64_t>(cfg.io_timeout_ms));
    if (accepted == SRT_INVALID_SOCK) {
        throw std::runtime_error("input accept failed: " + SrtLastErrorString());
    }
    ApplyIntSockOpt(accepted, SRTO_RCVTIMEO, cfg.io_timeout_ms, "SRTO_RCVTIMEO");
    logger.Log(LogLevel::kInfo, "input-connected", "socket=" + std::to_string(accepted));
    return accepted;
}

SRTSOCKET ConnectOutput(const SrtUri& output, const Config& cfg, const Logger& logger) {
    SRTSOCKET sock = srt_create_socket();
    if (sock == SRT_INVALID_SOCK) {
        throw std::runtime_error("failed to create output socket: " + SrtLastErrorString());
    }

    try {
        ApplyIntSockOpt(sock, SRTO_CONNTIMEO, cfg.io_timeout_ms, "SRTO_CONNTIMEO");
        ApplyIntSockOpt(sock, SRTO_SNDTIMEO, cfg.io_timeout_ms, "SRTO_SNDTIMEO");
        ApplyCommonSrtOptions(sock, output, logger);
        socklen_t addr_len = 0;
        sockaddr_storage addr = ResolveSockaddr(output.host, output.port, &addr_len);
        if (srt_connect(sock, reinterpret_cast<sockaddr*>(&addr), addr_len) == SRT_ERROR) {
            throw std::runtime_error("output connect failed: " + SrtLastErrorString());
        }
    } catch (...) {
        srt_close(sock);
        throw;
    }

    logger.Log(LogLevel::kInfo, "output-connected",
               "uri_host=" + output.host,
               "uri_port=" + std::to_string(output.port));
    return sock;
}

void SleepReconnectDelay(const Config& cfg) {
    const auto delay = std::chrono::milliseconds(cfg.reconnect_delay_ms);
    const auto chunk = std::chrono::milliseconds(100);
    std::chrono::milliseconds slept(0);
    while (slept < delay && !g_shutdown_requested.load()) {
        std::this_thread::sleep_for(std::min(chunk, delay - slept));
        slept += std::min(chunk, delay - slept);
    }
}

int RelayMain(const Config& cfg, const Logger& logger) {
    const SrtUri input_uri = ParseSrtUri(cfg.input_uri);
    const SrtUri output_uri = ParseSrtUri(cfg.output_uri);

    const std::string input_mode = QueryString(input_uri.query, "mode");
    if (!input_mode.empty() && input_mode != "listener") {
        throw std::runtime_error("input URI mode must be listener for this relay");
    }
    const std::string output_mode = QueryString(output_uri.query, "mode");
    if (!output_mode.empty() && output_mode != "caller") {
        throw std::runtime_error("output URI mode must be caller for this relay");
    }

    RelayStats stats;
    RelayState state;
    MetricsState metrics;
    std::vector<char> buffer(static_cast<size_t>(cfg.max_message_size));
    auto last_stats_at = std::chrono::steady_clock::now();
    const auto startup_ms = UnixNowMs();
    metrics.last_rx_unix_ms.store(startup_ms, std::memory_order_relaxed);
    metrics.last_tx_unix_ms.store(startup_ms, std::memory_order_relaxed);

    SrtSocketHolder input_listener;
    SrtSocketHolder input_session;
    SrtSocketHolder output_socket;
    MetricsServer metrics_server(cfg, logger, metrics);
    metrics_server.Start();

    logger.Log(LogLevel::kInfo, "startup",
               "input_uri=" + cfg.input_uri,
               "output_uri=" + cfg.output_uri,
               "stats_interval_ms=" + std::to_string(cfg.stats_interval_ms),
               "reconnect_delay_ms=" + std::to_string(cfg.reconnect_delay_ms),
               "max_message_size=" + std::to_string(cfg.max_message_size),
               "io_timeout_ms=" + std::to_string(cfg.io_timeout_ms),
               "metrics_enabled=" + std::string(cfg.metrics_enabled ? "true" : "false"),
               "metrics_host=" + cfg.metrics_host,
               "metrics_port=" + std::to_string(cfg.metrics_port));

    while (!g_shutdown_requested.load()) {
        MaybeLogStats(cfg, logger, &stats, state, &metrics, input_session.Get(), output_socket.Get(), &last_stats_at);

        if (!input_listener.Valid()) {
            try {
                input_listener.Set(CreateListeningInput(input_uri, cfg, logger));
                state.input_listening = true;
                metrics.input_listening.store(1, std::memory_order_relaxed);
            } catch (const std::exception& ex) {
                logger.Log(LogLevel::kError, "input-listen-failed", "error=" + std::string(ex.what()));
                if (cfg.exit_on_input_failure) return 2;
                SleepReconnectDelay(cfg);
                continue;
            }
        }

        if (!input_session.Valid()) {
            try {
                input_session.Set(AcceptInputSession(input_listener.Get(), cfg, logger));
                state.input_connected = true;
                metrics.input_connected.store(1, std::memory_order_relaxed);
            } catch (const std::exception& ex) {
                if (IsSrtTimeoutError()) {
                    MaybeLogStats(cfg, logger, &stats, state, &metrics, input_session.Get(), output_socket.Get(), &last_stats_at);
                    continue;
                }
                logger.Log(LogLevel::kWarn, "input-accept-failed", "error=" + std::string(ex.what()));
                state.input_connected = false;
                metrics.input_connected.store(0, std::memory_order_relaxed);
                metrics.input_links_total.store(0, std::memory_order_relaxed);
                metrics.input_links_healthy.store(0, std::memory_order_relaxed);
                metrics.input_links_running.store(0, std::memory_order_relaxed);
                MarkAllTrackedInputLinksDisconnected(&metrics);
                metrics.input_transport_members_total.store(0, std::memory_order_relaxed);
                input_session.Reset();
                input_listener.Reset();
                state.input_listening = false;
                metrics.input_listening.store(0, std::memory_order_relaxed);
                if (cfg.exit_on_input_failure) return 2;
                SleepReconnectDelay(cfg);
                continue;
            }
        }

        if (!output_socket.Valid()) {
            try {
                output_socket.Set(ConnectOutput(output_uri, cfg, logger));
                state.output_connected = true;
                metrics.output_connected.store(1, std::memory_order_relaxed);
            } catch (const std::exception& ex) {
                stats.reconnect_count++;
                metrics.reconnect_count.store(stats.reconnect_count, std::memory_order_relaxed);
                logger.Log(LogLevel::kWarn, "output-connect-failed",
                           "error=" + std::string(ex.what()),
                           "reconnect_count=" + std::to_string(stats.reconnect_count));
                state.output_connected = false;
                metrics.output_connected.store(0, std::memory_order_relaxed);
                output_socket.Reset();
                if (cfg.exit_on_output_failure) return 3;
                SleepReconnectDelay(cfg);
                continue;
            }
        }

        SRT_MSGCTRL rx_ctrl = srt_msgctrl_default;
        SRT_SOCKGROUPDATA rx_group_data[16] {};
        rx_ctrl.grpdata = rx_group_data;
        rx_ctrl.grpdata_size = sizeof(rx_group_data) / sizeof(rx_group_data[0]);
        const int recv_size = srt_recvmsg2(input_session.Get(), buffer.data(), cfg.max_message_size, &rx_ctrl);
        if (recv_size == SRT_ERROR) {
            if (IsSrtTimeoutError()) {
                MaybeLogStats(cfg, logger, &stats, state, &metrics, input_session.Get(), output_socket.Get(), &last_stats_at);
                continue;
            }
            logger.Log(LogLevel::kWarn, "input-disconnected", "error=" + SrtLastErrorString());
            state.input_connected = false;
            metrics.input_connected.store(0, std::memory_order_relaxed);
            metrics.input_links_total.store(0, std::memory_order_relaxed);
            metrics.input_links_healthy.store(0, std::memory_order_relaxed);
            metrics.input_links_running.store(0, std::memory_order_relaxed);
            MarkAllTrackedInputLinksDisconnected(&metrics);
            metrics.input_transport_members_total.store(0, std::memory_order_relaxed);
            input_session.Reset();
            input_listener.Reset();
            state.input_listening = false;
            metrics.input_listening.store(0, std::memory_order_relaxed);
            if (cfg.exit_on_input_failure) return 2;
            continue;
        }

        stats.total_rx_bytes += static_cast<uint64_t>(recv_size);
        stats.total_rx_msgs += 1;
        stats.interval_rx_bytes += static_cast<uint64_t>(recv_size);
        stats.interval_rx_msgs += 1;
        UpdateInputLinkHealthFromMsgCtrl(rx_ctrl, &metrics);
        metrics.total_rx_bytes.store(stats.total_rx_bytes, std::memory_order_relaxed);
        metrics.total_rx_msgs.store(stats.total_rx_msgs, std::memory_order_relaxed);
        metrics.last_rx_unix_ms.store(UnixNowMs(), std::memory_order_relaxed);

        SRT_MSGCTRL tx_ctrl = srt_msgctrl_default;
        // Do not force srctime pass-through. For some sender/input combinations
        // libsrt rejects forwarded timestamps as invalid for this output socket.
        // Leaving srctime at default lets libsrt derive timing safely.
        const int sent_size = srt_sendmsg2(output_socket.Get(), buffer.data(), recv_size, &tx_ctrl);
        if (sent_size == SRT_ERROR) {
            stats.total_send_failures += 1;
            stats.interval_send_failures += 1;
            stats.reconnect_count += 1;
            metrics.total_send_failures.store(stats.total_send_failures, std::memory_order_relaxed);
            metrics.reconnect_count.store(stats.reconnect_count, std::memory_order_relaxed);
            logger.Log(LogLevel::kWarn, "output-send-failed",
                       "error=" + SrtLastErrorString(),
                       "reconnect_count=" + std::to_string(stats.reconnect_count));
            output_socket.Reset();
            state.output_connected = false;
            metrics.output_connected.store(0, std::memory_order_relaxed);
            if (cfg.exit_on_output_failure) return 3;
            SleepReconnectDelay(cfg);
            continue;
        }

        stats.total_tx_bytes += static_cast<uint64_t>(sent_size);
        stats.total_tx_msgs += 1;
        stats.interval_tx_bytes += static_cast<uint64_t>(sent_size);
        stats.interval_tx_msgs += 1;
        metrics.total_tx_bytes.store(stats.total_tx_bytes, std::memory_order_relaxed);
        metrics.total_tx_msgs.store(stats.total_tx_msgs, std::memory_order_relaxed);
        metrics.last_tx_unix_ms.store(UnixNowMs(), std::memory_order_relaxed);
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

int main(int argc, char** argv) {
    Config cfg;
    try {
        cfg = ParseArgs(argc, argv);
    } catch (const std::exception& ex) {
        std::cerr << "argument error: " << ex.what() << std::endl;
        PrintUsage();
        return 1;
    }

    Logger logger;
    logger.min_level = cfg.log_level;

    struct sigaction sa {};
    sa.sa_handler = OnSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  // Keep syscalls interruptible instead of auto-restarting.
    if (sigaction(SIGINT, &sa, nullptr) == -1 || sigaction(SIGTERM, &sa, nullptr) == -1) {
        std::cerr << "failed to register signal handlers" << std::endl;
        return 1;
    }

    if (srt_startup() == SRT_ERROR) {
        std::cerr << "srt_startup failed: " << SrtLastErrorString() << std::endl;
        return 1;
    }

    const int exit_code = [&]() {
        if (cfg.verify_linkage) {
            return VerifyLinkage(logger) ? 0 : 1;
        }

        try {
            return RelayMain(cfg, logger);
        } catch (const std::exception& ex) {
            logger.Log(LogLevel::kError, "fatal", "error=" + std::string(ex.what()));
            return 1;
        }
    }();

    srt_cleanup();
    return exit_code;
}
