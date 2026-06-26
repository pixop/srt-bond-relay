#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace srtrelay {

enum class FailureSide {
    kInput = 0,
    kOutput = 1,
};

enum class ReasonClass {
    kTimeout = 0,
    kDisconnect = 1,
    kConnectFailure = 2,
    kIoFailure = 3,
    kProtocolState = 4,
    kInternal = 5,
};

enum class ReasonCode {
    kConnectTimeout = 0,
    kReceiveTimeout = 1,
    kSendTimeout = 2,
    kAcceptTimeout = 3,
    kConnectFailed = 4,
    kReceiveFailed = 5,
    kSendFailed = 6,
    kAcceptFailed = 7,
    kDisconnected = 8,
    kProtocolEof = 9,
    kInternalError = 10,
};

enum class TimeoutType {
    kConnect = 0,
    kReceive = 1,
    kSend = 2,
    kAccept = 3,
};

enum class FailureOperation {
    kEnsure,
    kReceive,
    kSend,
    kDisconnect,
    kProtocol,
    kInternal,
};

struct ReasonDescriptor {
    ReasonCode code = ReasonCode::kInternalError;
    ReasonClass reason_class = ReasonClass::kInternal;
    bool has_timeout_type = false;
    TimeoutType timeout_type = TimeoutType::kConnect;
};

struct LastFailureSnapshot {
    int64_t timestamp_unix_seconds = 0;
    std::string reason_code;
    std::string reason_class;
    std::string reason_detail;
    std::string incident_id;
    uint64_t attempt_id = 0;
    std::string source;
};

constexpr size_t kFailureSides = 2;
constexpr size_t kReasonCodes = 11;
constexpr size_t kTimeoutTypes = 4;

size_t SideIndex(FailureSide side);
size_t ReasonCodeIndex(ReasonCode code);
size_t TimeoutTypeIndex(TimeoutType type);

const char* FailureSideName(FailureSide side);
const char* ReasonClassName(ReasonClass reason_class);
const char* ReasonCodeName(ReasonCode code);
const char* TimeoutTypeName(TimeoutType type);

ReasonClass ReasonClassFromCode(ReasonCode code);
ReasonDescriptor ClassifyReason(FailureOperation op,
                                bool is_timeout,
                                bool is_disconnected,
                                bool listener_mode,
                                const std::string& detail);

const std::array<ReasonCode, kReasonCodes>& AllReasonCodes();
const std::array<TimeoutType, kTimeoutTypes>& AllTimeoutTypes();

}  // namespace srtrelay
