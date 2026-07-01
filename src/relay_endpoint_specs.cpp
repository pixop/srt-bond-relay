#include "srtrelay/relay_io.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>

namespace srtrelay {

namespace {

std::string Trim(std::string value) {
    auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(),
               [&](char ch) { return !is_space(static_cast<unsigned char>(ch)); }));
    value.erase(std::find_if(value.rbegin(), value.rend(),
               [&](char ch) { return !is_space(static_cast<unsigned char>(ch)); }).base(),
               value.end());
    return value;
}

bool IsStdinInputSpec(const std::string& input_uri) {
    return input_uri == "stdin" || input_uri == "-" || input_uri == "fd://stdin";
}

bool IsStdoutOutputSpec(const std::string& output_uri) {
    return output_uri == "stdout" || output_uri == "-" || output_uri == "fd://stdout";
}

std::vector<std::string> SplitSpecList(const std::string& value) {
    const char delimiter = (value.find(';') != std::string::npos) ? ';' : ',';
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= value.size()) {
        const size_t next = value.find(delimiter, start);
        const std::string token = Trim(value.substr(start, next == std::string::npos ? std::string::npos : next - start));
        if (!token.empty()) {
            parts.push_back(token);
        }
        if (next == std::string::npos) break;
        start = next + 1;
    }
    if (parts.empty()) {
        throw std::runtime_error("empty endpoint specification");
    }
    return parts;
}

std::vector<SrtUri> ParseSrtUriList(const std::string& spec) {
    const auto raw = SplitSpecList(spec);
    std::vector<SrtUri> uris;
    uris.reserve(raw.size());
    for (const auto& part : raw) {
        uris.push_back(ParseSrtUri(part));
    }
    return uris;
}

enum class EndpointScheme {
    kSrt,
    kUdp,
};

EndpointScheme DetectScheme(const std::string& value) {
    if (value.rfind("srt://", 0) == 0) return EndpointScheme::kSrt;
    if (value.rfind("udp://", 0) == 0) return EndpointScheme::kUdp;
    throw std::runtime_error("unsupported endpoint URI scheme: " + value);
}

bool HasBondQueryOption(const std::map<std::string, std::string>& query) {
    return !QueryString(query, "grouptype").empty() ||
           !QueryString(query, "group_type").empty() ||
           !QueryString(query, "bond").empty() ||
           !QueryString(query, "bond_mode").empty();
}

std::optional<SRT_GROUP_TYPE> ParseBondGroupType(const std::map<std::string, std::string>& query) {
    std::string value = QueryString(query, "grouptype");
    if (value.empty()) value = QueryString(query, "group_type");
    if (value.empty()) value = QueryString(query, "bond");
    if (value.empty()) value = QueryString(query, "bond_mode");
    if (value.empty()) return std::nullopt;

    if (value == "broadcast" || value == "1") return SRT_GTYPE_BROADCAST;
    if (value == "backup" || value == "2") return SRT_GTYPE_BACKUP;
    throw std::runtime_error("unsupported bonded group type: " + value);
}

SRT_GROUP_TYPE ResolveGroupType(const std::vector<SrtUri>& uris) {
    std::optional<SRT_GROUP_TYPE> selected;
    for (const auto& uri : uris) {
        const auto parsed = ParseBondGroupType(uri.query);
        if (!parsed.has_value()) continue;
        if (!selected.has_value()) {
            selected = parsed;
            continue;
        }
        if (*selected != *parsed) {
            throw std::runtime_error("all grouped SRT URIs must use the same bonded group type");
        }
    }
    if (selected.has_value()) return *selected;
    if (uris.size() > 1) return SRT_GTYPE_BROADCAST;
    return SRT_GTYPE_UNDEFINED;
}

std::vector<std::string> ParseRawUriList(const std::string& spec, EndpointScheme* out_scheme) {
    const auto raw = SplitSpecList(spec);
    if (raw.empty()) {
        throw std::runtime_error("empty endpoint specification");
    }
    const EndpointScheme scheme = DetectScheme(raw.front());
    for (const auto& token : raw) {
        if (DetectScheme(token) != scheme) {
            throw std::runtime_error("mixed endpoint URI schemes are not supported in one endpoint list");
        }
    }
    *out_scheme = scheme;
    return raw;
}

}  // namespace

std::vector<OutputEndpointSpec> ParseOutputEndpointSpecs(const Config& cfg);

InputEndpointSpec ParseInputEndpointSpecFromUri(const std::string& input_uri) {
    InputEndpointSpec out;
    if (IsStdinInputSpec(input_uri)) {
        out.kind = InputEndpointKind::kStdin;
        return out;
    }

    EndpointScheme scheme = EndpointScheme::kSrt;
    const auto raw = ParseRawUriList(input_uri, &scheme);
    if (scheme == EndpointScheme::kSrt) {
        out.uris = ParseSrtUriList(input_uri);
        const std::string mode = QueryString(out.uris.front().query, "mode");
        const std::string resolved_mode = mode.empty() ? "listener" : mode;
        for (const auto& uri : out.uris) {
            const std::string uri_mode = QueryString(uri.query, "mode");
            const std::string uri_resolved_mode = uri_mode.empty() ? "listener" : uri_mode;
            if (uri_resolved_mode != resolved_mode) {
                throw std::runtime_error("all input SRT URIs must use the same mode");
            }
        }

        if (resolved_mode == "listener") {
            out.kind = InputEndpointKind::kSrtListener;
        } else if (resolved_mode == "caller") {
            out.kind = InputEndpointKind::kSrtCaller;
        } else {
            throw std::runtime_error("input SRT URI mode must be listener or caller");
        }

        out.group_type = ResolveGroupType(out.uris);
        out.bonded = out.uris.size() > 1 || out.group_type != SRT_GTYPE_UNDEFINED;
        if (!out.bonded) {
            out.group_type = SRT_GTYPE_UNDEFINED;
        } else if (out.group_type == SRT_GTYPE_UNDEFINED) {
            out.group_type = SRT_GTYPE_BROADCAST;
        }
        return out;
    }

    if (raw.size() != 1) {
        throw std::runtime_error("grouped endpoint lists are only supported for SRT URIs");
    }
    out.udp_uri = ParseUdpUri(raw.front());
    if (HasBondQueryOption(out.udp_uri.query)) {
        throw std::runtime_error("bond options are only supported for SRT URIs");
    }
    const std::string mode = QueryString(out.udp_uri.query, "mode");
    const std::string resolved_mode = mode.empty() ? "listener" : mode;
    if (resolved_mode == "listener") {
        out.kind = InputEndpointKind::kUdpListener;
    } else if (resolved_mode == "caller") {
        throw std::runtime_error("input UDP caller mode is not supported; use mode=listener");
    } else {
        throw std::runtime_error("input UDP URI mode must be listener or caller");
    }
    return out;
}

