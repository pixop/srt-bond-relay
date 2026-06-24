#include "srtrelay/linkage.hpp"

#include <dlfcn.h>
#include <srt.h>

#include <string>

#include "srtrelay/srt_utils.hpp"

namespace srtrelay {

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

}  // namespace srtrelay
