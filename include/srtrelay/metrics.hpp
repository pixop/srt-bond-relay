#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <srt.h>

#include "srtrelay/causality.hpp"
#include "srtrelay/config.hpp"
#include "srtrelay/logger.hpp"
#include <httplib.h>

namespace srtrelay {

struct InputEndpointSpec;
struct OutputEndpointSpec;

struct RelayStats {
    uint64_t total_rx_bytes = 0;
    uint64_t total_tx_bytes = 0;
    uint64_t total_rx_msgs = 0;
    uint64_t total_tx_msgs = 0;
    uint64_t total_send_failures = 0;
    uint64_t reconnect_count = 0;

    uint64_t interval_rx_bytes = 0;
    uint64_t interval_tx_bytes = 0;
    uint64_t interval_rx_msgs = 0;
    uint64_t interval_tx_msgs = 0;
    uint64_t interval_send_failures = 0;
};

struct RelayState {
    bool input_listening = false;
    bool input_connected = false;
    bool output_connected = false;
};

int64_t UnixNowMs();

struct TransportCounterSnapshot {
    uint64_t byte_recv_total = 0;
    uint64_t byte_recv_unique_total = 0;
    uint64_t byte_retrans_total = 0;
    uint64_t byte_loss_total = 0;
    uint64_t packet_belated_total = 0;
    uint64_t byte_sent_total = 0;
    uint64_t byte_sent_unique_total = 0;
    uint64_t byte_drop_total = 0;
};

struct GroupDropCounterSnapshot {
    uint64_t packet_drop_total = 0;
    uint64_t byte_drop_total = 0;
};

struct MetricsState {
    static constexpr size_t kMaxTrackedMembers = 16;
    static constexpr size_t kMaxInputSources = 16;

    std::atomic<uint64_t> total_rx_bytes{0};
    std::atomic<uint64_t> total_tx_bytes{0};
    std::atomic<uint64_t> total_rx_msgs{0};
    std::atomic<uint64_t> total_tx_msgs{0};
    std::atomic<uint64_t> total_send_failures{0};
    std::atomic<uint64_t> reconnect_count{0};

    std::atomic<uint64_t> rx_bytes_per_sec{0};
    std::atomic<uint64_t> tx_bytes_per_sec{0};
    std::atomic<uint64_t> rx_msgs_per_sec{0};
    std::atomic<uint64_t> tx_msgs_per_sec{0};
    std::atomic<uint64_t> interval_send_failures{0};

    std::atomic<int> input_listening{0};
    std::atomic<int> input_connected{0};
    std::atomic<int> output_connected{0};
    std::atomic<int64_t> input_links_total{0};
    std::atomic<int64_t> input_links_healthy{0};
    std::atomic<int64_t> input_links_running{0};
    std::atomic<int64_t> input_links_snapshot_count{0};
    std::atomic<int64_t> output_links_total{0};
    std::atomic<int64_t> output_links_healthy{0};
    std::atomic<int64_t> output_links_running{0};
    std::atomic<int64_t> output_links_snapshot_count{0};

    std::atomic<uint64_t> input_transport_byte_recv_total{0};
    std::atomic<uint64_t> input_transport_byte_recv_unique_total{0};
    std::atomic<uint64_t> input_transport_byte_retrans_total{0};
    std::atomic<uint64_t> input_transport_byte_loss_total{0};
    std::atomic<uint64_t> input_transport_packet_belated_total{0};
    std::atomic<int64_t> input_transport_members_total{0};
    std::atomic<uint64_t> input_transport_byte_recv_current{0};
    std::atomic<uint64_t> input_transport_byte_recv_unique_current{0};
    std::atomic<uint64_t> input_transport_byte_retrans_current{0};
    std::atomic<uint64_t> input_transport_byte_loss_current{0};
    std::atomic<uint64_t> input_transport_packet_belated_current{0};
    std::atomic<uint64_t> input_group_packet_drop_total{0};
    std::atomic<uint64_t> input_group_byte_drop_total{0};
    std::atomic<int64_t> input_group_drop_sockets_tracked{0};
    std::atomic<uint64_t> input_group_packet_drop_current{0};
    std::atomic<uint64_t> input_group_byte_drop_current{0};
    std::array<std::atomic<uint64_t>, kMaxTrackedMembers> input_link_rx_bytes_total {};
    std::array<std::atomic<uint64_t>, kMaxTrackedMembers> input_link_tx_bytes_total {};
    std::array<std::atomic<uint64_t>, kMaxTrackedMembers> input_link_rx_bytes_current {};
    std::array<std::atomic<uint64_t>, kMaxTrackedMembers> input_link_tx_bytes_current {};
    std::array<std::atomic<uint64_t>, kMaxTrackedMembers> input_link_rx_bytes_last {};
    std::array<std::atomic<uint64_t>, kMaxTrackedMembers> input_link_tx_bytes_last {};
    std::array<std::atomic<uint64_t>, kMaxTrackedMembers> input_link_packet_belated_total {};
    std::array<std::atomic<uint64_t>, kMaxTrackedMembers> input_link_packet_belated_current {};
    std::array<std::atomic<uint64_t>, kMaxTrackedMembers> input_link_packet_belated_last {};
    std::array<std::atomic<int64_t>, kMaxTrackedMembers> input_link_rtt_ms {};