std::vector<InputEndpointSpec> ParseInputEndpointSpecs(const Config& cfg) {
    std::vector<InputEndpointSpec> out;
    out.reserve(cfg.input_uris.size());
    size_t stdin_count = 0;
    for (const auto& input_uri : cfg.input_uris) {
        InputEndpointSpec spec = ParseInputEndpointSpecFromUri(input_uri);
        if (spec.kind == InputEndpointKind::kStdin) {
            ++stdin_count;
            if (stdin_count > 1) {
                throw std::runtime_error("multi-input mode supports at most one stdin source");
            }
        }
        out.push_back(std::move(spec));
    }
    if (out.empty()) {
        throw std::runtime_error("--input is required");
    }
    return out;
}

InputEndpointSpec ParseInputEndpointSpec(const Config& cfg) {
    const std::vector<InputEndpointSpec> specs = ParseInputEndpointSpecs(cfg);
    return specs.front();
}

OutputEndpointSpec ParseOutputEndpointSpec(const Config& cfg) {
    const std::vector<OutputEndpointSpec> specs = ParseOutputEndpointSpecs(cfg);
    return specs.front();
}

OutputEndpointSpec ParseOutputEndpointSpecFromUri(const std::string& output_uri) {
    OutputEndpointSpec out;
    if (IsStdoutOutputSpec(output_uri)) {
        out.kind = OutputEndpointKind::kStdout;
        return out;
    }

    EndpointScheme scheme = EndpointScheme::kSrt;
    const auto raw = ParseRawUriList(output_uri, &scheme);
    if (scheme == EndpointScheme::kSrt) {
        out.uris = ParseSrtUriList(output_uri);
        const std::string mode = QueryString(out.uris.front().query, "mode");
        const std::string resolved_mode = mode.empty() ? "caller" : mode;
        for (const auto& uri : out.uris) {
            const std::string uri_mode = QueryString(uri.query, "mode");
            const std::string uri_resolved_mode = uri_mode.empty() ? "caller" : uri_mode;
            if (uri_resolved_mode != resolved_mode) {
                throw std::runtime_error("all output SRT URIs must use the same mode");
            }
        }

        if (resolved_mode == "caller") {
            out.kind = OutputEndpointKind::kSrtCaller;
        } else if (resolved_mode == "listener") {
            out.kind = OutputEndpointKind::kSrtListener;
        } else {
            throw std::runtime_error("output SRT URI mode must be caller or listener");
        }

        out.group_type = ResolveGroupType(out.uris);
        out.bonded = out.uris.size() > 1 || out.group_type != SRT_GTYPE_UNDEFINED;
        if (!out.bonded) {
            out.group_type = SRT_GTYPE_UNDEFINED;
        } else if (out.group_type == SRT_GTYPE_UNDEFINED) {
            out.group_type = SRT_GTYPE_BROADCAST;
        }
        return out;
    }

    if (raw.size() != 1) {
        throw std::runtime_error("grouped endpoint lists are only supported for SRT URIs");
    }
    out.udp_uri = ParseUdpUri(raw.front());
    if (HasBondQueryOption(out.udp_uri.query)) {
        throw std::runtime_error("bond options are only supported for SRT URIs");
    }
    const std::string mode = QueryString(out.udp_uri.query, "mode");
    const std::string resolved_mode = mode.empty() ? "caller" : mode;
    if (resolved_mode == "caller") {
        out.kind = OutputEndpointKind::kUdpCaller;
    } else if (resolved_mode == "listener") {
        throw std::runtime_error("output UDP listener mode is not supported; use mode=caller");
    } else {
        throw std::runtime_error("output UDP URI mode must be caller or listener");
    }
    return out;
}

std::vector<OutputEndpointSpec> ParseOutputEndpointSpecs(const Config& cfg) {
    std::vector<OutputEndpointSpec> out;
    out.reserve(cfg.output_uris.size());
    size_t stdout_count = 0;
    for (const auto& output_uri : cfg.output_uris) {
        OutputEndpointSpec spec = ParseOutputEndpointSpecFromUri(output_uri);
        if (spec.kind == OutputEndpointKind::kStdout) {
            ++stdout_count;
            if (stdout_count > 1) {
                throw std::runtime_error("multi-output mode supports at most one stdout sink");
            }
        }
        out.push_back(std::move(spec));
    }
    if (out.empty()) {
        throw std::runtime_error("--output is required");
    }
    return out;
}

}  // namespace srtrelay
