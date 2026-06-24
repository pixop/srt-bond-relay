#pragma once

#include "srtrelay/config.hpp"
#include "srtrelay/logger.hpp"

namespace srtrelay {

void OnSignal(int sig);
int RunRelay(const Config& cfg, const Logger& logger);

}  // namespace srtrelay