    std::atomic<uint64_t> output_transport_byte_sent_total{0};
    std::atomic<uint64_t> output_transport_byte_sent_unique_total{0};
    std::atomic<uint64_t> output_transport_byte_retrans_total{0};
    std::atomic<uint64_t> output_transport_byte_drop_total{0};
    std::atomic<int64_t> output_transport_members_total{0};
    std::atomic<uint64_t> output_transport_byte_sent_current{0};
    std::atomic<uint64_t> output_transport_byte_sent_unique_current{0};
    std::atomic<uint64_t> output_transport_byte_retrans_current{0};
    std::atomic<uint64_t> output_transport_byte_drop_current{0};

    std::atomic<int64_t> input_rtt_ms{-1};
    std::atomic<int64_t> output_rtt_ms{-1};
    std::atomic<int64_t> input_effective_latency_ms{-1};
    std::atomic<int64_t> output_effective_latency_ms{-1};
    std::atomic<int> input_bond_mode{0};  // 0=unknown, 1=broadcast, 2=backup
    std::atomic<int> output_bond_mode{0};  // 0=unknown, 1=broadcast, 2=backup
    std::atomic<int64_t> input_sources_total{1};
    std::atomic<int64_t> active_input_index{1};  // 1-based
    std::atomic<uint64_t> input_switches_total{0};
    std::atomic<int64_t> primary_input_index{0};  // 0 means not set
    std::atomic<int> switch_policy{0};            // 0=round_robin, 1=preferred_primary
    std::atomic<int> switch_mode{0};              // 0=serial, 1=delayed
    std::array<std::atomic<int>, kMaxInputSources> input_source_connected {};
    std::array<std::atomic<int>, kMaxInputSources> input_source_listening {};
    std::array<std::atomic<int>, kMaxInputSources> input_source_bond_mode {};  // 0=unknown,1=broadcast,2=backup

    std::atomic<int64_t> last_rx_unix_ms{0};
    std::atomic<int64_t> last_tx_unix_ms{0};
    std::atomic<int64_t> input_session_socket_id{static_cast<int64_t>(SRT_INVALID_SOCK)};
    std::atomic<int64_t> output_transport_socket_id{static_cast<int64_t>(SRT_INVALID_SOCK)};

    std::array<std::atomic<int64_t>, kMaxTrackedMembers> input_member_ids {};
    std::array<std::atomic<int>, kMaxTrackedMembers> input_member_connected {};
    std::array<std::atomic<uint64_t>, kMaxTrackedMembers> input_member_identity_keys {};
    std::array<std::atomic<int64_t>, kMaxTrackedMembers> output_member_ids {};
    std::array<std::atomic<int>, kMaxTrackedMembers> output_member_connected {};
    std::array<std::atomic<uint64_t>, kMaxTrackedMembers> output_member_identity_keys {};
    std::array<std::atomic<uint64_t>, kMaxTrackedMembers> output_link_rx_bytes_total {};
    std::array<std::atomic<uint64_t>, kMaxTrackedMembers> output_link_tx_bytes_total {};
    std::array<std::atomic<uint64_t>, kMaxTrackedMembers> output_link_rx_bytes_current {};
    std::array<std::atomic<uint64_t>, kMaxTrackedMembers> output_link_tx_bytes_current {};
    std::array<std::atomic<uint64_t>, kMaxTrackedMembers> output_link_rx_bytes_last {};
    std::array<std::atomic<uint64_t>, kMaxTrackedMembers> output_link_tx_bytes_last {};
    std::array<std::atomic<int64_t>, kMaxTrackedMembers> output_link_rtt_ms {};
    std::array<std::atomic<int>, kMaxTrackedMembers> input_member_sock_state {};
    std::array<std::atomic<int>, kMaxTrackedMembers> input_member_group_state {};
    std::array<std::atomic<int>, kMaxTrackedMembers> input_member_peer_port {};
    std::array<std::atomic<int>, kMaxTrackedMembers> output_member_sock_state {};
    std::array<std::atomic<int>, kMaxTrackedMembers> output_member_group_state {};
    std::array<std::atomic<int>, kMaxTrackedMembers> output_member_peer_port {};

