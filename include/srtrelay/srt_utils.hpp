#pragma once

#include <initializer_list>
#include <map>
#include <string>

#include <srt.h>

#include "srtrelay/logger.hpp"

namespace srtrelay {

std::string SrtLastErrorString();
int SrtLastErrorCode();
bool IsSrtTimeoutError();

struct SrtUri {
    std::string host;
    int port = 0;
    std::map<std::string, std::string> query;
};

std::string PercentDecode(std::string value);
SrtUri ParseSrtUri(const std::string& uri);
std::string QueryString(const std::map<std::string, std::string>& query, const std::string& key);
int ParseIntOptionValue(const std::string& value, const char* opt_name);

void ApplyIntSockOpt(SRTSOCKET sock, SRT_SOCKOPT opt, int value, const char* opt_name);
void ApplyStringSockOpt(SRTSOCKET sock, SRT_SOCKOPT opt, const std::string& value, const char* opt_name);
void ApplyLingerSockOpt(SRTSOCKET sock, int seconds);
void ApplyCommonSrtOptions(SRTSOCKET sock, const SrtUri& uri, const Logger& logger);

sockaddr_storage ResolveSockaddr(const std::string& host, int port, socklen_t* out_len);

class SrtSocketHolder {
public:
    ~SrtSocketHolder();
    SrtSocketHolder();
    SrtSocketHolder(const SrtSocketHolder&) = delete;
    SrtSocketHolder& operator=(const SrtSocketHolder&) = delete;

    void Set(SRTSOCKET s);
    void Reset();

    SRTSOCKET Get() const;
    bool Valid() const;

private:
    SRTSOCKET sock_ = SRT_INVALID_SOCK;
};

}  // namespace srtrelay
