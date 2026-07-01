#include "metrics_link_slots.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <sstream>

#include <arpa/inet.h>

#include <nlohmann/json.hpp>

namespace srtrelay {
namespace {

using json = nlohmann::json;

bool IsConnectedGroupMember(const SRT_SOCKGROUPDATA& member) {
    if (member.memberstate == SRT_GST_RUNNING) {
        return true;
    }
    return member.sockstate == SRTS_CONNECTED;
}

bool TryExtractEndpoint(const sockaddr_storage& addr, std::string* out_host, int* out_port) {
    if (out_host == nullptr || out_port == nullptr) {
        return false;
    }
    char host_buf[INET6_ADDRSTRLEN] = {};
    if (addr.ss_family == AF_INET) {
        const auto* v4 = reinterpret_cast<const sockaddr_in*>(&addr);
        if (::inet_ntop(AF_INET, &(v4->sin_addr), host_buf, sizeof(host_buf)) == nullptr) {
            return false;
        }
        *out_host = host_buf;
        *out_port = static_cast<int>(ntohs(v4->sin_port));
        return true;
    }
    if (addr.ss_family == AF_INET6) {
        const auto* v6 = reinterpret_cast<const sockaddr_in6*>(&addr);
        if (::inet_ntop(AF_INET6, &(v6->sin6_addr), host_buf, sizeof(host_buf)) == nullptr) {
            return false;
        }
        *out_host = host_buf;
        *out_port = static_cast<int>(ntohs(v6->sin6_port));
        return true;
    }
    return false;
}

uint64_t HashBytesFnv1a64(const void* data, size_t size, uint64_t seed = 1469598103934665603ULL) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    uint64_t hash = seed;
    for (size_t i = 0; i < size; ++i) {
        hash ^= static_cast<uint64_t>(bytes[i]);
        hash *= 1099511628211ULL;
    }
    return hash;
}

bool TryHashSockaddrEndpoint(const sockaddr_storage& addr, uint64_t* out_hash) {
    if (out_hash == nullptr) {
        return false;
    }
    uint64_t hash = 1469598103934665603ULL;
    if (addr.ss_family == AF_INET) {
        const auto* v4 = reinterpret_cast<const sockaddr_in*>(&addr);
        hash = HashBytesFnv1a64(&v4->sin_addr, sizeof(v4->sin_addr), hash);
        const uint16_t port = ntohs(v4->sin_port);
        hash = HashBytesFnv1a64(&port, sizeof(port), hash);
        *out_hash = hash;
        return true;
    }
    if (addr.ss_family == AF_INET6) {
        const auto* v6 = reinterpret_cast<const sockaddr_in6*>(&addr);
        hash = HashBytesFnv1a64(&v6->sin6_addr, sizeof(v6->sin6_addr), hash);
        const uint16_t port = ntohs(v6->sin6_port);
        hash = HashBytesFnv1a64(&port, sizeof(port), hash);
        *out_hash = hash;
        return true;
    }
    return false;
}

bool TryReadSocketLocalAddress(SRTSOCKET sock, sockaddr_storage* out_addr) {
    if (sock == SRT_INVALID_SOCK || sock == 0 || out_addr == nullptr) {
        return false;
    }
    sockaddr_storage addr {};
    int len = static_cast<int>(sizeof(addr));
    if (srt_getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &len) == SRT_ERROR) {
        return false;
    }
    *out_addr = addr;
    return true;
}

uint64_t MemberIdentityKey(LinkSide side, const SRT_SOCKGROUPDATA& member) {
    // Input slot identity should follow the relay local leg endpoint so reconnects
    // with new sender source ports still map to the same slot.
    if (side == LinkSide::kInput) {
        sockaddr_storage local_addr {};
        if (TryReadSocketLocalAddress(member.id, &local_addr)) {
            uint64_t local_hash = 0;
            if (TryHashSockaddrEndpoint(local_addr, &local_hash)) {
                return local_hash;
            }
        }
    }

    uint64_t endpoint_hash = 0;
    if (TryHashSockaddrEndpoint(member.peeraddr, &endpoint_hash)) {
        return endpoint_hash;
    }

    if (member.token != 0) {
        return HashBytesFnv1a64(&member.token, sizeof(member.token));
    }
    return 0;
}