    std::unordered_map<SRTSOCKET, TransportCounterSnapshot> input_transport_last_by_socket;
    std::unordered_map<SRTSOCKET, TransportCounterSnapshot> output_transport_last_by_socket;
    std::unordered_map<SRTSOCKET, GroupDropCounterSnapshot> input_group_drop_last_by_socket;

    std::array<std::array<std::atomic<uint64_t>, kTimeoutTypes>, kFailureSides> timeouts_total {};
    std::array<std::array<std::atomic<uint64_t>, kReasonCodes>, kFailureSides> disconnects_total {};
    std::array<std::atomic<uint64_t>, kFailureSides> reconnect_attempts_total {};
    std::atomic<int64_t> last_input_failure_unix_seconds{0};
    std::atomic<int64_t> last_output_failure_unix_seconds{0};
    std::atomic<int> incident_active{0};
    std::atomic<int64_t> incident_open_unix_ms{0};

    std::array<std::atomic<uint64_t>, kFailureSides> active_attempt_id {};
    std::string active_incident_id;
    LastFailureSnapshot input_last_failure;
    LastFailureSnapshot output_last_failure;
    std::array<std::string, kMaxTrackedMembers> input_member_peer_host {};
    std::array<std::string, kMaxTrackedMembers> output_member_peer_host {};
    std::array<int, kMaxTrackedMembers> input_member_status_last_logged {};
    std::array<int, kMaxTrackedMembers> output_member_status_last_logged {};
    std::array<int, kMaxTrackedMembers> input_member_connected_last_logged {};
    std::array<int, kMaxTrackedMembers> output_member_connected_last_logged {};

    mutable std::mutex link_metrics_mutex;
    mutable std::mutex causality_mutex;

    MetricsState();
};

enum class OutputMetricsMode {
    kSrtSocket,
    kStdout,
};

class MetricsServer {
public:
    MetricsServer(const Config& cfg,
                  const Logger& logger,
                  MetricsState& metrics,
                  const std::vector<InputEndpointSpec>& input_specs,
                  const OutputEndpointSpec& output_spec);
    ~MetricsServer();

    void Start();
    void Stop();

private:
    const Config& cfg_;
    const Logger& logger_;
    MetricsState& metrics_;
    const std::vector<InputEndpointSpec>* input_specs_ = nullptr;
    const OutputEndpointSpec* output_spec_ = nullptr;
    httplib::Server server_;
    std::thread thread_;
};

std::string RenderPrometheusMetrics(const MetricsState& metrics);
std::string BuildInputLinkStatusCompact(const MetricsState& metrics);
std::string BuildOutputLinkStatusCompact(const MetricsState& metrics);
void MarkAllTrackedInputLinksDisconnected(MetricsState* metrics);
void MarkAllTrackedOutputLinksDisconnected(MetricsState* metrics);
void UpdateInputLinkHealthFromMsgCtrl(const SRT_MSGCTRL& rx_ctrl, MetricsState* metrics);
void UpdateOutputLinkHealthFromMsgCtrl(const SRT_MSGCTRL& tx_ctrl, MetricsState* metrics);
void ResetInputTrackingMetrics(MetricsState* metrics);
void ResetOutputTrackingMetrics(MetricsState* metrics);

void MaybeLogStats(const Config& cfg,
                   const Logger& logger,
                   RelayStats* stats,
                   const RelayState& state,
                   MetricsState* metrics,
                   SRTSOCKET input_session_sock,
                   SRTSOCKET output_sock,
                   OutputMetricsMode output_metrics_mode,
                   const std::string& active_incident_id,
                   std::chrono::steady_clock::time_point* last_stats_at);

}  // namespace srtrelay
