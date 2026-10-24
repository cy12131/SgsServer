#pragma once

#include "cardgame/server/lobby/lobby_service.h"
#include "cardgame/server/protocol/message_codec.h"

#include <cstdint>
#include <vector>

namespace cardgame::server::protocol {

struct RouteResult {
    Message response;
    std::vector<lobby::Notification> notifications;
    bool handshake_completed = false;
    bool close_after_write = false;
};

class MessageRouter final {
public:
    MessageRouter(lobby::LobbyService& lobby_service, std::uint64_t session_id) noexcept;

    [[nodiscard]] RouteResult route(const Message& request, bool ready) const;

private:
    [[nodiscard]] static Message error(
        std::int32_t request_id, std::string code, std::string text);

    lobby::LobbyService& lobby_service_;
    std::uint64_t session_id_;
};

}  // namespace cardgame::server::protocol
