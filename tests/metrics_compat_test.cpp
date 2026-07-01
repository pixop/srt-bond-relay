#include <cassert>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "srtrelay/config.hpp"
#include "srtrelay/metrics.hpp"
#include "srtrelay/relay_io.hpp"
#include "metrics_link_slots.hpp"

namespace {

void ExpectContains(const std::string& haystack, const std::string& needle) {
    if (haystack.find(needle) == std::string::npos) {
        std::cerr << "missing expected snippet: " << needle << "\n";
        std::abort();
    }
}

void ExpectThrows(const std::function<void()>& fn) {
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

srtrelay::Config ParseConfig(std::vector<std::string> args) {
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (auto& arg : args) {
        argv.push_back(arg.data());
    }
    return srtrelay::ParseArgs(static_cast<int>(argv.size()), argv.data());
}

void TestPrometheusCompatibility() {
    srtrelay::MetricsState metrics;
    metrics.total_rx_bytes.store(111, std::memory_order_relaxed);
    metrics.total_tx_bytes.store(222, std::memory_order_relaxed);
    metrics.input_connected.store(1, std::memory_order_relaxed);
    metrics.output_connected.store(1, std::memory_order_relaxed);
    metrics.input_sources_total.store(2, std::memory_order_relaxed);
    metrics.output_sources_total.store(2, std::memory_order_relaxed);
    metrics.output_source_connected[0].store(1, std::memory_order_relaxed);
    metrics.output_source_connected[1].store(0, std::memory_order_relaxed);

    metrics.input_tracked.slots[0].member_identity_key = 1234;
    metrics.input_tracked.slots[0].member_id = 5001;
    metrics.input_tracked.slots[0].member_connected = 1;
    metrics.input_tracked.slots[0].link_rx_bytes_total = 999;
    metrics.input_links_snapshot_count.store(1, std::memory_order_relaxed);

    const std::string rendered = srtrelay::RenderPrometheusMetrics(metrics);
    ExpectContains(rendered, "# HELP srt_relay_rx_bytes_total Total bytes received by relay input.");
    ExpectContains(rendered, "srt_relay_rx_bytes_total 111");
    ExpectContains(rendered, "srt_relay_tx_bytes_total 222");
    ExpectContains(rendered, "srt_relay_path_ready 1");
    ExpectContains(rendered, "srt_relay_output_sources_total 2");
    ExpectContains(rendered, "srt_relay_output_source_connected{output_index=\"1\"} 1");
    ExpectContains(rendered, "srt_relay_output_source_connected{output_index=\"2\"} 0");
    ExpectContains(rendered, "srt_relay_input_link_rx_bytes_total{link_index=\"1\",socket_id=\"5001\"} 999");
}

void TestInputCompactionBehavior() {
    srtrelay::MetricsState metrics;
    metrics.input_links_snapshot_count.store(3, std::memory_order_relaxed);

    metrics.input_tracked.slots[0].member_identity_key = 11;
    metrics.input_tracked.slots[0].member_id = static_cast<int64_t>(SRT_INVALID_SOCK);
    metrics.input_tracked.slots[0].member_connected = 0;

    metrics.input_tracked.slots[1].member_identity_key = 22;
    metrics.input_tracked.slots[1].member_id = 1001;
    metrics.input_tracked.slots[1].member_connected = 1;
    metrics.input_tracked.slots[1].link_rx_bytes_total = 300;

    metrics.input_tracked.slots[2].member_identity_key = 33;
    metrics.input_tracked.slots[2].member_id = 1002;
    metrics.input_tracked.slots[2].member_connected = 1;
    metrics.input_tracked.slots[2].link_rx_bytes_total = 400;

    srtrelay::CompactResult result;
    {
        srtrelay::MetricsState::LinkMetricsGuard lock(metrics);
        result = srtrelay::CompactSlotsLocked(srtrelay::LinkSide::kInput, &metrics);
    }

    assert(result.before_slots == 3);
    assert(result.after_slots == 2);
    assert(result.dropped == 1);
    assert(metrics.input_links_snapshot_count.load(std::memory_order_relaxed) == 2);
    assert(metrics.input_tracked.slots[0].member_id == 1001);
    assert(metrics.input_tracked.slots[1].member_id == 1002);
    assert(metrics.input_tracked.slots[0].link_rx_bytes_total == 300);
    assert(metrics.input_tracked.slots[1].link_rx_bytes_total == 400);
}

void TestParseArgsMultiOutput() {
    const srtrelay::Config cfg = ParseConfig({
        "srt-bond-relay",
        "--input", "stdin",
        "--output", "udp://127.0.0.1:5000",
        "--output", "udp://127.0.0.1:5001",
    });
    assert(cfg.output_uris.size() == 2);
    assert(cfg.output_uris[0] == "udp://127.0.0.1:5000");
    assert(cfg.output_uris[1] == "udp://127.0.0.1:5001");
}

void TestParseArgsRejectsMultipleStdoutOutputs() {
    ExpectThrows([]() {
        (void)ParseConfig({
            "srt-bond-relay",
            "--input", "stdin",
            "--output", "stdout",
            "--output", "fd://stdout",
        });
    });
}

void TestParseOutputEndpointSpecsPreservesBondedSingleFlag() {
    srtrelay::Config cfg;
    cfg.output_uris = {
        "srt://127.0.0.1:5000?mode=caller;srt://127.0.0.2:5000?mode=caller&grouptype=broadcast",
        "udp://127.0.0.1:6000?mode=caller",
    };
    const auto specs = srtrelay::ParseOutputEndpointSpecs(cfg);
    assert(specs.size() == 2);
    assert(specs[0].kind == srtrelay::OutputEndpointKind::kSrtCaller);
    assert(specs[0].bonded);
    assert(specs[0].uris.size() == 2);
    assert(specs[1].kind == srtrelay::OutputEndpointKind::kUdpCaller);
}

}  // namespace

int main() {
    TestPrometheusCompatibility();
    TestInputCompactionBehavior();
    TestParseArgsMultiOutput();
    TestParseArgsRejectsMultipleStdoutOutputs();
    TestParseOutputEndpointSpecsPreservesBondedSingleFlag();
    std::cout << "metrics_compat_test passed\n";
    return 0;
}
