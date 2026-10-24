#pragma once

#include <boost/json/object.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cardgame::server::protocol {

inline constexpr std::size_t maximum_body_size = 65536;

struct Message {
    int protocol = 1;
    std::string type;
    std::int32_t request_id = 0;
    boost::json::object data;
};

struct DecodeResult {
    std::optional<Message> message;
    std::string error;
};

class MessageCodec final {
public:
    [[nodiscard]] static DecodeResult decode_body(std::string_view body);
    [[nodiscard]] static std::vector<std::uint8_t> encode_frame(const Message& message);
};

}  // namespace cardgame::server::protocol