struct SnapshotSlotState {
    SRTSOCKET id = SRT_INVALID_SOCK;
    int connected = 0;
    uint64_t identity_key = 0;
    int sock_state = SRTS_NONEXIST;
    int group_state = SRT_GST_IDLE;
    int peer_port = 0;
    std::string peer_host;
};

using SnapshotSlots = std::array<SnapshotSlotState, MetricsState::kMaxTrackedMembers>;

void ReadSnapshotSlots(LinkSide side, const MetricsState& metrics, SnapshotSlots* slots) {
    if (side == LinkSide::kInput) {
        const auto& tracked_slots = metrics.input_tracked.slots;
        for (size_t i = 0; i < MetricsState::kMaxTrackedMembers; ++i) {
            (*slots)[i].id = static_cast<SRTSOCKET>(tracked_slots[i].member_id);
            (*slots)[i].connected = tracked_slots[i].member_connected;
            (*slots)[i].identity_key = tracked_slots[i].member_identity_key;
            (*slots)[i].sock_state = tracked_slots[i].member_sock_state;
            (*slots)[i].group_state = tracked_slots[i].member_group_state;
            (*slots)[i].peer_port = tracked_slots[i].member_peer_port;
            (*slots)[i].peer_host = tracked_slots[i].member_peer_host;
        }
    } else {
        const auto& tracked_slots = metrics.output_tracked.slots;
        for (size_t i = 0; i < MetricsState::kMaxTrackedMembers; ++i) {
            (*slots)[i].id = static_cast<SRTSOCKET>(tracked_slots[i].member_id);
            (*slots)[i].connected = tracked_slots[i].member_connected;
            (*slots)[i].identity_key = tracked_slots[i].member_identity_key;
            (*slots)[i].sock_state = tracked_slots[i].member_sock_state;
            (*slots)[i].group_state = tracked_slots[i].member_group_state;
            (*slots)[i].peer_port = tracked_slots[i].member_peer_port;
            (*slots)[i].peer_host = tracked_slots[i].member_peer_host;
        }
    }
}

void WriteSnapshotSlots(LinkSide side, MetricsState* metrics, const SnapshotSlots& slots, size_t* out_span) {
    size_t span = 0;
    if (side == LinkSide::kInput) {
        auto& tracked_slots = metrics->input_tracked.slots;
        for (size_t i = 0; i < MetricsState::kMaxTrackedMembers; ++i) {
            tracked_slots[i].member_id = static_cast<int64_t>(slots[i].id);
            tracked_slots[i].member_connected = slots[i].connected;
            tracked_slots[i].member_identity_key = slots[i].identity_key;
            tracked_slots[i].member_sock_state = slots[i].sock_state;
            tracked_slots[i].member_group_state = slots[i].group_state;
            tracked_slots[i].member_peer_port = slots[i].peer_port;
            tracked_slots[i].member_peer_host = slots[i].peer_host;
            if (slots[i].identity_key != 0) {
                span = i + 1;
            }
        }
    } else {
        auto& tracked_slots = metrics->output_tracked.slots;
        for (size_t i = 0; i < MetricsState::kMaxTrackedMembers; ++i) {
            tracked_slots[i].member_id = static_cast<int64_t>(slots[i].id);
            tracked_slots[i].member_connected = slots[i].connected;
            tracked_slots[i].member_identity_key = slots[i].identity_key;
            tracked_slots[i].member_sock_state = slots[i].sock_state;
            tracked_slots[i].member_group_state = slots[i].group_state;
            tracked_slots[i].member_peer_port = slots[i].peer_port;
            tracked_slots[i].member_peer_host = slots[i].peer_host;
            if (slots[i].identity_key != 0) {
                span = i + 1;
            }
        }
    }
    *out_span = span;
}

