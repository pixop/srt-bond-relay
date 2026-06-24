#pragma once

#include <vector>

#include "srtrelay/config.hpp"
#include "srtrelay/logger.hpp"
#include "srtrelay/srt_utils.hpp"

namespace srtrelay::detail {

void CloseSocketList(std::vector<SRTSOCKET>* sockets);
SRTSOCKET CreateListeningSocket(const SrtUri& uri, const Config& cfg, const Logger& logger, const char* endpoint_name);
SRTSOCKET AcceptBondSession(const std::vector<SRTSOCKET>& listeners, const Config& cfg, const char* endpoint_name);
SRTSOCKET ConnectSingleCallerSocket(const SrtUri& uri, const Config& cfg, const Logger& logger, bool for_output);
SRTSOCKET ConnectBondedCallerGroup(const std::vector<SrtUri>& uris,
                                   SRT_GROUP_TYPE group_type,
                                   const Config& cfg,
                                   const Logger& logger,
                                   bool for_output);

}  // namespace srtrelay::detail
