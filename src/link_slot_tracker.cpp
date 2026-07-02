#include "srtrelay/metrics.hpp"

#include "metrics_link_slots.hpp"

namespace srtrelay {

std::string BuildInputLinkStatusCompact(const MetricsState& metrics) {
    return BuildLinkStatusCompact(LinkSide::kInput, metrics);
}

std::string BuildOutputLinkStatusCompact(const MetricsState& metrics) {
    return BuildLinkStatusCompact(LinkSide::kOutput, metrics);
}

void MarkAllTrackedInputLinksDisconnected(MetricsState* metrics) {
    MarkAllTrackedLinksDisconnected(LinkSide::kInput, metrics);
}

void MarkAllTrackedOutputLinksDisconnected(MetricsState* metrics) {
    MarkAllTrackedLinksDisconnected(LinkSide::kOutput, metrics);
}

void MaybeAutoCompactLinkSlots(const Config& cfg,
                               const Logger& logger,
                               MetricsState* metrics,
                               int64_t now_unix_ms) {
    if (metrics == nullptr || now_unix_ms <= 0) {
        return;
    }
    const bool delay_enabled = cfg.links_compact_disconnect_delay_ms >= 0;
    if (!delay_enabled) {
        return;
    }
    const auto side_enabled = [&](LinkSide side) -> bool {
        switch (cfg.links_compact_sides) {
            case AutoCompactSides::kBoth:
                return true;
            case AutoCompactSides::kInput:
                return side == LinkSide::kInput;
            case AutoCompactSides::kOutput:
                return side == LinkSide::kOutput;
        }
        return true;
    };

    struct TriggerOutcome {
        bool delay_triggered = false;
        CompactResult result {};
    };

    auto has_disconnected_slots = [&](LinkSide side) -> bool {
        const size_t capped = metrics->SnapshotCountCapped(side);
        if (side == LinkSide::kInput) {
            for (size_t i = 0; i < capped; ++i) {
                const auto& slot = metrics->input_tracked.slots[i];
                if (slot.member_identity_key == 0) {
                    continue;
                }
                const bool connected = slot.member_connected == 1;
                const auto socket_id = static_cast<SRTSOCKET>(slot.member_id);
                if (!connected || socket_id == SRT_INVALID_SOCK || socket_id == 0) {
                    return true;
                }
            }
        } else {
            for (size_t i = 0; i < capped; ++i) {
                const auto& slot = metrics->output_tracked.slots[i];
                if (slot.member_identity_key == 0) {
                    continue;
                }
                const bool connected = slot.member_connected == 1;
                const auto socket_id = static_cast<SRTSOCKET>(slot.member_id);
                if (!connected || socket_id == SRT_INVALID_SOCK || socket_id == 0) {
                    return true;
                }
            }
        }
        return false;
    };

    auto run_for_side = [&](LinkSide side) -> TriggerOutcome {
        TriggerOutcome outcome {};
        const size_t state_index = side == LinkSide::kInput ? 0 : 1;
        auto& runtime_state = metrics->auto_compact_state[state_index];
        if (!side_enabled(side)) {
            runtime_state.disconnect_deadline_unix_ms = 0;
            return outcome;
        }

        bool trigger_by_delay = false;
        const bool disconnected_present = has_disconnected_slots(side);
        if (delay_enabled) {
            if (!disconnected_present) {
                runtime_state.disconnect_deadline_unix_ms = 0;
            } else if (runtime_state.disconnect_deadline_unix_ms == 0) {
                runtime_state.disconnect_deadline_unix_ms =
                    now_unix_ms + static_cast<int64_t>(cfg.links_compact_disconnect_delay_ms);
            } else if (now_unix_ms >= runtime_state.disconnect_deadline_unix_ms) {
                trigger_by_delay = true;
                runtime_state.disconnect_deadline_unix_ms = 0;
            }
        }

        if (!trigger_by_delay) {
            return outcome;
        }

        outcome.delay_triggered = trigger_by_delay;
        outcome.result = CompactSlotsLocked(side, metrics);
        return outcome;
    };

    TriggerOutcome input_outcome;
    TriggerOutcome output_outcome;
    {
        MetricsState::LinkMetricsGuard lock(*metrics);
        input_outcome = run_for_side(LinkSide::kInput);
        output_outcome = run_for_side(LinkSide::kOutput);
    }

    auto maybe_log = [&](const char* side_name, const TriggerOutcome& outcome) {
        if (!outcome.delay_triggered) {
            return;
        }
        logger.Log(LogLevel::kInfo,
                   "metrics-links-auto-compacted",
                   std::string("side=") + side_name,
                   "trigger=delay",
                   "before=" + std::to_string(outcome.result.before_slots),
                   "after=" + std::to_string(outcome.result.after_slots),
                   "moved=" + std::to_string(outcome.result.moved),
                   "dropped=" + std::to_string(outcome.result.dropped));
    };
    maybe_log("input", input_outcome);
    maybe_log("output", output_outcome);
}

}  // namespace srtrelay
