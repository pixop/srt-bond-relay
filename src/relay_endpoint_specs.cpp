#include "srtrelay/relay_io.hpp"

#include <algorithm>
#include <cctype>
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

}  // namespace

InputEndpointSpec ParseInputEndpointSpec(const Config& cfg) {
    InputEndpointSpec out;
    if (IsStdinInputSpec(cfg.input_uri)) {
        out.kind = InputEndpointKind::kStdin;
        return out;
    }

    out.uris = ParseSrtUriList(cfg.input_uri);
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
        throw std::runtime_error("input URI mode must be listener or caller");
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

OutputEndpointSpec ParseOutputEndpointSpec(const Config& cfg) {
    OutputEndpointSpec out;
    if (IsStdoutOutputSpec(cfg.output_uri)) {
        out.kind = OutputEndpointKind::kStdout;
        return out;
    }

    out.uris = ParseSrtUriList(cfg.output_uri);
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
        throw std::runtime_error("output URI mode must be caller or listener");
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

}  // namespace srtrelay
