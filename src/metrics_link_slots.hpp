#pragma once

#include <string>
#include <vector>

#include <srt.h>

#include "srtrelay/metrics.hpp"

namespace srtrelay {

struct CompactResult {
    size_t before_slots = 0;
    size_t after_slots = 0;
    size_t moved = 0;
    size_t dropped = 0;
};

struct CompactResponse {
    std::string direction;
    bool include_input = false;
    bool include_output = false;
    CompactResult input;
    CompactResult output;
};

void SaveMemberSnapshot(LinkSide side,
                        const std::vector<SRT_SOCKGROUPDATA>& group_members,
                        MetricsState* metrics);
void ClearTrackedMembersForDisconnectedSocket(LinkSide side, MetricsState* metrics);
void MarkAllTrackedLinksDisconnected(LinkSide side, MetricsState* metrics);
// Caller must hold MetricsState::LinkMetricsGuard for metrics->link_metrics_mutex.
CompactResult CompactSlotsLocked(LinkSide side, MetricsState* metrics);
std::string BuildLinkStatusCompact(LinkSide side, const MetricsState& metrics);
std::string BuildCompactResponseJson(const CompactResponse& response);

}  // namespace srtrelay
