#include <streamcore/numeric.hpp>

#include <functional>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {
int checks = 0;
void check(bool ok, const std::string& label) {
    ++checks;
    if (!ok) throw std::runtime_error(label);
}
void rejects(const std::function<void()>& action, const std::string& label) {
    bool rejected = false;
    try { action(); } catch (const streamcore::ProtocolError&) { rejected = true; }
    check(rejected, label);
}
const std::string valid = R"({"schema_version":1,"source_id":"sim","stream_id":"values","session_id":"run1","seq":"0","kind":"numeric","payload":{"name":"temperature","value":21.5}})";
std::string replace(std::string text, const std::string& from, const std::string& to) {
    const auto position = text.find(from);
    if (position == std::string::npos) throw std::runtime_error("bad test replacement");
    text.replace(position, from.size(), to);
    return text;
}
void same(const streamcore::NumericMessage& a, const streamcore::NumericMessage& b) {
    check(a.schema_version == b.schema_version && a.kind == b.kind &&
          a.source_id == b.source_id && a.stream_id == b.stream_id && a.session_id == b.session_id &&
          a.seq == b.seq && a.name == b.name && a.value == b.value && a.unit == b.unit,
          "round-trip field semantics");
    check(a.source_time.has_value() == b.source_time.has_value(), "optional time presence");
    if (a.source_time) check(a.source_time->value == b.source_time->value && a.source_time->unit == b.source_time->unit, "time fields");
}
}  // namespace

int main() {
    try {
        auto message = streamcore::decode_numeric(valid);
        check(message.source_id == "sim" && message.name == "temperature" && message.value == 21.5, "known values");
        check(!message.unit && !message.source_time && message.seq == 0, "absent optional values");
        same(message, streamcore::decode_numeric(streamcore::encode_numeric(message)));
        message.seq = std::numeric_limits<std::uint64_t>::max();
        message.source_time = streamcore::SourceTime{message.seq, "us"};
        message.unit = "°C";
        message.name = "温度";
        same(message, streamcore::decode_numeric(streamcore::encode_numeric(message)));
        message.seq = 9007199254740993ULL;
        same(message, streamcore::decode_numeric(streamcore::encode_numeric(message)));
        std::string temporary = valid;
        auto owned = streamcore::decode_numeric(temporary);
        temporary.assign(temporary.size(), 'x');
        check(owned.source_id == "sim" && owned.name == "temperature", "owns input data");
        std::string at_limit = valid + std::string(streamcore::max_body_bytes - valid.size(), ' ');
        same(streamcore::decode_numeric(valid), streamcore::decode_numeric(at_limit));
        rejects([&] { streamcore::decode_numeric(at_limit + " "); }, "oversized body");
        for (const std::string& bad : std::vector<std::string>{"", "{", "[]", "null", "true", "{}", valid + "{}", "\xEF\xBB\xBF[]"}) {
            rejects([&] { streamcore::decode_numeric(bad); }, "invalid JSON/body");
        }
        for (const auto& pair : std::vector<std::pair<std::string, std::string>>{
            {"\"schema_version\":1", "\"schema_version\":1.0"},
            {"\"schema_version\":1", "\"schema_version\":2"},
            {"\"schema_version\":1", "\"schema_version\":true"},
            {"\"schema_version\":1,", ""},
            {"\"seq\":\"0\"", "\"seq\":0"},
            {"\"seq\":\"0\"", "\"seq\":\"18446744073709551616\""},
            {"\"seq\":\"0\"", "\"seq\":\"-1\""},
            {"\"seq\":\"0\"", "\"seq\":\"01\""},
            {"\"seq\":\"0\"", "\"seq\":\"+1\""},
            {"\"seq\":\"0\"", "\"seq\":\"1e3\""},
            {"\"seq\":\"0\"", "\"seq\":\"\""},
            {"\"kind\":\"numeric\"", "\"kind\":\"image\""},
            {"\"source_id\":\"sim\"", "\"source_id\":null"},
            {"\"source_id\":\"sim\"", "\"source_id\":\"\""},
            {"\"source_id\":\"sim\"", "\"source_id\":\"bad id\""},
            {"21.5", "true"}, {"21.5", "\"21.5\""}, {"21.5", "null"},
            {"21.5", "1e400"}, {"21.5", "NaN"}, {"21.5", "Infinity"},
            {"\"temperature\"", "\"\""}, {"\"temperature\"", "\"a\\u0000b\""},
            {"\"payload\":", "\"extra\":0,\"payload\":"},
            {"\"seq\":\"0\"", "\"seq\":\"0\",\"seq\":\"1\""},
            {"\"value\":21.5", "\"value\":21.5,\"value\":2"},
            {"\"payload\":", "\"source_time\":null,\"payload\":"},
            {"\"payload\":", "\"source_time\":{\"value\":\"0\",\"unit\":\"ms\"},\"payload\":"},
            {"\"value\":21.5", "\"value\":21.5,\"unit\":null"},
            {"\"value\":21.5", "\"value\":21.5,\"unit\":\"\""}
        }) {
            rejects([&] { streamcore::decode_numeric(replace(valid, pair.first, pair.second)); }, "invalid field: " + pair.second);
        }
        rejects([&] { streamcore::decode_numeric(std::string(100, '[') + std::string(100, ']')); }, "deep nesting");
        rejects([&] { streamcore::decode_numeric(replace(valid, "temperature", std::string(129, 'n'))); }, "incoming long name");
        rejects([&] { streamcore::decode_numeric(replace(valid, "sim", std::string(65, 's'))); }, "incoming long id");
        rejects([&] { streamcore::decode_numeric(replace(valid, "temperature", std::string(1, char(0xff)))); }, "incoming invalid UTF-8");
        rejects([&] { streamcore::decode_numeric(valid + std::string("\0junk", 5)); }, "raw NUL cannot hide trailing data");
        for (const auto& mutate : std::vector<std::function<void(streamcore::NumericMessage&)>>{
            [](auto& m) { m.schema_version = 2; }, [](auto& m) { m.kind = "market"; },
            [](auto& m) { m.value = std::numeric_limits<double>::quiet_NaN(); },
            [](auto& m) { m.value = std::numeric_limits<double>::infinity(); },
            [](auto& m) { m.source_id = std::string(65, 'a'); },
            [](auto& m) { m.stream_id = ""; }, [](auto& m) { m.session_id = "bad id"; },
            [](auto& m) { m.name = std::string(129, 'a'); },
            [](auto& m) { m.name = std::string(1, char(0xff)); },
            [](auto& m) { m.unit = std::string(17, 'a'); },
            [](auto& m) { m.source_time = streamcore::SourceTime{1, "seconds"}; }
        }) {
            auto bad = streamcore::decode_numeric(valid);
            mutate(bad);
            rejects([&] { streamcore::validate_numeric(bad); }, "direct model validation");
            rejects([&] { streamcore::encode_numeric(bad); }, "serialization must validate");
        }
        auto boundary = streamcore::decode_numeric(valid);
        boundary.source_id = std::string(64, 'a');
        boundary.name = std::string(128, 'n');
        boundary.unit = std::string(16, 'u');
        boundary.value = std::numeric_limits<double>::max();
        same(boundary, streamcore::decode_numeric(streamcore::encode_numeric(boundary)));
        std::cout << "PASS: numeric " << checks << " checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
