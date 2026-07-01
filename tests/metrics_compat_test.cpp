#include <cassert>
#include <iostream>
#include <string>

#include "srtrelay/metrics.hpp"
#include "metrics_link_slots.hpp"

namespace {

void ExpectContains(const std::string& haystack, const std::string& needle) {
    if (haystack.find(needle) == std::string::npos) {
        std::cerr << "missing expected snippet: " << needle << "\n";
        std::abort();
    }
}

void TestPrometheusCompatibility() {
    srtrelay::MetricsState metrics;
    metrics.total_rx_bytes.store(111, std::memory_order_relaxed);
    metrics.total_tx_bytes.store(222, std::memory_order_relaxed);
    metrics.input_connected.store(1, std::memory_order_relaxed);
    metrics.output_connected.store(1, std::memory_order_relaxed);
    metrics.input_sources_total.store(2, std::memory_order_relaxed);

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

}  // namespace

int main() {
    TestPrometheusCompatibility();
    TestInputCompactionBehavior();
    std::cout << "metrics_compat_test passed\n";
    return 0;
}