void ResetTrafficCountersForSlot(LinkSide side, MetricsState* metrics, size_t slot) {
    metrics->AssertLinkMetricsLocked("ResetTrafficCountersForSlot");
    if (side == LinkSide::kInput) {
        auto& s = metrics->input_tracked.slots[slot];
        s.link_rx_bytes_current = 0;
        s.link_tx_bytes_current = 0;
        s.link_rx_bytes_last = 0;
        s.link_tx_bytes_last = 0;
        s.link_rx_bytes_total = 0;
        s.link_tx_bytes_total = 0;
        s.link_packet_belated_current = 0;
        s.link_packet_belated_last = 0;
        s.link_packet_belated_total = 0;
    } else {
        auto& s = metrics->output_tracked.slots[slot];
        s.link_rx_bytes_current = 0;
        s.link_tx_bytes_current = 0;
        s.link_rx_bytes_last = 0;
        s.link_tx_bytes_last = 0;
        s.link_rx_bytes_total = 0;
        s.link_tx_bytes_total = 0;
    }
}

struct CompactionSlotState {
    int64_t member_id = static_cast<int64_t>(SRT_INVALID_SOCK);
    int member_connected = 0;
    uint64_t member_identity = 0;
    uint64_t rx_total = 0;
    uint64_t tx_total = 0;
    uint64_t rx_current = 0;
    uint64_t tx_current = 0;
    uint64_t rx_last = 0;
    uint64_t tx_last = 0;
    uint64_t belated_total = 0;
    uint64_t belated_current = 0;
    uint64_t belated_last = 0;
    int64_t rtt_ms = -1;
};

CompactionSlotState ReadCompactionSlotState(LinkSide side, const MetricsState& metrics, size_t index) {
    CompactionSlotState state {};
    if (side == LinkSide::kInput) {
        const auto& s = metrics.input_tracked.slots[index];
        state.member_id = s.member_id;
        state.member_connected = s.member_connected;
        state.member_identity = s.member_identity_key;
        state.rx_total = s.link_rx_bytes_total;
        state.tx_total = s.link_tx_bytes_total;
        state.rx_current = s.link_rx_bytes_current;
        state.tx_current = s.link_tx_bytes_current;
        state.rx_last = s.link_rx_bytes_last;
        state.tx_last = s.link_tx_bytes_last;
        state.belated_total = s.link_packet_belated_total;
        state.belated_current = s.link_packet_belated_current;
        state.belated_last = s.link_packet_belated_last;
        state.rtt_ms = s.link_rtt_ms;
    } else {
        const auto& s = metrics.output_tracked.slots[index];
        state.member_id = s.member_id;
        state.member_connected = s.member_connected;
        state.member_identity = s.member_identity_key;
        state.rx_total = s.link_rx_bytes_total;
        state.tx_total = s.link_tx_bytes_total;
        state.rx_current = s.link_rx_bytes_current;
        state.tx_current = s.link_tx_bytes_current;
        state.rx_last = s.link_rx_bytes_last;
        state.tx_last = s.link_tx_bytes_last;
        state.rtt_ms = s.link_rtt_ms;
    }
    return state;
}

void WriteCompactionSlotState(LinkSide side, MetricsState* metrics, size_t index, const CompactionSlotState& state) {
    if (side == LinkSide::kInput) {
        auto& s = metrics->input_tracked.slots[index];
        s.member_id = state.member_id;
        s.member_connected = state.member_connected;
        s.member_identity_key = state.member_identity;
        s.link_rx_bytes_total = state.rx_total;
        s.link_tx_bytes_total = state.tx_total;
        s.link_rx_bytes_current = state.rx_current;
        s.link_tx_bytes_current = state.tx_current;
        s.link_rx_bytes_last = state.rx_last;
        s.link_tx_bytes_last = state.tx_last;
        s.link_packet_belated_total = state.belated_total;
        s.link_packet_belated_current = state.belated_current;
        s.link_packet_belated_last = state.belated_last;
        s.link_rtt_ms = state.rtt_ms;
    } else {
        auto& s = metrics->output_tracked.slots[index];
        s.member_id = state.member_id;
        s.member_connected = state.member_connected;
        s.member_identity_key = state.member_identity;
        s.link_rx_bytes_total = state.rx_total;
        s.link_tx_bytes_total = state.tx_total;
        s.link_rx_bytes_current = state.rx_current;
        s.link_tx_bytes_current = state.tx_current;
        s.link_rx_bytes_last = state.rx_last;
        s.link_tx_bytes_last = state.tx_last;
        s.link_rtt_ms = state.rtt_ms;
    }
}

