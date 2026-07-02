#include "srtrelay/srt_utils.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#include <cstring>
#include <stdexcept>

namespace srtrelay {

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

template <typename UriT>
UriT ParseUriWithPrefix(const std::string& uri, const std::string& prefix, const std::string& scheme_name) {
    if (uri.rfind(prefix, 0) != 0) {
        throw std::runtime_error("only " + scheme_name + ":// URIs are supported: " + uri);
    }

    const std::string rest = uri.substr(prefix.size());
    const size_t qpos = rest.find('?');
    const std::string authority = rest.substr(0, qpos);
    const std::string query = (qpos == std::string::npos) ? "" : rest.substr(qpos + 1);

    const size_t colon = authority.rfind(':');
    if (colon == std::string::npos) {
        throw std::runtime_error("SRT URI must include host:port: " + uri);
    }
    UriT parsed;
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

SrtUri ParseSrtUri(const std::string& uri) {
    return ParseUriWithPrefix<SrtUri>(uri, "srt://", "srt");
}

UdpUri ParseUdpUri(const std::string& uri) {
    return ParseUriWithPrefix<UdpUri>(uri, "udp://", "udp");
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
    for (const auto& [key, value] : uri.query) {
        if (key == "mode" ||
            key == "fanout" ||
            key == "max_clients" ||
            key == "fanout_max_clients" ||
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

SrtSocketHolder::~SrtSocketHolder() { Reset(); }
SrtSocketHolder::SrtSocketHolder() = default;

void SrtSocketHolder::Set(SRTSOCKET s) {
    Reset();
    sock_ = s;
}

void SrtSocketHolder::Reset() {
    if (sock_ != SRT_INVALID_SOCK) {
        srt_close(sock_);
        sock_ = SRT_INVALID_SOCK;
    }
}

SRTSOCKET SrtSocketHolder::Get() const { return sock_; }
bool SrtSocketHolder::Valid() const { return sock_ != SRT_INVALID_SOCK; }

}  // namespace srtrelay
