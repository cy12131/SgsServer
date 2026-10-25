#include "cardgame/server/protocol/message_codec.h"

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>

#include <limits>
#include <stdexcept>

namespace cardgame::server::protocol {
namespace {

bool is_valid_utf8(std::string_view text) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    std::size_t index = 0;
    while (index < text.size()) {
        const unsigned char first = bytes[index];
        std::size_t continuation_count = 0;
        std::uint32_t code_point = 0;
        if (first <= 0x7F) {
            ++index;
            continue;
        }
        if ((first & 0xE0U) == 0xC0U) {
            continuation_count = 1;
            code_point = first & 0x1FU;
        } else if ((first & 0xF0U) == 0xE0U) {
            continuation_count = 2;
            code_point = first & 0x0FU;
        } else if ((first & 0xF8U) == 0xF0U) {
            continuation_count = 3;
            code_point = first & 0x07U;
        } else {
            return false;
        }
        if (index + continuation_count >= text.size()) {
            return false;
        }
        for (std::size_t offset = 1; offset <= continuation_count; ++offset) {
            const unsigned char continuation = bytes[index + offset];
            if ((continuation & 0xC0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (continuation & 0x3FU);
        }
        const bool overlong = (continuation_count == 1 && code_point < 0x80U) ||
                              (continuation_count == 2 && code_point < 0x800U) ||
                              (continuation_count == 3 && code_point < 0x10000U);
        if (overlong || code_point > 0x10FFFFU ||
            (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
            return false;
        }
        index += continuation_count + 1;
    }
    return true;
}

}  // namespace

DecodeResult MessageCodec::decode_body(std::string_view body) {
    if (body.empty() || body.size() > maximum_body_size) {
        return {std::nullopt, "body length is outside SGS/1 limits"};
    }
    if (!is_valid_utf8(body)) {
        return {std::nullopt, "body is not valid UTF-8"};
    }

    boost::system::error_code error;
    boost::json::value value = boost::json::parse(body, error);
    if (error || !value.is_object()) {
        return {std::nullopt, "body is not a JSON object"};
    }

    const boost::json::object& object = value.as_object();
    const auto* protocol_value = object.if_contains("protocol");
    const auto* type_value = object.if_contains("type");
    const auto* request_id_value = object.if_contains("request_id");
    const auto* data_value = object.if_contains("data");
    if (object.size() != 4 || protocol_value == nullptr || type_value == nullptr ||
        request_id_value == nullptr || data_value == nullptr ||
        !protocol_value->is_int64() || !type_value->is_string() ||
        !request_id_value->is_int64() || !data_value->is_object()) {
        return {std::nullopt, "required message fields are missing or invalid"};
    }

    const std::int64_t protocol_number = protocol_value->as_int64();
    const std::int64_t request_id = request_id_value->as_int64();
    if (protocol_number < std::numeric_limits<int>::min() ||
        protocol_number > std::numeric_limits<int>::max() || request_id < 1 ||
        request_id > std::numeric_limits<std::int32_t>::max()) {
        return {std::nullopt, "numeric message field is outside its allowed range"};
    }

    Message message;
    message.protocol = static_cast<int>(protocol_number);
    message.type = std::string{type_value->as_string()};
    message.request_id = static_cast<std::int32_t>(request_id);
    message.data = data_value->as_object();
    return {std::move(message), {}};
}

std::vector<std::uint8_t> MessageCodec::encode_frame(const Message& message) {
    boost::json::object object;
    object["protocol"] = message.protocol;
    object["type"] = message.type;
    object["request_id"] = message.request_id;
    object["data"] = message.data;
    const std::string body = boost::json::serialize(object);
    if (body.empty() || body.size() > maximum_body_size) {
        throw std::length_error{"encoded SGS/1 body is outside allowed limits"};
    }

    const auto length = static_cast<std::uint32_t>(body.size());
    std::vector<std::uint8_t> frame(4 + body.size());
    frame[0] = static_cast<std::uint8_t>((length >> 24U) & 0xFFU);
    frame[1] = static_cast<std::uint8_t>((length >> 16U) & 0xFFU);
    frame[2] = static_cast<std::uint8_t>((length >> 8U) & 0xFFU);
    frame[3] = static_cast<std::uint8_t>(length & 0xFFU);
    std::copy(body.begin(), body.end(), frame.begin() + 4);
    return frame;
}

}  // namespace cardgame::server::protocol