void ResetCompactionSlotState(CompactionSlotState* state) {
    *state = CompactionSlotState{};
}

}  // namespace

void SaveMemberSnapshot(LinkSide side,
                        const std::vector<SRT_SOCKGROUPDATA>& group_members,
                        MetricsState* metrics) {
    MetricsState::LinkMetricsGuard lock(*metrics);

    SnapshotSlots slots {};
    ReadSnapshotSlots(side, *metrics, &slots);

    std::array<bool, MetricsState::kMaxTrackedMembers> slot_used {};
    std::array<int, MetricsState::kMaxTrackedMembers> member_slot {};
    member_slot.fill(-1);
    std::vector<bool> matched(group_members.size(), false);

    for (size_t i = 0; i < group_members.size(); ++i) {
        const uint64_t key = MemberIdentityKey(side, group_members[i]);
        if (key == 0) {
            continue;
        }
        for (size_t slot = 0; slot < MetricsState::kMaxTrackedMembers; ++slot) {
            if (slot_used[slot] || slots[slot].identity_key != key) {
                continue;
            }
            member_slot[i] = static_cast<int>(slot);
            slot_used[slot] = true;
            matched[i] = true;
            break;
        }
    }

    for (size_t i = 0; i < group_members.size(); ++i) {
        if (matched[i]) {
            continue;
        }
        for (size_t slot = 0; slot < MetricsState::kMaxTrackedMembers; ++slot) {
            if (slot_used[slot] || slots[slot].identity_key != 0) {
                continue;
            }
            member_slot[i] = static_cast<int>(slot);
            slot_used[slot] = true;
            matched[i] = true;
            break;
        }
    }

    for (size_t i = 0; i < group_members.size(); ++i) {
        if (matched[i]) {
            continue;
        }
        for (size_t slot = 0; slot < MetricsState::kMaxTrackedMembers; ++slot) {
            if (slot_used[slot]) {
                continue;
            }
            member_slot[i] = static_cast<int>(slot);
            slot_used[slot] = true;
            matched[i] = true;
            slots[slot].id = SRT_INVALID_SOCK;
            slots[slot].connected = 0;
            slots[slot].identity_key = 0;
            ResetTrafficCountersForSlot(side, metrics, slot);
            break;
        }
    }

    std::array<bool, MetricsState::kMaxTrackedMembers> matched_slot {};
    for (size_t i = 0; i < group_members.size(); ++i) {
        const int slot = member_slot[i];
        if (slot < 0) {
            continue;
        }
        matched_slot[static_cast<size_t>(slot)] = true;
        const auto& member = group_members[i];
        slots[slot].id = member.id;
        slots[slot].connected = IsConnectedGroupMember(member) ? 1 : 0;
        slots[slot].sock_state = member.sockstate;
        slots[slot].group_state = member.memberstate;

        std::string peer_host;
        int peer_port = 0;
        if (TryExtractEndpoint(member.peeraddr, &peer_host, &peer_port)) {
            slots[slot].peer_host = peer_host;
            slots[slot].peer_port = peer_port;
        } else {
            slots[slot].peer_host.clear();
            slots[slot].peer_port = 0;
        }

        uint64_t identity_key = MemberIdentityKey(side, member);
        if (identity_key == 0) {
            identity_key = slots[slot].identity_key;
        }
        slots[slot].identity_key = identity_key;
    }

    for (size_t slot = 0; slot < MetricsState::kMaxTrackedMembers; ++slot) {
        if (matched_slot[slot]) {
            continue;
        }
        if (slots[slot].identity_key == 0) {
            slots[slot].id = SRT_INVALID_SOCK;
            slots[slot].connected = 0;
            slots[slot].sock_state = SRTS_NONEXIST;
            slots[slot].group_state = SRT_GST_IDLE;
            slots[slot].peer_host.clear();
            slots[slot].peer_port = 0;
            continue;
        }
        slots[slot].id = SRT_INVALID_SOCK;
        slots[slot].connected = 0;
    }

    size_t span = 0;
    WriteSnapshotSlots(side, metrics, slots, &span);
    if (side == LinkSide::kInput) {
        metrics->input_links_snapshot_count.store(static_cast<int64_t>(span), std::memory_order_relaxed);
    } else {
        metrics->output_links_snapshot_count.store(static_cast<int64_t>(span), std::memory_order_relaxed);
    }
}

