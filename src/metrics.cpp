#include "srtrelay/metrics.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <unordered_set>

#include "srtrelay/srt_utils.hpp"

namespace srtrelay {

int64_t UnixNowMs() {
    const auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

MetricsState::MetricsState() {
    for (size_t i = 0; i < kMaxTrackedMembers; ++i) {
        input_member_ids[i].store(static_cast<int64_t>(SRT_INVALID_SOCK), std::memory_order_relaxed);
        input_member_connected[i].store(0, std::memory_order_relaxed);
        input_member_identity_keys[i].store(0, std::memory_order_relaxed);
        input_link_rx_bytes_total[i].store(0, std::memory_order_relaxed);
        input_link_tx_bytes_total[i].store(0, std::memory_order_relaxed);
        input_link_rx_bytes_current[i].store(0, std::memory_order_relaxed);
        input_link_tx_bytes_current[i].store(0, std::memory_order_relaxed);
        input_link_rx_bytes_last[i].store(0, std::memory_order_relaxed);
        input_link_tx_bytes_last[i].store(0, std::memory_order_relaxed);
        input_link_rtt_ms[i].store(-1, std::memory_order_relaxed);
    }
}

namespace {

void MaybeUpdateRttMetric(SRTSOCKET sock, std::atomic<int64_t>* out_rtt_ms) {
    if (sock == SRT_INVALID_SOCK) {
        out_rtt_ms->store(-1, std::memory_order_relaxed);
        return;
    }

    SRT_TRACEBSTATS sock_stats {};
    if (srt_bstats(sock, &sock_stats, 1) == SRT_ERROR) {
        return;
    }
    out_rtt_ms->store(static_cast<int64_t>(sock_stats.msRTT), std::memory_order_relaxed);
}

int ReadBondModeForSocket(SRTSOCKET sock) {
    if (sock == SRT_INVALID_SOCK) {
        return 0;
    }
    int group_type = static_cast<int>(SRT_GTYPE_UNDEFINED);
    int opt_len = sizeof(group_type);
    if (srt_getsockflag(sock, SRTO_GROUPTYPE, &group_type, &opt_len) == SRT_ERROR) {
        return 0;
    }
    if (group_type == static_cast<int>(SRT_GTYPE_BROADCAST)) {
        return 1;
    }
    if (group_type == static_cast<int>(SRT_GTYPE_BACKUP)) {
        return 2;
    }
    return 0;
}

void UpdateInputBondModeMetric(SRTSOCKET input_session_sock, MetricsState* metrics) {
    if (input_session_sock == SRT_INVALID_SOCK) {
        metrics->input_bond_mode.store(0, std::memory_order_relaxed);
        return;
    }
    int mode = ReadBondModeForSocket(input_session_sock);
    if (mode == 0) {
        const SRTSOCKET input_group_sock = srt_groupof(input_session_sock);
        mode = ReadBondModeForSocket(input_group_sock);
    }
    metrics->input_bond_mode.store(mode, std::memory_order_relaxed);
}

bool IsHealthyGroupMember(const SRT_SOCKGROUPDATA& member) {
    if (member.memberstate == SRT_GST_BROKEN) {
        return false;
    }
    return member.sockstate != SRTS_BROKEN &&
           member.sockstate != SRTS_CLOSING &&
           member.sockstate != SRTS_CLOSED &&
           member.sockstate != SRTS_NONEXIST;
}

bool IsConnectedGroupMember(const SRT_SOCKGROUPDATA& member) {
    if (member.memberstate == SRT_GST_RUNNING) {
        return true;
    }
    return member.sockstate == SRTS_CONNECTED;
}

bool HasUsablePeerAddress(const SRT_SOCKGROUPDATA& member) {
    return member.peeraddr.ss_family == AF_INET || member.peeraddr.ss_family == AF_INET6;
}

bool IsUsableGroupMemberSnapshot(const SRT_SOCKGROUPDATA& member) {
    if (member.id != SRT_INVALID_SOCK && member.id != 0) {
        return true;
    }
    if (HasUsablePeerAddress(member)) {
        return true;
    }
    if (member.token != 0) {
        return true;
    }
    return false;
}

bool TryGetPeerAddrForSocket(SRTSOCKET sock, sockaddr_storage* out_peer_addr) {
    if (sock == SRT_INVALID_SOCK || out_peer_addr == nullptr) {
        return false;
    }
    sockaddr_storage peer_addr {};
    int peer_len = static_cast<int>(sizeof(peer_addr));
    if (srt_getpeername(sock, reinterpret_cast<sockaddr*>(&peer_addr), &peer_len) == SRT_ERROR) {
        return false;
    }
    if (peer_len <= 0) {
        return false;
    }
    *out_peer_addr = peer_addr;
    return true;
}

uint64_t HashBytesFnv1a64(const void* data, size_t size, uint64_t seed = 1469598103934665603ULL) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint64_t hash = seed;
    for (size_t i = 0; i < size; ++i) {
        hash ^= static_cast<uint64_t>(bytes[i]);
        hash *= 1099511628211ULL;
    }
    return hash;
}

uint64_t InputMemberIdentityKey(const SRT_SOCKGROUPDATA& member) {
    const auto family = member.peeraddr.ss_family;
    if (family == AF_INET) {
        const auto* v4 = reinterpret_cast<const sockaddr_in*>(&member.peeraddr);
        const uint64_t hash = HashBytesFnv1a64(&(v4->sin_addr), sizeof(v4->sin_addr));
        return hash == 0 ? 1 : hash;
    }
    if (family == AF_INET6) {
        const auto* v6 = reinterpret_cast<const sockaddr_in6*>(&member.peeraddr);
        const uint64_t hash = HashBytesFnv1a64(&(v6->sin6_addr), sizeof(v6->sin6_addr));
        return hash == 0 ? 1 : hash;
    }
    if (member.id != SRT_INVALID_SOCK && member.id != 0) {
        return static_cast<uint64_t>(member.id);
    }
    if (member.token != 0) {
        return static_cast<uint64_t>(member.token);
    }
    return 0;
}

std::vector<SRT_SOCKGROUPDATA> BuildSingleSocketFallbackSnapshot(SRTSOCKET sock) {
    std::vector<SRT_SOCKGROUPDATA> members;
    if (sock == SRT_INVALID_SOCK) {
        return members;
    }

    SRT_SOCKGROUPDATA member {};
    member.id = sock;
    member.sockstate = SRTS_CONNECTED;
    member.memberstate = SRT_GST_RUNNING;
    if (!TryGetPeerAddrForSocket(sock, &member.peeraddr)) {
        member.peeraddr.ss_family = AF_UNSPEC;
    }
    members.push_back(member);
    return members;
}

bool TryReadGroupMembers(SRTSOCKET group_or_session_sock, std::vector<SRT_SOCKGROUPDATA>* out_members) {
    if (group_or_session_sock == SRT_INVALID_SOCK || out_members == nullptr) {
        return false;
    }
    SRT_SOCKGROUPDATA members[MetricsState::kMaxTrackedMembers] {};
    size_t capacity = MetricsState::kMaxTrackedMembers;
    const int sz = srt_group_data(group_or_session_sock, members, &capacity);
    if (sz <= 0) {
        return false;
    }
    out_members->clear();
    out_members->reserve(static_cast<size_t>(sz));
    for (int i = 0; i < sz; ++i) {
        if (!IsUsableGroupMemberSnapshot(members[i])) {
            continue;
        }
        out_members->push_back(members[i]);
    }
    return !out_members->empty();
}

void UpdateInputLinkHealthFromGroupMembers(const std::vector<SRT_SOCKGROUPDATA>& group_members,
                                           MetricsState* metrics) {
    int64_t healthy_count = 0;
    int64_t running_count = 0;
    for (const auto& member : group_members) {
        if (IsHealthyGroupMember(member)) {
            healthy_count++;
        }
        if (member.memberstate == SRT_GST_RUNNING || member.sockstate == SRTS_CONNECTED) {
            running_count++;
        }
    }
    metrics->input_links_total.store(static_cast<int64_t>(group_members.size()), std::memory_order_relaxed);
    metrics->input_links_healthy.store(healthy_count, std::memory_order_relaxed);
    metrics->input_links_running.store(running_count, std::memory_order_relaxed);
}

void SaveInputMemberSnapshot(const std::vector<SRT_SOCKGROUPDATA>& group_members, MetricsState* metrics);

void UpdateInputLinkHealthFallbackSingleSocket(SRTSOCKET input_session_sock, MetricsState* metrics) {
    if (input_session_sock == SRT_INVALID_SOCK) {
        metrics->input_links_total.store(0, std::memory_order_relaxed);
        metrics->input_links_healthy.store(0, std::memory_order_relaxed);
        metrics->input_links_running.store(0, std::memory_order_relaxed);
        MarkAllTrackedInputLinksDisconnected(metrics);
        for (size_t i = 0; i < MetricsState::kMaxTrackedMembers; ++i) {
            const auto key = metrics->input_member_identity_keys[i].load(std::memory_order_relaxed);
            if (key == 0) continue;
            metrics->input_member_ids[i].store(static_cast<int64_t>(SRT_INVALID_SOCK), std::memory_order_relaxed);
            metrics->input_member_connected[i].store(0, std::memory_order_relaxed);
            metrics->input_member_identity_keys[i].store(0, std::memory_order_relaxed);
            metrics->input_link_rx_bytes_current[i].store(0, std::memory_order_relaxed);
            metrics->input_link_tx_bytes_current[i].store(0, std::memory_order_relaxed);
            metrics->input_link_rx_bytes_last[i].store(0, std::memory_order_relaxed);
            metrics->input_link_tx_bytes_last[i].store(0, std::memory_order_relaxed);
            metrics->input_link_rx_bytes_total[i].store(0, std::memory_order_relaxed);
            metrics->input_link_tx_bytes_total[i].store(0, std::memory_order_relaxed);
        }
        metrics->input_links_snapshot_count.store(0, std::memory_order_relaxed);
        return;
    }
    auto members = BuildSingleSocketFallbackSnapshot(input_session_sock);
    UpdateInputLinkHealthFromGroupMembers(members, metrics);
    SaveInputMemberSnapshot(members, metrics);
}

void SaveInputMemberSnapshot(const std::vector<SRT_SOCKGROUPDATA>& group_members, MetricsState* metrics) {
    std::array<SRTSOCKET, MetricsState::kMaxTrackedMembers> slot_ids {};
    std::array<int, MetricsState::kMaxTrackedMembers> slot_connected {};
    std::array<uint64_t, MetricsState::kMaxTrackedMembers> slot_identity_keys {};
    for (size_t i = 0; i < MetricsState::kMaxTrackedMembers; ++i) {
        slot_ids[i] = static_cast<SRTSOCKET>(metrics->input_member_ids[i].load(std::memory_order_relaxed));
        slot_connected[i] = metrics->input_member_connected[i].load(std::memory_order_relaxed);
        slot_identity_keys[i] = metrics->input_member_identity_keys[i].load(std::memory_order_relaxed);
    }

    std::array<bool, MetricsState::kMaxTrackedMembers> slot_used {};
    std::array<int, MetricsState::kMaxTrackedMembers> member_slot {};
    member_slot.fill(-1);
    std::vector<bool> matched(group_members.size(), false);

    for (size_t i = 0; i < group_members.size(); ++i) {
        const uint64_t key = InputMemberIdentityKey(group_members[i]);
        if (key == 0) {
            continue;
        }
        for (size_t slot = 0; slot < MetricsState::kMaxTrackedMembers; ++slot) {
            if (slot_used[slot]) continue;
            if (slot_identity_keys[slot] != key) continue;
            member_slot[i] = static_cast<int>(slot);
            slot_used[slot] = true;
            matched[i] = true;
            break;
        }
    }

    for (size_t i = 0; i < group_members.size(); ++i) {
        if (matched[i]) continue;
        for (size_t slot = 0; slot < MetricsState::kMaxTrackedMembers; ++slot) {
            if (slot_used[slot]) continue;
            if (slot_identity_keys[slot] != 0) continue;
            member_slot[i] = static_cast<int>(slot);
            slot_used[slot] = true;
            matched[i] = true;
            break;
        }
    }

    for (size_t i = 0; i < group_members.size(); ++i) {
        if (matched[i]) continue;
        for (size_t slot = 0; slot < MetricsState::kMaxTrackedMembers; ++slot) {
            if (slot_used[slot]) continue;
            member_slot[i] = static_cast<int>(slot);
            slot_used[slot] = true;
            matched[i] = true;
            slot_ids[slot] = SRT_INVALID_SOCK;
            slot_connected[slot] = 0;
            slot_identity_keys[slot] = 0;
            metrics->input_link_rx_bytes_current[slot].store(0, std::memory_order_relaxed);
            metrics->input_link_tx_bytes_current[slot].store(0, std::memory_order_relaxed);
            metrics->input_link_rx_bytes_last[slot].store(0, std::memory_order_relaxed);
            metrics->input_link_tx_bytes_last[slot].store(0, std::memory_order_relaxed);
            metrics->input_link_rx_bytes_total[slot].store(0, std::memory_order_relaxed);
            metrics->input_link_tx_bytes_total[slot].store(0, std::memory_order_relaxed);
            break;
        }
    }

    std::array<bool, MetricsState::kMaxTrackedMembers> matched_slot {};
    for (size_t i = 0; i < group_members.size(); ++i) {
        const int slot = member_slot[i];
        if (slot < 0) continue;
        matched_slot[static_cast<size_t>(slot)] = true;
        const auto& member = group_members[i];
        slot_ids[slot] = member.id;
        slot_connected[slot] = IsConnectedGroupMember(member) ? 1 : 0;
        uint64_t identity_key = InputMemberIdentityKey(member);
        if (identity_key == 0) {
            identity_key = slot_identity_keys[slot];
        }
        slot_identity_keys[slot] = identity_key;
    }

    for (size_t slot = 0; slot < MetricsState::kMaxTrackedMembers; ++slot) {
        if (matched_slot[slot]) {
            continue;
        }
        if (slot_identity_keys[slot] == 0) {
            slot_ids[slot] = SRT_INVALID_SOCK;
            slot_connected[slot] = 0;
            continue;
        }
        slot_ids[slot] = SRT_INVALID_SOCK;
        slot_connected[slot] = 0;
    }

    size_t span = 0;
    for (size_t slot = 0; slot < MetricsState::kMaxTrackedMembers; ++slot) {
        metrics->input_member_ids[slot].store(static_cast<int64_t>(slot_ids[slot]), std::memory_order_relaxed);
        metrics->input_member_connected[slot].store(slot_connected[slot], std::memory_order_relaxed);
        metrics->input_member_identity_keys[slot].store(slot_identity_keys[slot], std::memory_order_relaxed);
        if (slot_identity_keys[slot] != 0) {
            span = slot + 1;
        }
    }
    metrics->input_links_snapshot_count.store(static_cast<int64_t>(span), std::memory_order_relaxed);
}

}  // namespace

std::string BuildInputLinkStatusCompact(const MetricsState& metrics) {
    int64_t count = metrics.input_links_snapshot_count.load(std::memory_order_relaxed);
    if (count < 0) count = 0;
    const size_t capped = std::min(static_cast<size_t>(count), MetricsState::kMaxTrackedMembers);
    std::ostringstream out;
    bool first = true;
    for (size_t i = 0; i < capped; ++i) {
        const auto identity_key = metrics.input_member_identity_keys[i].load(std::memory_order_relaxed);
        if (identity_key == 0) {
            continue;
        }
        if (!first) out << ",";
        first = false;
        const auto socket_id = static_cast<SRTSOCKET>(metrics.input_member_ids[i].load(std::memory_order_relaxed));
        const auto is_connected = metrics.input_member_connected[i].load(std::memory_order_relaxed) == 1;
        out << "slot" << (i + 1) << ":" << socket_id << ":" << (is_connected ? "up" : "down");
    }
    return out.str();
}

void MarkAllTrackedInputLinksDisconnected(MetricsState* metrics) {
    int64_t count = metrics->input_links_snapshot_count.load(std::memory_order_relaxed);
    if (count < 0) count = 0;
    const size_t capped = std::min(static_cast<size_t>(count), MetricsState::kMaxTrackedMembers);
    for (size_t i = 0; i < capped; ++i) {
        const auto identity_key = metrics->input_member_identity_keys[i].load(std::memory_order_relaxed);
        if (identity_key == 0) {
            continue;
        }
        metrics->input_member_connected[i].store(0, std::memory_order_relaxed);
    }
}

void ResetInputTrackingMetrics(MetricsState* metrics) {
    if (metrics == nullptr) return;
    metrics->input_links_total.store(0, std::memory_order_relaxed);
    metrics->input_links_healthy.store(0, std::memory_order_relaxed);
    metrics->input_links_running.store(0, std::memory_order_relaxed);
    MarkAllTrackedInputLinksDisconnected(metrics);
    metrics->input_transport_members_total.store(0, std::memory_order_relaxed);
}

namespace {

void UpdateInputLinkHealthMetrics(SRTSOCKET input_session_sock, MetricsState* metrics) {
    if (input_session_sock == SRT_INVALID_SOCK) {
        metrics->input_links_total.store(0, std::memory_order_relaxed);
        metrics->input_links_healthy.store(0, std::memory_order_relaxed);
        metrics->input_links_running.store(0, std::memory_order_relaxed);
        MarkAllTrackedInputLinksDisconnected(metrics);
        metrics->input_transport_members_total.store(0, std::memory_order_relaxed);
        return;
    }

    SRTSOCKET group_sock = srt_groupof(input_session_sock);
    if (group_sock == SRT_INVALID_SOCK) {
        group_sock = input_session_sock;
    }
    std::vector<SRT_SOCKGROUPDATA> group_members;
    if (!TryReadGroupMembers(group_sock, &group_members)) {
        if (!TryReadGroupMembers(input_session_sock, &group_members)) {
            UpdateInputLinkHealthFallbackSingleSocket(input_session_sock, metrics);
            return;
        }
    }

    if (group_members.empty()) {
        UpdateInputLinkHealthFallbackSingleSocket(input_session_sock, metrics);
        return;
    }
    UpdateInputLinkHealthFromGroupMembers(group_members, metrics);
    SaveInputMemberSnapshot(group_members, metrics);
}

}  // namespace

void UpdateInputLinkHealthFromMsgCtrl(const SRT_MSGCTRL& rx_ctrl, MetricsState* metrics) {
    if (rx_ctrl.grpdata == nullptr || rx_ctrl.grpdata_size <= 0) {
        return;
    }
    const size_t capped = std::min(static_cast<size_t>(rx_ctrl.grpdata_size), MetricsState::kMaxTrackedMembers);
    std::vector<SRT_SOCKGROUPDATA> group_members;
    group_members.reserve(capped);
    for (size_t i = 0; i < capped; ++i) {
        const auto& member = rx_ctrl.grpdata[i];
        if (!IsUsableGroupMemberSnapshot(member)) continue;
        group_members.push_back(member);
    }
    if (group_members.empty()) {
        return;
    }
    UpdateInputLinkHealthFromGroupMembers(group_members, metrics);
    SaveInputMemberSnapshot(group_members, metrics);
}

namespace {

std::vector<SRTSOCKET> GetInputMemberSocketsSnapshot(const MetricsState& metrics) {
    std::vector<SRTSOCKET> sockets;
    int64_t count = metrics.input_links_snapshot_count.load(std::memory_order_relaxed);
    if (count < 0) count = 0;
    const size_t capped = std::min(static_cast<size_t>(count), MetricsState::kMaxTrackedMembers);
    sockets.reserve(capped);
    for (size_t i = 0; i < capped; ++i) {
        if (metrics.input_member_connected[i].load(std::memory_order_relaxed) != 1) {
            continue;
        }
        const auto id = static_cast<SRTSOCKET>(metrics.input_member_ids[i].load(std::memory_order_relaxed));
        if (id == SRT_INVALID_SOCK || id == 0) {
            continue;
        }
        sockets.push_back(id);
    }
    return sockets;
}

uint64_t CounterDeltaWithReset(uint64_t prev, uint64_t curr) {
    if (curr >= prev) {
        return curr - prev;
    }
    return curr;
}

void UpdateInputLinkTrafficPerSlot(MetricsState* metrics) {
    int64_t count = metrics->input_links_snapshot_count.load(std::memory_order_relaxed);
    if (count < 0) count = 0;
    const size_t capped = std::min(static_cast<size_t>(count), MetricsState::kMaxTrackedMembers);

    for (size_t i = 0; i < capped; ++i) {
        const auto identity_key = metrics->input_member_identity_keys[i].load(std::memory_order_relaxed);
        if (identity_key == 0) {
            metrics->input_link_rx_bytes_current[i].store(0, std::memory_order_relaxed);
            metrics->input_link_tx_bytes_current[i].store(0, std::memory_order_relaxed);
            metrics->input_link_rtt_ms[i].store(-1, std::memory_order_relaxed);
            continue;
        }

        const auto member_sock = static_cast<SRTSOCKET>(metrics->input_member_ids[i].load(std::memory_order_relaxed));
        const auto member_connected = metrics->input_member_connected[i].load(std::memory_order_relaxed) == 1;
        if (!member_connected || member_sock == SRT_INVALID_SOCK || member_sock == 0) {
            metrics->input_link_rx_bytes_current[i].store(0, std::memory_order_relaxed);
            metrics->input_link_tx_bytes_current[i].store(0, std::memory_order_relaxed);
            metrics->input_link_rtt_ms[i].store(-1, std::memory_order_relaxed);
            continue;
        }

        SRT_TRACEBSTATS stats {};
        if (srt_bstats(member_sock, &stats, 1) == SRT_ERROR) {
            continue;
        }

        const auto rx_current = static_cast<uint64_t>(stats.byteRecvTotal);
        const auto tx_current = static_cast<uint64_t>(stats.byteSentTotal);
        const auto rx_last = metrics->input_link_rx_bytes_last[i].load(std::memory_order_relaxed);
        const auto tx_last = metrics->input_link_tx_bytes_last[i].load(std::memory_order_relaxed);
        const auto rx_total = metrics->input_link_rx_bytes_total[i].load(std::memory_order_relaxed) +
                              CounterDeltaWithReset(rx_last, rx_current);
        const auto tx_total = metrics->input_link_tx_bytes_total[i].load(std::memory_order_relaxed) +
                              CounterDeltaWithReset(tx_last, tx_current);
        const auto rtt_ms = static_cast<int64_t>(stats.msRTT);
        metrics->input_link_rx_bytes_total[i].store(rx_total, std::memory_order_relaxed);
        metrics->input_link_tx_bytes_total[i].store(tx_total, std::memory_order_relaxed);
        metrics->input_link_rx_bytes_current[i].store(rx_current, std::memory_order_relaxed);
        metrics->input_link_tx_bytes_current[i].store(tx_current, std::memory_order_relaxed);
        metrics->input_link_rx_bytes_last[i].store(rx_current, std::memory_order_relaxed);
        metrics->input_link_tx_bytes_last[i].store(tx_current, std::memory_order_relaxed);
        metrics->input_link_rtt_ms[i].store(rtt_ms, std::memory_order_relaxed);
    }
}

void UpdateTransportTrafficMetrics(SRTSOCKET output_sock, MetricsState* metrics) {
    UpdateInputLinkTrafficPerSlot(metrics);
    const auto member_sockets = GetInputMemberSocketsSnapshot(*metrics);
    std::unordered_set<SRTSOCKET> sockets_with_stats;
    sockets_with_stats.reserve(member_sockets.size());
    uint64_t input_byte_recv_total = metrics->input_transport_byte_recv_total.load(std::memory_order_relaxed);
    uint64_t input_byte_recv_unique_total = metrics->input_transport_byte_recv_unique_total.load(std::memory_order_relaxed);
    uint64_t input_byte_retrans_total = metrics->input_transport_byte_retrans_total.load(std::memory_order_relaxed);
    uint64_t input_byte_loss_total = metrics->input_transport_byte_loss_total.load(std::memory_order_relaxed);
    uint64_t input_byte_recv_current = 0;
    uint64_t input_byte_recv_unique_current = 0;
    uint64_t input_byte_retrans_current = 0;
    uint64_t input_byte_loss_current = 0;

    for (const auto member_sock : member_sockets) {
        SRT_TRACEBSTATS in_stats {};
        if (srt_bstats(member_sock, &in_stats, 1) == SRT_ERROR) {
            continue;
        }
        sockets_with_stats.insert(member_sock);

        input_byte_recv_current += static_cast<uint64_t>(in_stats.byteRecvTotal);
        input_byte_recv_unique_current += static_cast<uint64_t>(in_stats.byteRecvUniqueTotal);
        input_byte_retrans_current += static_cast<uint64_t>(in_stats.byteRetransTotal);
        input_byte_loss_current += static_cast<uint64_t>(in_stats.byteRcvLossTotal);

        auto& prev = metrics->input_transport_last_by_socket[member_sock];
        input_byte_recv_total += CounterDeltaWithReset(prev.byte_recv_total, static_cast<uint64_t>(in_stats.byteRecvTotal));
        input_byte_recv_unique_total += CounterDeltaWithReset(prev.byte_recv_unique_total, static_cast<uint64_t>(in_stats.byteRecvUniqueTotal));
        input_byte_retrans_total += CounterDeltaWithReset(prev.byte_retrans_total, static_cast<uint64_t>(in_stats.byteRetransTotal));
        input_byte_loss_total += CounterDeltaWithReset(prev.byte_loss_total, static_cast<uint64_t>(in_stats.byteRcvLossTotal));
        prev.byte_recv_total = static_cast<uint64_t>(in_stats.byteRecvTotal);
        prev.byte_recv_unique_total = static_cast<uint64_t>(in_stats.byteRecvUniqueTotal);
        prev.byte_retrans_total = static_cast<uint64_t>(in_stats.byteRetransTotal);
        prev.byte_loss_total = static_cast<uint64_t>(in_stats.byteRcvLossTotal);
    }

    std::unordered_map<SRTSOCKET, TransportCounterSnapshot> next_last;
    next_last.reserve(sockets_with_stats.size());
    for (const auto sock : sockets_with_stats) {
        next_last.emplace(sock, metrics->input_transport_last_by_socket[sock]);
    }
    metrics->input_transport_last_by_socket.swap(next_last);

    metrics->input_transport_members_total.store(static_cast<int64_t>(sockets_with_stats.size()), std::memory_order_relaxed);
    metrics->input_transport_byte_recv_total.store(input_byte_recv_total, std::memory_order_relaxed);
    metrics->input_transport_byte_recv_unique_total.store(input_byte_recv_unique_total, std::memory_order_relaxed);
    metrics->input_transport_byte_retrans_total.store(input_byte_retrans_total, std::memory_order_relaxed);
    metrics->input_transport_byte_loss_total.store(input_byte_loss_total, std::memory_order_relaxed);
    metrics->input_transport_byte_recv_current.store(input_byte_recv_current, std::memory_order_relaxed);
    metrics->input_transport_byte_recv_unique_current.store(input_byte_recv_unique_current, std::memory_order_relaxed);
    metrics->input_transport_byte_retrans_current.store(input_byte_retrans_current, std::memory_order_relaxed);
    metrics->input_transport_byte_loss_current.store(input_byte_loss_current, std::memory_order_relaxed);

    if (output_sock == SRT_INVALID_SOCK) {
        metrics->output_transport_last_socket = SRT_INVALID_SOCK;
        metrics->output_transport_byte_sent_current.store(0, std::memory_order_relaxed);
        metrics->output_transport_byte_sent_unique_current.store(0, std::memory_order_relaxed);
        metrics->output_transport_byte_retrans_current.store(0, std::memory_order_relaxed);
        metrics->output_transport_byte_drop_current.store(0, std::memory_order_relaxed);
        return;
    }

    SRT_TRACEBSTATS output_stats {};
    if (srt_bstats(output_sock, &output_stats, 1) == SRT_ERROR) {
        return;
    }

    uint64_t output_byte_sent_total = metrics->output_transport_byte_sent_total.load(std::memory_order_relaxed);
    uint64_t output_byte_sent_unique_total = metrics->output_transport_byte_sent_unique_total.load(std::memory_order_relaxed);
    uint64_t output_byte_retrans_total = metrics->output_transport_byte_retrans_total.load(std::memory_order_relaxed);
    uint64_t output_byte_drop_total = metrics->output_transport_byte_drop_total.load(std::memory_order_relaxed);

    if (metrics->output_transport_last_socket != output_sock) {
        metrics->output_transport_last_socket = output_sock;
        metrics->output_transport_last.byte_sent_total = output_stats.byteSentTotal;
        metrics->output_transport_last.byte_sent_unique_total = output_stats.byteSentUniqueTotal;
        metrics->output_transport_last.byte_retrans_total = output_stats.byteRetransTotal;
        metrics->output_transport_last.byte_drop_total = output_stats.byteSndDropTotal;
    } else {
        output_byte_sent_total += CounterDeltaWithReset(metrics->output_transport_last.byte_sent_total, output_stats.byteSentTotal);
        output_byte_sent_unique_total += CounterDeltaWithReset(metrics->output_transport_last.byte_sent_unique_total, output_stats.byteSentUniqueTotal);
        output_byte_retrans_total += CounterDeltaWithReset(metrics->output_transport_last.byte_retrans_total, output_stats.byteRetransTotal);
        output_byte_drop_total += CounterDeltaWithReset(metrics->output_transport_last.byte_drop_total, output_stats.byteSndDropTotal);
        metrics->output_transport_last.byte_sent_total = output_stats.byteSentTotal;
        metrics->output_transport_last.byte_sent_unique_total = output_stats.byteSentUniqueTotal;
        metrics->output_transport_last.byte_retrans_total = output_stats.byteRetransTotal;
        metrics->output_transport_last.byte_drop_total = output_stats.byteSndDropTotal;
    }

    metrics->output_transport_byte_sent_total.store(output_byte_sent_total, std::memory_order_relaxed);
    metrics->output_transport_byte_sent_unique_total.store(output_byte_sent_unique_total, std::memory_order_relaxed);
    metrics->output_transport_byte_retrans_total.store(output_byte_retrans_total, std::memory_order_relaxed);
    metrics->output_transport_byte_drop_total.store(output_byte_drop_total, std::memory_order_relaxed);
    metrics->output_transport_byte_sent_current.store(output_stats.byteSentTotal, std::memory_order_relaxed);
    metrics->output_transport_byte_sent_unique_current.store(output_stats.byteSentUniqueTotal, std::memory_order_relaxed);
    metrics->output_transport_byte_retrans_current.store(output_stats.byteRetransTotal, std::memory_order_relaxed);
    metrics->output_transport_byte_drop_current.store(output_stats.byteSndDropTotal, std::memory_order_relaxed);
}

}  // namespace

std::string RenderPrometheusMetrics(const MetricsState& metrics) {
    const auto total_rx_bytes = metrics.total_rx_bytes.load(std::memory_order_relaxed);
    const auto total_tx_bytes = metrics.total_tx_bytes.load(std::memory_order_relaxed);
    const auto total_rx_msgs = metrics.total_rx_msgs.load(std::memory_order_relaxed);
    const auto total_tx_msgs = metrics.total_tx_msgs.load(std::memory_order_relaxed);
    const auto total_send_failures = metrics.total_send_failures.load(std::memory_order_relaxed);
    const auto reconnect_count = metrics.reconnect_count.load(std::memory_order_relaxed);

    const auto rx_bytes_per_sec = metrics.rx_bytes_per_sec.load(std::memory_order_relaxed);
    const auto tx_bytes_per_sec = metrics.tx_bytes_per_sec.load(std::memory_order_relaxed);
    const auto rx_msgs_per_sec = metrics.rx_msgs_per_sec.load(std::memory_order_relaxed);
    const auto tx_msgs_per_sec = metrics.tx_msgs_per_sec.load(std::memory_order_relaxed);
    const auto interval_send_failures = metrics.interval_send_failures.load(std::memory_order_relaxed);

    const auto input_listening = metrics.input_listening.load(std::memory_order_relaxed);
    const auto input_connected = metrics.input_connected.load(std::memory_order_relaxed);
    const auto output_connected = metrics.output_connected.load(std::memory_order_relaxed);
    const auto input_links_total = metrics.input_links_total.load(std::memory_order_relaxed);
    const auto input_links_healthy = metrics.input_links_healthy.load(std::memory_order_relaxed);
    const auto input_links_running = metrics.input_links_running.load(std::memory_order_relaxed);
    int64_t input_links_snapshot_count = metrics.input_links_snapshot_count.load(std::memory_order_relaxed);
    if (input_links_snapshot_count < 0) {
        input_links_snapshot_count = 0;
    }
    const size_t input_links_snapshot_capped =
        std::min(static_cast<size_t>(input_links_snapshot_count), MetricsState::kMaxTrackedMembers);
    const auto input_transport_byte_recv_total = metrics.input_transport_byte_recv_total.load(std::memory_order_relaxed);
    const auto input_transport_byte_recv_unique_total = metrics.input_transport_byte_recv_unique_total.load(std::memory_order_relaxed);
    const auto input_transport_byte_retrans_total = metrics.input_transport_byte_retrans_total.load(std::memory_order_relaxed);
    const auto input_transport_byte_loss_total = metrics.input_transport_byte_loss_total.load(std::memory_order_relaxed);
    const auto input_transport_members_total = metrics.input_transport_members_total.load(std::memory_order_relaxed);
    const auto input_transport_byte_recv_current = metrics.input_transport_byte_recv_current.load(std::memory_order_relaxed);
    const auto input_transport_byte_recv_unique_current = metrics.input_transport_byte_recv_unique_current.load(std::memory_order_relaxed);
    const auto input_transport_byte_retrans_current = metrics.input_transport_byte_retrans_current.load(std::memory_order_relaxed);
    const auto input_transport_byte_loss_current = metrics.input_transport_byte_loss_current.load(std::memory_order_relaxed);
    const auto output_transport_byte_sent_total = metrics.output_transport_byte_sent_total.load(std::memory_order_relaxed);
    const auto output_transport_byte_sent_unique_total = metrics.output_transport_byte_sent_unique_total.load(std::memory_order_relaxed);
    const auto output_transport_byte_retrans_total = metrics.output_transport_byte_retrans_total.load(std::memory_order_relaxed);
    const auto output_transport_byte_drop_total = metrics.output_transport_byte_drop_total.load(std::memory_order_relaxed);
    const auto output_transport_byte_sent_current = metrics.output_transport_byte_sent_current.load(std::memory_order_relaxed);
    const auto output_transport_byte_sent_unique_current = metrics.output_transport_byte_sent_unique_current.load(std::memory_order_relaxed);
    const auto output_transport_byte_retrans_current = metrics.output_transport_byte_retrans_current.load(std::memory_order_relaxed);
    const auto output_transport_byte_drop_current = metrics.output_transport_byte_drop_current.load(std::memory_order_relaxed);
    const auto path_ready = (input_connected == 1 && output_connected == 1) ? 1 : 0;
    const auto input_bond_mode = metrics.input_bond_mode.load(std::memory_order_relaxed);

    const auto input_rtt_ms = metrics.input_rtt_ms.load(std::memory_order_relaxed);
    const auto output_rtt_ms = metrics.output_rtt_ms.load(std::memory_order_relaxed);
    const auto last_rx_unix_seconds = metrics.last_rx_unix_ms.load(std::memory_order_relaxed) / 1000;
    const auto last_tx_unix_seconds = metrics.last_tx_unix_ms.load(std::memory_order_relaxed) / 1000;

    std::ostringstream out;
    auto emit_u64 = [&](const char* name, const char* type, const char* help, uint64_t value) {
        out << "# HELP " << name << " " << help << "\n";
        out << "# TYPE " << name << " " << type << "\n";
        out << name << " " << value << "\n";
    };
    auto emit_i64 = [&](const char* name, const char* type, const char* help, int64_t value) {
        out << "# HELP " << name << " " << help << "\n";
        out << "# TYPE " << name << " " << type << "\n";
        out << name << " " << value << "\n";
    };

    emit_u64("srt_relay_rx_bytes_total", "counter", "Total bytes received by relay input.", total_rx_bytes);
    emit_u64("srt_relay_tx_bytes_total", "counter", "Total bytes sent by relay output.", total_tx_bytes);
    emit_u64("srt_relay_rx_messages_total", "counter", "Total messages received by relay input.", total_rx_msgs);
    emit_u64("srt_relay_tx_messages_total", "counter", "Total messages sent by relay output.", total_tx_msgs);
    emit_u64("srt_relay_send_failures_total", "counter", "Total failed output send attempts.", total_send_failures);
    emit_u64("srt_relay_reconnects_total", "counter", "Total reconnect attempts after output failures.", reconnect_count);

    emit_u64("srt_relay_rx_bytes_per_sec", "gauge", "Received bytes per second over last stats interval.", rx_bytes_per_sec);
    emit_u64("srt_relay_tx_bytes_per_sec", "gauge", "Sent bytes per second over last stats interval.", tx_bytes_per_sec);
    emit_u64("srt_relay_rx_messages_per_sec", "gauge", "Received messages per second over last stats interval.", rx_msgs_per_sec);
    emit_u64("srt_relay_tx_messages_per_sec", "gauge", "Sent messages per second over last stats interval.", tx_msgs_per_sec);
    emit_u64("srt_relay_send_failures_interval", "gauge", "Output send failures over last stats interval.", interval_send_failures);

    emit_i64("srt_relay_input_listening", "gauge", "Relay input listener is up (1/0).", input_listening);
    emit_i64("srt_relay_input_connected", "gauge", "Relay input session is connected (1/0).", input_connected);
    emit_i64("srt_relay_output_connected", "gauge", "Relay output session is connected (1/0).", output_connected);
    emit_i64("srt_relay_path_ready", "gauge", "Relay data path input+output connected (1/0).", path_ready);
    emit_i64("srt_relay_input_links_total", "gauge", "Total number of input links in the active SRT group.", input_links_total);
    emit_i64("srt_relay_input_links_healthy", "gauge", "Number of healthy input links in the active SRT group.", input_links_healthy);
    emit_i64("srt_relay_input_links_running", "gauge", "Number of currently running/active input links in the active SRT group.", input_links_running);

    auto emit_input_link_metric_u64 = [&](const char* name,
                                          const char* type,
                                          const char* help,
                                          const std::array<std::atomic<uint64_t>, MetricsState::kMaxTrackedMembers>& values) {
        out << "# HELP " << name << " " << help << "\n";
        out << "# TYPE " << name << " " << type << "\n";
        for (size_t i = 0; i < input_links_snapshot_capped; ++i) {
            const auto identity_key = metrics.input_member_identity_keys[i].load(std::memory_order_relaxed);
            if (identity_key == 0) {
                continue;
            }
            const auto socket_id = static_cast<SRTSOCKET>(metrics.input_member_ids[i].load(std::memory_order_relaxed));
            out << name << "{link_index=\"" << (i + 1)
                << "\",socket_id=\"" << socket_id << "\"} "
                << values[i].load(std::memory_order_relaxed) << "\n";
        }
    };
    auto emit_input_link_metric_i64 = [&](const char* name,
                                          const char* type,
                                          const char* help,
                                          const std::array<std::atomic<int64_t>, MetricsState::kMaxTrackedMembers>& values) {
        out << "# HELP " << name << " " << help << "\n";
        out << "# TYPE " << name << " " << type << "\n";
        for (size_t i = 0; i < input_links_snapshot_capped; ++i) {
            const auto identity_key = metrics.input_member_identity_keys[i].load(std::memory_order_relaxed);
            if (identity_key == 0) {
                continue;
            }
            const auto socket_id = static_cast<SRTSOCKET>(metrics.input_member_ids[i].load(std::memory_order_relaxed));
            out << name << "{link_index=\"" << (i + 1)
                << "\",socket_id=\"" << socket_id << "\"} "
                << values[i].load(std::memory_order_relaxed) << "\n";
        }
    };
    auto emit_input_link_metric_i32 = [&](const char* name,
                                          const char* type,
                                          const char* help,
                                          const std::array<std::atomic<int>, MetricsState::kMaxTrackedMembers>& values) {
        out << "# HELP " << name << " " << help << "\n";
        out << "# TYPE " << name << " " << type << "\n";
        for (size_t i = 0; i < input_links_snapshot_capped; ++i) {
            const auto identity_key = metrics.input_member_identity_keys[i].load(std::memory_order_relaxed);
            if (identity_key == 0) {
                continue;
            }
            const auto socket_id = static_cast<SRTSOCKET>(metrics.input_member_ids[i].load(std::memory_order_relaxed));
            out << name << "{link_index=\"" << (i + 1)
                << "\",socket_id=\"" << socket_id << "\"} "
                << values[i].load(std::memory_order_relaxed) << "\n";
        }
    };

    emit_input_link_metric_i32("srt_relay_input_link_connected", "gauge",
                               "Per-input-link connection state in the active SRT group (1/0).",
                               metrics.input_member_connected);
    emit_input_link_metric_u64("srt_relay_input_link_rx_bytes_total", "counter",
                               "Monotonic per-link transport RX bytes for each stable input link slot.",
                               metrics.input_link_rx_bytes_total);
    emit_input_link_metric_u64("srt_relay_input_link_tx_bytes_total", "counter",
                               "Monotonic per-link transport TX bytes for each stable input link slot.",
                               metrics.input_link_tx_bytes_total);
    emit_input_link_metric_u64("srt_relay_input_link_rx_bytes_current", "gauge",
                               "Current per-link byteRecvTotal for each stable input link slot.",
                               metrics.input_link_rx_bytes_current);
    emit_input_link_metric_u64("srt_relay_input_link_tx_bytes_current", "gauge",
                               "Current per-link byteSentTotal for each stable input link slot.",
                               metrics.input_link_tx_bytes_current);
    emit_input_link_metric_i64("srt_relay_input_link_rtt_ms", "gauge",
                               "Per-link RTT in milliseconds for each stable input link slot.",
                               metrics.input_link_rtt_ms);

    emit_u64("srt_relay_input_transport_byte_recv_total", "counter", "Monotonic input transport bytes received across tracked SRT member sockets (includes duplicate traffic).", input_transport_byte_recv_total);
    emit_u64("srt_relay_input_transport_byte_recv_unique_total", "counter", "Monotonic input transport unique bytes received across tracked SRT member sockets.", input_transport_byte_recv_unique_total);
    emit_u64("srt_relay_input_transport_byte_retrans_total", "counter", "Monotonic input transport retransmitted bytes across tracked SRT member sockets.", input_transport_byte_retrans_total);
    emit_u64("srt_relay_input_transport_byte_loss_total", "counter", "Monotonic input transport reported lost bytes across tracked SRT member sockets.", input_transport_byte_loss_total);
    emit_i64("srt_relay_input_transport_members_tracked", "gauge", "Number of input SRT member sockets tracked for transport metrics.", input_transport_members_total);
    emit_u64("srt_relay_input_transport_byte_recv_current", "gauge", "Current summed byteRecvTotal across tracked SRT member sockets.", input_transport_byte_recv_current);
    emit_u64("srt_relay_input_transport_byte_recv_unique_current", "gauge", "Current summed byteRecvUniqueTotal across tracked SRT member sockets.", input_transport_byte_recv_unique_current);
    emit_u64("srt_relay_input_transport_byte_retrans_current", "gauge", "Current summed byteRetransTotal across tracked SRT member sockets.", input_transport_byte_retrans_current);
    emit_u64("srt_relay_input_transport_byte_loss_current", "gauge", "Current summed byteRcvLossTotal across tracked SRT member sockets.", input_transport_byte_loss_current);

    emit_u64("srt_relay_output_transport_byte_sent_total", "counter", "Monotonic output transport bytes sent on relay output SRT socket (includes retransmissions).", output_transport_byte_sent_total);
    emit_u64("srt_relay_output_transport_byte_sent_unique_total", "counter", "Monotonic output transport unique bytes sent on relay output SRT socket.", output_transport_byte_sent_unique_total);
    emit_u64("srt_relay_output_transport_byte_retrans_total", "counter", "Monotonic output transport retransmitted bytes on relay output SRT socket.", output_transport_byte_retrans_total);
    emit_u64("srt_relay_output_transport_byte_drop_total", "counter", "Monotonic output transport dropped bytes on relay output SRT socket.", output_transport_byte_drop_total);
    emit_u64("srt_relay_output_transport_byte_sent_current", "gauge", "Current byteSentTotal on relay output SRT socket.", output_transport_byte_sent_current);
    emit_u64("srt_relay_output_transport_byte_sent_unique_current", "gauge", "Current byteSentUniqueTotal on relay output SRT socket.", output_transport_byte_sent_unique_current);
    emit_u64("srt_relay_output_transport_byte_retrans_current", "gauge", "Current byteRetransTotal on relay output SRT socket.", output_transport_byte_retrans_current);
    emit_u64("srt_relay_output_transport_byte_drop_current", "gauge", "Current byteSndDropTotal on relay output SRT socket.", output_transport_byte_drop_current);

    emit_i64("srt_relay_input_rtt_ms", "gauge", "Input socket RTT in milliseconds.", input_rtt_ms);
    emit_i64("srt_relay_output_rtt_ms", "gauge", "Output socket RTT in milliseconds.", output_rtt_ms);
    out << "# HELP srt_relay_input_bond_mode Input bonded mode for active input session (1 for current mode, 0 otherwise).\n";
    out << "# TYPE srt_relay_input_bond_mode gauge\n";
    out << "srt_relay_input_bond_mode{mode=\"unknown\"} " << (input_bond_mode == 0 ? 1 : 0) << "\n";
    out << "srt_relay_input_bond_mode{mode=\"broadcast\"} " << (input_bond_mode == 1 ? 1 : 0) << "\n";
    out << "srt_relay_input_bond_mode{mode=\"backup\"} " << (input_bond_mode == 2 ? 1 : 0) << "\n";
    emit_i64("srt_relay_last_rx_unix_seconds", "gauge", "Unix timestamp of last received packet.", last_rx_unix_seconds);
    emit_i64("srt_relay_last_tx_unix_seconds", "gauge", "Unix timestamp of last forwarded packet.", last_tx_unix_seconds);

    return out.str();
}

MetricsServer::MetricsServer(const Config& cfg, const Logger& logger, const MetricsState& metrics)
    : cfg_(cfg), logger_(logger), metrics_(metrics) {}

MetricsServer::~MetricsServer() { Stop(); }

void MetricsServer::Start() {
    if (!cfg_.metrics_enabled) {
        return;
    }
    server_.Get("/metrics", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content(RenderPrometheusMetrics(metrics_),
                        "text/plain; version=0.0.4; charset=utf-8");
    });
    server_.Get("/healthz", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content("ok\n", "text/plain");
    });

    thread_ = std::thread([this]() {
        if (!server_.bind_to_port(cfg_.metrics_host, cfg_.metrics_port)) {
            logger_.Log(LogLevel::kError,
                        "metrics-bind-failed",
                        "host=" + cfg_.metrics_host,
                        "port=" + std::to_string(cfg_.metrics_port));
            return;
        }
        logger_.Log(LogLevel::kInfo,
                    "metrics-listening",
                    "host=" + cfg_.metrics_host,
                    "port=" + std::to_string(cfg_.metrics_port));
        server_.listen_after_bind();
        logger_.Log(LogLevel::kInfo, "metrics-stopped");
    });
}

