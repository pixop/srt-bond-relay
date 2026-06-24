#include <signal.h>
#include <unistd.h>

#include <iostream>
#include <stdexcept>
#include <string>

#include <srt.h>

#include "srtrelay/config.hpp"
#include "srtrelay/linkage.hpp"
#include "srtrelay/logger.hpp"
#include "srtrelay/relay.hpp"
#include "srtrelay/srt_utils.hpp"

int main(int argc, char** argv) {
    srtrelay::Config cfg;
    try {
        cfg = srtrelay::ParseArgs(argc, argv);
    } catch (const std::exception& ex) {
        std::cerr << "argument error: " << ex.what() << std::endl;
        srtrelay::PrintUsage();
        return 1;
    }

    srtrelay::Logger logger;
    logger.min_level = cfg.log_level;

    struct sigaction sa {};
    sa.sa_handler = srtrelay::OnSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, nullptr) == -1 || sigaction(SIGTERM, &sa, nullptr) == -1) {
        std::cerr << "failed to register signal handlers" << std::endl;
        return 1;
    }
    // Writing binary payload to stdout should not terminate process on a closed pipe.
    signal(SIGPIPE, SIG_IGN);

    if (srt_startup() == SRT_ERROR) {
        std::cerr << "srt_startup failed: " << srtrelay::SrtLastErrorString() << std::endl;
        return 1;
    }

    const int exit_code = [&]() {
        if (cfg.verify_linkage) {
            return srtrelay::VerifyLinkage(logger) ? 0 : 1;
        }
        try {
            return srtrelay::RunRelay(cfg, logger);
        } catch (const std::exception& ex) {
            logger.Log(srtrelay::LogLevel::kError, "fatal", "error=" + std::string(ex.what()));
            return 1;
        }
    }();

    srt_cleanup();
    return exit_code;
}