void ClearTrackedMembersForDisconnectedSocket(LinkSide side, MetricsState* metrics) {
    MetricsState::LinkMetricsGuard lock(*metrics);
    for (size_t i = 0; i < MetricsState::kMaxTrackedMembers; ++i) {
        const uint64_t key = side == LinkSide::kInput
            ? metrics->input_tracked.slots[i].member_identity_key
            : metrics->output_tracked.slots[i].member_identity_key;
        if (key == 0) {
            continue;
        }

        if (side == LinkSide::kInput) {
            auto& s = metrics->input_tracked.slots[i];
            s.member_connected = 0;
            s.member_id = static_cast<int64_t>(SRT_INVALID_SOCK);
            s.member_identity_key = 0;
            s.member_sock_state = SRTS_NONEXIST;
            s.member_group_state = SRT_GST_IDLE;
            s.member_peer_port = 0;
            s.member_peer_host.clear();
        } else {
            auto& s = metrics->output_tracked.slots[i];
            s.member_connected = 0;
            s.member_id = static_cast<int64_t>(SRT_INVALID_SOCK);
            s.member_identity_key = 0;
            s.member_sock_state = SRTS_NONEXIST;
            s.member_group_state = SRT_GST_IDLE;
            s.member_peer_port = 0;
            s.member_peer_host.clear();
        }
        ResetTrafficCountersForSlot(side, metrics, i);
    }
    if (side == LinkSide::kInput) {
        metrics->input_links_snapshot_count.store(0, std::memory_order_relaxed);
    } else {
        metrics->output_links_snapshot_count.store(0, std::memory_order_relaxed);
    }
}

void MarkAllTrackedLinksDisconnected(LinkSide side, MetricsState* metrics) {
    MetricsState::LinkMetricsGuard lock(*metrics);
    const size_t capped = metrics->SnapshotCountCapped(side);
    if (side == LinkSide::kInput) {
        auto& tracked_slots = metrics->input_tracked.slots;
        for (size_t i = 0; i < capped; ++i) {
            const uint64_t identity_key = tracked_slots[i].member_identity_key;
            if (identity_key == 0) {
                continue;
            }
            tracked_slots[i].member_connected = 0;
        }
    } else {
        auto& tracked_slots = metrics->output_tracked.slots;
        for (size_t i = 0; i < capped; ++i) {
            const uint64_t identity_key = tracked_slots[i].member_identity_key;
            if (identity_key == 0) {
                continue;
            }
            tracked_slots[i].member_connected = 0;
        }
    }
}

