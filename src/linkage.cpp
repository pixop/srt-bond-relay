#include "srtrelay/linkage.hpp"

#include <dlfcn.h>
#include <srt.h>

#include <string>

#include "srtrelay/srt_utils.hpp"

namespace srtrelay {

bool VerifyLinkage(const Logger& logger) {
#ifndef SRT_LINKAGE_MODE
#define SRT_LINKAGE_MODE "dynamic"
#endif

    void* startup_symbol = reinterpret_cast<void*>(&srt_startup);

    Dl_info info {};
    if (dladdr(startup_symbol, &info) == 0 || info.dli_fname == nullptr) {
        logger.Log(LogLevel::kError, "verify-linkage-failed", "reason=dladdr-failed");
        return false;
    }

    const std::string symbol_owner_path = info.dli_fname;
    const uint32_t version = srt_getversion();
    logger.Log(LogLevel::kInfo, "verify-linkage",
               "srt_linkage_mode=" + std::string(SRT_LINKAGE_MODE),
               "srt_symbol_owner=" + symbol_owner_path,
               "srt_version=" + std::to_string(version));

    const bool owner_is_shared_srt = symbol_owner_path.find("libsrt.so") != std::string::npos;
    if (std::string(SRT_LINKAGE_MODE) == "dynamic") {
        const bool using_custom_prefix = symbol_owner_path.find("/opt/pixop-srt/") != std::string::npos;
        if (!using_custom_prefix) {
            logger.Log(LogLevel::kError, "verify-linkage-failed",
                       "reason=libsrt-not-from-custom-prefix",
                       "expected_prefix=/opt/pixop-srt");
            return false;
        }
    } else if (std::string(SRT_LINKAGE_MODE) == "static") {
        if (owner_is_shared_srt) {
            logger.Log(LogLevel::kError, "verify-linkage-failed",
                       "reason=expected-static-srt-but-shared-detected",
                       "detected_symbol_owner=" + symbol_owner_path);
            return false;
        }
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

}  // namespace srtrelay
