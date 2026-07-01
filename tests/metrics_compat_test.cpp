#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "srtrelay/config.hpp"
#include "srtrelay/metrics.hpp"
#include "srtrelay/relay_io.hpp"
#include "metrics_link_slots.hpp"
#include "test_expect.hpp"

namespace {

using srtrelay::test::ExpectContains;
using srtrelay::test::ExpectNotContains;
using srtrelay::test::ExpectThrows;

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

void TestPrometheusOutputLinkMetricCompatibility() {
    srtrelay::MetricsState metrics;
    metrics.output_links_snapshot_count.store(1, std::memory_order_relaxed);
    metrics.output_tracked.slots[0].member_identity_key = 4321;
    metrics.output_tracked.slots[0].member_id = 6001;
    metrics.output_tracked.slots[0].member_connected = 1;
    metrics.output_tracked.slots[0].link_tx_bytes_total = 12345;

    const std::string rendered = srtrelay::RenderPrometheusMetrics(metrics);
    ExpectContains(rendered,
                   "srt_relay_output_link_tx_bytes_total{link_index=\"1\",socket_id=\"6001\"} 12345");
}

void TestPrometheusSnapshotCountClampingAndGaugeInvariants() {
    srtrelay::MetricsState metrics;
    metrics.input_links_snapshot_count.store(-9, std::memory_order_relaxed);
    metrics.input_tracked.slots[0].member_identity_key = 1010;
    metrics.input_tracked.slots[0].member_id = 7001;
    metrics.input_tracked.slots[0].member_connected = 1;

    metrics.output_links_snapshot_count.store(999, std::memory_order_relaxed);
    metrics.output_tracked.slots[15].member_identity_key = 2020;
    metrics.output_tracked.slots[15].member_id = 7016;
    metrics.output_tracked.slots[15].member_connected = 1;
    metrics.output_tracked.slots[15].link_tx_bytes_total = 16;

    const std::string rendered = srtrelay::RenderPrometheusMetrics(metrics);

    // Negative input snapshot count must clamp to zero and emit no per-link input entries.
    ExpectNotContains(rendered, "srt_relay_input_link_connected{");
    // Oversized output snapshot count must clamp to max tracked members (slot 16 allowed).
    ExpectContains(rendered,
                   "srt_relay_output_link_tx_bytes_total{link_index=\"16\",socket_id=\"7016\"} 16");
    ExpectNotContains(rendered, "link_index=\"17\"");

    // One-hot gauge defaults remain stable for dashboards.
    ExpectContains(rendered, "srt_relay_input_bond_mode{mode=\"unknown\"} 1");
    ExpectContains(rendered, "srt_relay_input_bond_mode{mode=\"broadcast\"} 0");
    ExpectContains(rendered, "srt_relay_input_bond_mode{mode=\"backup\"} 0");
    ExpectContains(rendered, "srt_relay_switch_policy{policy=\"round_robin\"} 1");
    ExpectContains(rendered, "srt_relay_switch_mode{mode=\"serial\"} 1");
}

void TestBuildCompactResponseJsonIncludeFlagsAndFields() {
    srtrelay::CompactResponse input_only;
    input_only.direction = "input";
    input_only.include_input = true;
    input_only.include_output = false;
    input_only.input.before_slots = 3;
    input_only.input.after_slots = 2;
    input_only.input.moved = 1;
    input_only.input.dropped = 1;

    const auto parsed_input = nlohmann::json::parse(srtrelay::BuildCompactResponseJson(input_only));
    assert(parsed_input["direction"] == "input");
    assert(parsed_input.contains("input"));
    assert(!parsed_input.contains("output"));
    assert(parsed_input["input"]["before_slots"] == 3);
    assert(parsed_input["input"]["after_slots"] == 2);
    assert(parsed_input["input"]["moved"] == 1);
    assert(parsed_input["input"]["dropped"] == 1);

    srtrelay::CompactResponse both;
    both.direction = "both";
    both.include_input = true;
    both.include_output = true;
    both.input.before_slots = 4;
    both.input.after_slots = 3;
    both.input.moved = 2;
    both.input.dropped = 1;
    both.output.before_slots = 6;
    both.output.after_slots = 5;
    both.output.moved = 1;
    both.output.dropped = 1;

    const auto parsed_both = nlohmann::json::parse(srtrelay::BuildCompactResponseJson(both));
    assert(parsed_both["direction"] == "both");
    assert(parsed_both.contains("input"));
    assert(parsed_both.contains("output"));
    assert(parsed_both["input"]["before_slots"] == 4);
    assert(parsed_both["output"]["after_slots"] == 5);
    assert(parsed_both["output"]["moved"] == 1);
    assert(parsed_both["output"]["dropped"] == 1);
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
    TestPrometheusOutputLinkMetricCompatibility();
    TestPrometheusSnapshotCountClampingAndGaugeInvariants();
    TestBuildCompactResponseJsonIncludeFlagsAndFields();
    TestParseArgsMultiOutput();
    TestParseArgsRejectsMultipleStdoutOutputs();
    TestParseOutputEndpointSpecsPreservesBondedSingleFlag();
    std::cout << "metrics_compat_test passed\n";
    return 0;
}
