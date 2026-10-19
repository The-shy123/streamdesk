#include <streamcore/numeric.hpp>

#include <nlohmann/json.hpp>

#include <charconv>
#include <cmath>
#include <initializer_list>
#include <set>
#include <vector>

namespace streamcore {
namespace {
using Json = nlohmann::json;

void require(bool condition, const char* message) {
    if (!condition) throw ProtocolError(message);
}

void identifier(const std::string& value) {
    require(!value.empty() && value.size() <= 64, "identifier length must be 1..64 bytes");
    for (unsigned char c : value) {
        require((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.',
                "identifier must use ASCII letters, digits, _, - or .");
    }
}

void text_field(const std::string& value, std::size_t limit) {
    require(!value.empty() && value.size() <= limit, "text field length out of range");
    for (unsigned char c : value) require(c >= 0x20 && c != 0x7f, "control character in text");
    try {
        (void)Json(value).dump(); // Strict UTF-8 validation, also for directly built models.
    } catch (const Json::exception&) {
        throw ProtocolError("text must be valid UTF-8");
    }
}

void keys(const Json& object, std::initializer_list<std::string_view> allowed) {
    require(object.is_object(), "expected JSON object");
    for (auto it = object.begin(); it != object.end(); ++it) {
        bool found = false;
        for (auto key : allowed) if (it.key() == key) found = true;
        require(found, "unknown field");
    }
}

std::string string_field(const Json& object, const char* key) {
    require(object.contains(key) && object.at(key).is_string(), "missing or non-string field");
    return object.at(key).get<std::string>();
}

std::uint64_t decimal(const std::string& text) {
    require(!text.empty() && text.size() <= 20, "uint64 decimal length out of range");
    require(text == "0" || text.front() != '0', "non-canonical leading zero");
    for (char c : text) require(c >= '0' && c <= '9', "expected unsigned decimal string");
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size(), "uint64 overflow");
    return value;
}
}  // namespace

void validate_numeric(const NumericMessage& message) {
    require(message.schema_version == 1, "unsupported schema_version");
    require(message.kind == "numeric", "unsupported kind");
    identifier(message.source_id);
    identifier(message.stream_id);
    identifier(message.session_id);
    text_field(message.name, 128);
    if (message.unit) text_field(*message.unit, 16);
    require(std::isfinite(message.value), "numeric value must be finite");
    if (message.source_time) require(message.source_time->unit == "us", "unsupported source_time unit");
}

NumericMessage decode_numeric(std::string_view body) {
    require(!body.empty() && body.size() <= max_body_bytes, "body length must be 1..65536 bytes");
    // This is a length-delimited protocol, not a NUL-terminated C string.
    require(body.find('\0') == std::string_view::npos, "raw NUL byte in JSON body");
    try {
        std::vector<std::set<std::string>> object_keys;
        auto callback = [&](int depth, Json::parse_event_t event, Json& parsed) {
            require(depth <= 8, "JSON nesting exceeds limit");
            if (event == Json::parse_event_t::object_start) object_keys.emplace_back();
            if (event == Json::parse_event_t::key) {
                require(object_keys.back().insert(parsed.get<std::string>()).second, "duplicate JSON key");
            }
            if (event == Json::parse_event_t::object_end) object_keys.pop_back();
            return true;
        };
        const auto json = Json::parse(body.begin(), body.end(), callback);
        keys(json, {"schema_version", "source_id", "stream_id", "session_id", "seq", "kind", "source_time", "payload"});
        require(json.contains("schema_version") && json.at("schema_version").is_number_integer(),
                "schema_version must be an integer");
        require(json.at("schema_version") == 1, "unsupported schema_version");
        NumericMessage message;
        message.source_id = string_field(json, "source_id");
        message.stream_id = string_field(json, "stream_id");
        message.session_id = string_field(json, "session_id");
        message.seq = decimal(string_field(json, "seq"));
        message.kind = string_field(json, "kind");
        if (json.contains("source_time")) {
            const auto& time = json.at("source_time");
            keys(time, {"value", "unit"});
            message.source_time = SourceTime{decimal(string_field(time, "value")), string_field(time, "unit")};
        }
        require(json.contains("payload"), "missing payload");
        const auto& payload = json.at("payload");
        keys(payload, {"name", "value", "unit"});
        message.name = string_field(payload, "name");
        require(payload.contains("value") && payload.at("value").is_number(), "value must be a JSON number");
        message.value = payload.at("value").get<double>();
        if (payload.contains("unit")) message.unit = string_field(payload, "unit");
        validate_numeric(message);
        return message;
    } catch (const Json::exception&) {
        throw ProtocolError("invalid JSON or out-of-range JSON number");
    }
}

std::string encode_numeric(const NumericMessage& message) {
    validate_numeric(message);
    Json json = {{"schema_version", message.schema_version}, {"source_id", message.source_id},
                 {"stream_id", message.stream_id}, {"session_id", message.session_id},
                 {"seq", std::to_string(message.seq)}, {"kind", message.kind},
                 {"payload", {{"name", message.name}, {"value", message.value}}}};
    if (message.unit) json["payload"]["unit"] = *message.unit;
    if (message.source_time) {
        json["source_time"] = {{"value", std::to_string(message.source_time->value)}, {"unit", message.source_time->unit}};
    }
    const auto body = json.dump();
    require(body.size() <= max_body_bytes, "encoded body exceeds limit");
    return body;
}

}  // namespace streamcore
