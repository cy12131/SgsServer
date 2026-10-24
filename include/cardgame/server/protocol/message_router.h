#pragma once

#include "cardgame/server/protocol/message_codec.h"

namespace cardgame::server::protocol {

struct RouteResult {
    Message response;
    bool handshake_completed = false;
    bool close_after_write = false;
};

class MessageRouter final {
public:
    [[nodiscard]] RouteResult route(const Message& request, bool ready) const;

private:
    [[nodiscard]] static Message error(
        std::int32_t request_id, std::string code, std::string text);
};

}  // namespace cardgame::server::protocol
