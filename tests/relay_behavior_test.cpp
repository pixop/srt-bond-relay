#include <arpa/inet.h>

#include <cassert>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "metrics_link_slots.hpp"
#include "srtrelay/config.hpp"
#include "srtrelay/metrics.hpp"
#include "srtrelay/relay_io.hpp"

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

sockaddr_storage MakeIpv4Sockaddr(const std::string& host, int port) {
    sockaddr_storage storage {};
    auto* addr = reinterpret_cast<sockaddr_in*>(&storage);
    addr->sin_family = AF_INET;
    addr->sin_port = htons(static_cast<uint16_t>(port));
    const int rc = inet_pton(AF_INET, host.c_str(), &addr->sin_addr);
    assert(rc == 1);
    return storage;
}

SRT_SOCKGROUPDATA MakeConnectedMember(SRTSOCKET socket_id, const std::string& host, int port) {
    SRT_SOCKGROUPDATA member {};
    member.id = socket_id;
    member.sockstate = SRTS_CONNECTED;
    member.memberstate = SRT_GST_RUNNING;
    member.peeraddr = MakeIpv4Sockaddr(host, port);
    return member;
}

void TestBuildOutputSinkFanoutShape() {
    srtrelay::Config cfg;
    cfg.output_uris = {
        "udp://127.0.0.1:5000?mode=caller",
        "udp://127.0.0.1:5001?mode=caller",
    };

    auto specs = srtrelay::ParseOutputEndpointSpecs(cfg);
    auto sink = srtrelay::BuildOutputSink(cfg, std::move(specs));
    assert(sink != nullptr);
    assert(sink->OutputCount() == 2);
    assert(!sink->IsConnected());
    assert(sink->NeedsEnsurePoll());
    assert(!sink->OutputConnected(0));
    assert(!sink->OutputConnected(1));
}

void TestBuildOutputSinkSinglePathCompatibility() {
    srtrelay::Config cfg;
    cfg.output_uris = {"stdout"};
    auto specs = srtrelay::ParseOutputEndpointSpecs(cfg);
    auto sink = srtrelay::BuildOutputSink(cfg, std::move(specs));
    assert(sink != nullptr);
    assert(sink->OutputCount() == 1);
}

void TestParseOutputEndpointSpecsRejectsGroupedUdp() {
    srtrelay::Config cfg;
    cfg.output_uris = {"udp://127.0.0.1:5000,udp://127.0.0.1:5001"};
    ExpectThrows([&cfg]() {
        (void)srtrelay::ParseOutputEndpointSpecs(cfg);
    });
}

void TestInputSlotReuseByEndpointIdentity() {
    srtrelay::MetricsState metrics;

    const std::vector<SRT_SOCKGROUPDATA> first_members = {
        MakeConnectedMember(101, "10.1.1.1", 9000),
        MakeConnectedMember(102, "10.1.1.2", 9000),
    };
    srtrelay::SaveMemberSnapshot(srtrelay::LinkSide::kInput, first_members, &metrics);

    assert(metrics.input_tracked.slots[0].member_peer_host == "10.1.1.1");
    assert(metrics.input_tracked.slots[1].member_peer_host == "10.1.1.2");

    const std::vector<SRT_SOCKGROUPDATA> reconnect_members = {
        MakeConnectedMember(202, "10.1.1.2", 9000),
        MakeConnectedMember(201, "10.1.1.1", 9000),
    };
    srtrelay::SaveMemberSnapshot(srtrelay::LinkSide::kInput, reconnect_members, &metrics);

    // Same endpoint identities should retain their original slots, even with new socket ids/order.
    assert(metrics.input_tracked.slots[0].member_peer_host == "10.1.1.1");
    assert(metrics.input_tracked.slots[0].member_id == 201);
    assert(metrics.input_tracked.slots[1].member_peer_host == "10.1.1.2");
    assert(metrics.input_tracked.slots[1].member_id == 202);
}

void TestOutputCompactionBehavior() {
    srtrelay::MetricsState metrics;
    metrics.output_links_snapshot_count.store(3, std::memory_order_relaxed);

    metrics.output_tracked.slots[0].member_identity_key = 11;
    metrics.output_tracked.slots[0].member_id = static_cast<int64_t>(SRT_INVALID_SOCK);
    metrics.output_tracked.slots[0].member_connected = 0;

    metrics.output_tracked.slots[1].member_identity_key = 22;
    metrics.output_tracked.slots[1].member_id = 2001;
    metrics.output_tracked.slots[1].member_connected = 1;
    metrics.output_tracked.slots[1].link_tx_bytes_total = 700;

    metrics.output_tracked.slots[2].member_identity_key = 33;
    metrics.output_tracked.slots[2].member_id = 2002;
    metrics.output_tracked.slots[2].member_connected = 1;
    metrics.output_tracked.slots[2].link_tx_bytes_total = 900;

    srtrelay::CompactResult result;
    {
        srtrelay::MetricsState::LinkMetricsGuard lock(metrics);
        result = srtrelay::CompactSlotsLocked(srtrelay::LinkSide::kOutput, &metrics);
    }

    assert(result.before_slots == 3);
    assert(result.after_slots == 2);
    assert(result.dropped == 1);
    assert(metrics.output_links_snapshot_count.load(std::memory_order_relaxed) == 2);
    assert(metrics.output_tracked.slots[0].member_id == 2001);
    assert(metrics.output_tracked.slots[1].member_id == 2002);
    assert(metrics.output_tracked.slots[0].link_tx_bytes_total == 700);
    assert(metrics.output_tracked.slots[1].link_tx_bytes_total == 900);
}

void TestLinkStatusCompactFormatting() {
    srtrelay::MetricsState metrics;
    metrics.input_links_snapshot_count.store(2, std::memory_order_relaxed);
    metrics.output_links_snapshot_count.store(1, std::memory_order_relaxed);

    metrics.input_tracked.slots[0].member_identity_key = 11;
    metrics.input_tracked.slots[0].member_id = 3001;
    metrics.input_tracked.slots[0].member_connected = 1;
    metrics.input_tracked.slots[1].member_identity_key = 22;
    metrics.input_tracked.slots[1].member_id = 3002;
    metrics.input_tracked.slots[1].member_connected = 0;

    metrics.output_tracked.slots[0].member_identity_key = 33;
    metrics.output_tracked.slots[0].member_id = 4001;
    metrics.output_tracked.slots[0].member_connected = 1;

    const std::string input_status = srtrelay::BuildInputLinkStatusCompact(metrics);
    const std::string output_status = srtrelay::BuildOutputLinkStatusCompact(metrics);
    ExpectContains(input_status, "slot1[socket=3001,state=up]");
    ExpectContains(input_status, "slot2[socket=3002,state=down]");
    ExpectContains(output_status, "slot1[socket=4001,state=up]");
}

}  // namespace

int main() {
    TestBuildOutputSinkFanoutShape();
    TestBuildOutputSinkSinglePathCompatibility();
    TestParseOutputEndpointSpecsRejectsGroupedUdp();
    TestInputSlotReuseByEndpointIdentity();
    TestOutputCompactionBehavior();
    TestLinkStatusCompactFormatting();
    std::cout << "relay_behavior_test passed\n";
    return 0;
}
