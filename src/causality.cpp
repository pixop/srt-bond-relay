#include "srtrelay/causality.hpp"

#include <algorithm>
#include <cctype>

namespace srtrelay {

namespace {

bool ContainsInsensitive(const std::string& value, const char* needle) {
    std::string lowered_value = value;
    std::transform(lowered_value.begin(), lowered_value.end(), lowered_value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    std::string lowered_needle = needle;
    std::transform(lowered_needle.begin(), lowered_needle.end(), lowered_needle.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return lowered_value.find(lowered_needle) != std::string::npos;
}

}  // namespace

size_t SideIndex(FailureSide side) {
    return side == FailureSide::kInput ? 0 : 1;
}

size_t ReasonCodeIndex(ReasonCode code) {
    return static_cast<size_t>(code);
}

size_t TimeoutTypeIndex(TimeoutType type) {
    return static_cast<size_t>(type);
}

const char* FailureSideName(FailureSide side) {
    return side == FailureSide::kInput ? "input" : "output";
}

const char* ReasonClassName(ReasonClass reason_class) {
    switch (reason_class) {
        case ReasonClass::kTimeout:
            return "timeout";
        case ReasonClass::kDisconnect:
            return "disconnect";
        case ReasonClass::kConnectFailure:
            return "connect_failure";
        case ReasonClass::kIoFailure:
            return "io_failure";
        case ReasonClass::kProtocolState:
            return "protocol_state";
        case ReasonClass::kInternal:
            return "internal";
    }
    return "internal";
}

const char* ReasonCodeName(ReasonCode code) {
    switch (code) {
        case ReasonCode::kConnectTimeout:
            return "connect_timeout";
        case ReasonCode::kReceiveTimeout:
            return "receive_timeout";
        case ReasonCode::kSendTimeout:
            return "send_timeout";
        case ReasonCode::kAcceptTimeout:
            return "accept_timeout";
        case ReasonCode::kConnectFailed:
            return "connect_failed";
        case ReasonCode::kReceiveFailed:
            return "receive_failed";
        case ReasonCode::kSendFailed:
            return "send_failed";
        case ReasonCode::kAcceptFailed:
            return "accept_failed";
        case ReasonCode::kDisconnected:
            return "disconnected";
        case ReasonCode::kProtocolEof:
            return "protocol_eof";
        case ReasonCode::kInternalError:
            return "internal_error";
    }
    return "internal_error";
}

const char* TimeoutTypeName(TimeoutType type) {
    switch (type) {
        case TimeoutType::kConnect:
            return "connect";
        case TimeoutType::kReceive:
            return "receive";
        case TimeoutType::kSend:
            return "send";
        case TimeoutType::kAccept:
            return "accept";
    }
    return "connect";
}

ReasonClass ReasonClassFromCode(ReasonCode code) {
    switch (code) {
        case ReasonCode::kConnectTimeout:
        case ReasonCode::kReceiveTimeout:
        case ReasonCode::kSendTimeout:
        case ReasonCode::kAcceptTimeout:
            return ReasonClass::kTimeout;
        case ReasonCode::kDisconnected:
            return ReasonClass::kDisconnect;
        case ReasonCode::kConnectFailed:
        case ReasonCode::kAcceptFailed:
            return ReasonClass::kConnectFailure;
        case ReasonCode::kReceiveFailed:
        case ReasonCode::kSendFailed:
            return ReasonClass::kIoFailure;
        case ReasonCode::kProtocolEof:
            return ReasonClass::kProtocolState;
        case ReasonCode::kInternalError:
            return ReasonClass::kInternal;
    }
    return ReasonClass::kInternal;
}

ReasonDescriptor ClassifyReason(FailureOperation op,
                                bool is_timeout,
                                bool is_disconnected,
                                bool listener_mode,
                                const std::string& detail) {
    ReasonDescriptor out {};
    auto set_code = [&](ReasonCode code) {
        out.code = code;
        out.reason_class = ReasonClassFromCode(code);
    };

    switch (op) {
        case FailureOperation::kEnsure: {
            const bool accept_path = listener_mode || ContainsInsensitive(detail, "accept");
            if (is_timeout) {
                if (accept_path) {
                    set_code(ReasonCode::kAcceptTimeout);
                    out.has_timeout_type = true;
                    out.timeout_type = TimeoutType::kAccept;
                } else {
                    set_code(ReasonCode::kConnectTimeout);
                    out.has_timeout_type = true;
                    out.timeout_type = TimeoutType::kConnect;
                }
                break;
            }
            set_code(accept_path ? ReasonCode::kAcceptFailed : ReasonCode::kConnectFailed);
            break;
        }
        case FailureOperation::kReceive:
            if (is_timeout) {
                set_code(ReasonCode::kReceiveTimeout);
                out.has_timeout_type = true;
                out.timeout_type = TimeoutType::kReceive;
            } else if (is_disconnected) {
                set_code(ReasonCode::kDisconnected);
            } else {
                set_code(ReasonCode::kReceiveFailed);
            }
            break;
        case FailureOperation::kSend:
            if (is_timeout) {
                set_code(ReasonCode::kSendTimeout);
                out.has_timeout_type = true;
                out.timeout_type = TimeoutType::kSend;
            } else if (is_disconnected) {
                set_code(ReasonCode::kDisconnected);
            } else {
                set_code(ReasonCode::kSendFailed);
            }
            break;
        case FailureOperation::kDisconnect:
            set_code(ReasonCode::kDisconnected);
            break;
        case FailureOperation::kProtocol:
            set_code(ReasonCode::kProtocolEof);
            break;
        case FailureOperation::kInternal:
            set_code(ReasonCode::kInternalError);
            break;
    }

    return out;
}

const std::array<ReasonCode, kReasonCodes>& AllReasonCodes() {
    static const std::array<ReasonCode, kReasonCodes> kCodes = {
        ReasonCode::kConnectTimeout,
        ReasonCode::kReceiveTimeout,
        ReasonCode::kSendTimeout,
        ReasonCode::kAcceptTimeout,
        ReasonCode::kConnectFailed,
        ReasonCode::kReceiveFailed,
        ReasonCode::kSendFailed,
        ReasonCode::kAcceptFailed,
        ReasonCode::kDisconnected,
        ReasonCode::kProtocolEof,
        ReasonCode::kInternalError,
    };
    return kCodes;
}

const std::array<TimeoutType, kTimeoutTypes>& AllTimeoutTypes() {
    static const std::array<TimeoutType, kTimeoutTypes> kTypes = {
        TimeoutType::kConnect,
        TimeoutType::kReceive,
        TimeoutType::kSend,
        TimeoutType::kAccept,
    };
    return kTypes;
}

}  // namespace srtrelay
