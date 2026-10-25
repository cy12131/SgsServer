#pragma once

#include "cardgame/server/lobby/room_manager.h"
#include "cardgame/server/lobby/user_manager.h"
#include "cardgame/server/protocol/message_codec.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cardgame::server::lobby {

struct Notification {
    std::uint64_t session_id = 0;
    protocol::Message message;
};

struct HandleResult {
    protocol::Message response;
    std::vector<Notification> notifications;
};

class LobbyService final {
public:
    [[nodiscard]] HandleResult handle(
        std::uint64_t session_id, const protocol::Message& request);
    [[nodiscard]] std::vector<Notification> disconnect(std::uint64_t session_id);

private:
    [[nodiscard]] HandleResult enter_lobby(
        std::uint64_t session_id, const protocol::Message& request);
    [[nodiscard]] HandleResult list_rooms(
        std::uint64_t session_id, const protocol::Message& request);
    [[nodiscard]] HandleResult create_room(
        std::uint64_t session_id, const protocol::Message& request);
    [[nodiscard]] HandleResult join_room(
        std::uint64_t session_id, const protocol::Message& request);
    [[nodiscard]] HandleResult leave_room(
        std::uint64_t session_id, const protocol::Message& request);
    [[nodiscard]] HandleResult set_ready(
        std::uint64_t session_id, const protocol::Message& request);

    [[nodiscard]] protocol::Message error(
        std::int32_t request_id, std::string code, std::string text) const;
    [[nodiscard]] protocol::Message room_snapshot(const Room& room) const;
    [[nodiscard]] std::vector<Notification> snapshot_notifications(const Room& room) const;
    [[nodiscard]] std::vector<Notification> room_list_changed_notifications() const;
    [[nodiscard]] std::vector<Notification> room_change_notifications(
        const LeaveOutcome& outcome) const;

    UserManager users_;
    RoomManager rooms_;
};

}  // namespace cardgame::server::lobby