void MetricsServer::Stop() {
    if (!cfg_.metrics_enabled) {
        return;
    }
    server_.stop();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void MaybeLogStats(const Config& cfg,
                   const Logger& logger,
                   RelayStats* stats,
                   const RelayState& state,
                   MetricsState* metrics,
                   SRTSOCKET input_session_sock,
                   SRTSOCKET output_sock,
                   OutputMetricsMode output_metrics_mode,
                   std::chrono::steady_clock::time_point* last_stats_at) {
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - *last_stats_at).count();
    if (elapsed_ms < cfg.stats_interval_ms) {
        return;
    }
    const double sec = static_cast<double>(elapsed_ms) / 1000.0;
    const auto rx_bps = static_cast<uint64_t>(stats->interval_rx_bytes / sec);
    const auto tx_bps = static_cast<uint64_t>(stats->interval_tx_bytes / sec);
    const auto rx_mps = static_cast<uint64_t>(stats->interval_rx_msgs / sec);
    const auto tx_mps = static_cast<uint64_t>(stats->interval_tx_msgs / sec);
    MaybeUpdateRttMetric(input_session_sock, &metrics->input_rtt_ms);
    if (output_metrics_mode == OutputMetricsMode::kSrtSocket) {
        MaybeUpdateRttMetric(output_sock, &metrics->output_rtt_ms);
    } else {
        metrics->output_rtt_ms.store(0, std::memory_order_relaxed);
    }
    UpdateInputBondModeMetric(input_session_sock, metrics);
    UpdateInputLinkHealthMetrics(input_session_sock, metrics);
    UpdateTransportTrafficMetrics(output_metrics_mode == OutputMetricsMode::kSrtSocket ? output_sock : SRT_INVALID_SOCK,
                                  metrics);
    const auto input_rtt_ms = metrics->input_rtt_ms.load(std::memory_order_relaxed);
    const auto output_rtt_ms = metrics->output_rtt_ms.load(std::memory_order_relaxed);
    const auto input_links_total = metrics->input_links_total.load(std::memory_order_relaxed);
    const auto input_links_healthy = metrics->input_links_healthy.load(std::memory_order_relaxed);
    const auto input_links_running = metrics->input_links_running.load(std::memory_order_relaxed);
    const auto input_transport_byte_recv_total = metrics->input_transport_byte_recv_total.load(std::memory_order_relaxed);
    const auto output_transport_byte_sent_total = metrics->output_transport_byte_sent_total.load(std::memory_order_relaxed);
    const auto input_bond_mode = metrics->input_bond_mode.load(std::memory_order_relaxed);
    const char* input_bond_mode_name = "unknown";
    if (input_bond_mode == 1) {
        input_bond_mode_name = "broadcast";
    } else if (input_bond_mode == 2) {
        input_bond_mode_name = "backup";
    }
    const auto input_link_status = BuildInputLinkStatusCompact(*metrics);

    metrics->rx_bytes_per_sec.store(rx_bps, std::memory_order_relaxed);
    metrics->tx_bytes_per_sec.store(tx_bps, std::memory_order_relaxed);
    metrics->rx_msgs_per_sec.store(rx_mps, std::memory_order_relaxed);
    metrics->tx_msgs_per_sec.store(tx_mps, std::memory_order_relaxed);
    metrics->interval_send_failures.store(stats->interval_send_failures, std::memory_order_relaxed);

    logger.Log(LogLevel::kInfo,
               "stats",
               "rx_bytes_per_sec=" + std::to_string(rx_bps),
               "tx_bytes_per_sec=" + std::to_string(tx_bps),
               "rx_msgs_per_sec=" + std::to_string(rx_mps),
               "tx_msgs_per_sec=" + std::to_string(tx_mps),
               "send_failures=" + std::to_string(stats->interval_send_failures),
               "reconnect_count=" + std::to_string(stats->reconnect_count),
               "input_rtt_ms=" + std::to_string(input_rtt_ms),
               "output_rtt_ms=" + std::to_string(output_rtt_ms),
               "input_links_total=" + std::to_string(input_links_total),
               "input_links_healthy=" + std::to_string(input_links_healthy),
               "input_links_running=" + std::to_string(input_links_running),
               "input_link_status=" + input_link_status,
               "input_transport_byte_recv_total=" + std::to_string(input_transport_byte_recv_total),
               "output_transport_byte_sent_total=" + std::to_string(output_transport_byte_sent_total),
               std::string("input_bond_mode=") + input_bond_mode_name,
               std::string("input_state=") + (state.input_connected ? "connected" : (state.input_listening ? "listening" : "down")),
               std::string("output_state=") + (state.output_connected ? "connected" : "down"));

    stats->interval_rx_bytes = 0;
    stats->interval_tx_bytes = 0;
    stats->interval_rx_msgs = 0;
    stats->interval_tx_msgs = 0;
    stats->interval_send_failures = 0;
    *last_stats_at = now;
}

}  // namespace srtrelay
