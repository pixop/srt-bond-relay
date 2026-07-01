#pragma once

#include <cstdlib>
#include <iostream>
#include <string>

namespace srtrelay::test {

inline void ExpectContains(const std::string& haystack, const std::string& needle) {
    if (haystack.find(needle) == std::string::npos) {
        std::cerr << "missing expected snippet: " << needle << "\n";
        std::abort();
    }
}

inline void ExpectNotContains(const std::string& haystack, const std::string& needle) {
    if (haystack.find(needle) != std::string::npos) {
        std::cerr << "unexpected snippet present: " << needle << "\n";
        std::abort();
    }
}

template <typename Fn>
inline void ExpectThrows(Fn&& fn) {
    bool threw = false;
    try {
        fn();
    } catch (...) {
        threw = true;
    }
    if (!threw) {
        std::cerr << "expected exception was not thrown\n";
        std::abort();
    }
}

}  // namespace srtrelay::test