CompactResult CompactSlotsLocked(LinkSide side, MetricsState* metrics) {
    metrics->AssertLinkMetricsLocked("CompactSlotsLocked");
    CompactResult result {};
    std::array<CompactionSlotState, MetricsState::kMaxTrackedMembers> compacted {};

    std::array<size_t, MetricsState::kMaxTrackedMembers> keep_slots {};
    size_t keep_count = 0;
    if (side == LinkSide::kInput) {
        const auto& tracked_slots = metrics->input_tracked.slots;
        for (size_t i = 0; i < MetricsState::kMaxTrackedMembers; ++i) {
            const auto identity = tracked_slots[i].member_identity_key;
            if (identity == 0) {
                continue;
            }
            result.before_slots++;
            const bool is_connected = tracked_slots[i].member_connected == 1;
            const auto socket_id = static_cast<SRTSOCKET>(tracked_slots[i].member_id);
            if (!is_connected || socket_id == SRT_INVALID_SOCK || socket_id == 0) {
                continue;
            }
            keep_slots[keep_count++] = i;
        }
    } else {
        const auto& tracked_slots = metrics->output_tracked.slots;
        for (size_t i = 0; i < MetricsState::kMaxTrackedMembers; ++i) {
            const auto identity = tracked_slots[i].member_identity_key;
            if (identity == 0) {
                continue;
            }
            result.before_slots++;
            const bool is_connected = tracked_slots[i].member_connected == 1;
            const auto socket_id = static_cast<SRTSOCKET>(tracked_slots[i].member_id);
            if (!is_connected || socket_id == SRT_INVALID_SOCK || socket_id == 0) {
                continue;
            }
            keep_slots[keep_count++] = i;
        }
    }

    result.after_slots = keep_count;
    result.dropped = result.before_slots - keep_count;
    for (size_t dst = 0; dst < keep_count; ++dst) {
        const size_t src = keep_slots[dst];
        if (src != dst) {
            result.moved++;
        }
        compacted[dst] = ReadCompactionSlotState(side, *metrics, src);
    }

    for (size_t i = keep_count; i < MetricsState::kMaxTrackedMembers; ++i) {
        ResetCompactionSlotState(&compacted[i]);
    }

    for (size_t i = 0; i < MetricsState::kMaxTrackedMembers; ++i) {
        WriteCompactionSlotState(side, metrics, i, compacted[i]);
    }

    if (side == LinkSide::kInput) {
        metrics->input_links_snapshot_count.store(static_cast<int64_t>(keep_count), std::memory_order_relaxed);
    } else {
        metrics->output_links_snapshot_count.store(static_cast<int64_t>(keep_count), std::memory_order_relaxed);
    }

    return result;
}

std::string BuildLinkStatusCompact(LinkSide side, const MetricsState& metrics) {
    MetricsState::LinkMetricsGuard lock(metrics);
    const size_t capped = metrics.SnapshotCountCapped(side);
    std::ostringstream out;
    bool first = true;
    if (side == LinkSide::kInput) {
        const auto& tracked_slots = metrics.input_tracked.slots;
        for (size_t i = 0; i < capped; ++i) {
            const auto identity_key = tracked_slots[i].member_identity_key;
            if (identity_key == 0) {
                continue;
            }
            if (!first) {
                out << ",";
            }
            first = false;
            const auto socket_id = static_cast<SRTSOCKET>(tracked_slots[i].member_id);
            const auto is_connected = tracked_slots[i].member_connected == 1;
            out << "slot" << (i + 1) << "[socket=" << socket_id
                << ",state=" << (is_connected ? "up" : "down") << "]";
        }
    } else {
        const auto& tracked_slots = metrics.output_tracked.slots;
        for (size_t i = 0; i < capped; ++i) {
            const auto identity_key = tracked_slots[i].member_identity_key;
            if (identity_key == 0) {
                continue;
            }
            if (!first) {
                out << ",";
            }
            first = false;
            const auto socket_id = static_cast<SRTSOCKET>(tracked_slots[i].member_id);
            const auto is_connected = tracked_slots[i].member_connected == 1;
            out << "slot" << (i + 1) << "[socket=" << socket_id
                << ",state=" << (is_connected ? "up" : "down") << "]";
        }
    }
    return out.str();
}

std::string BuildCompactResponseJson(const CompactResponse& response) {
    auto emit = [](const CompactResult& result) {
        return json{
            {"before_slots", result.before_slots},
            {"after_slots", result.after_slots},
            {"moved", result.moved},
            {"dropped", result.dropped},
        };
    };
    json out = {
        {"direction", response.direction},
    };
    if (response.include_input) {
        out["input"] = emit(response.input);
    }
    if (response.include_output) {
        out["output"] = emit(response.output);
    }
    return out.dump();
}

}  // namespace srtrelay
