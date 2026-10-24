#include "cardgame/server/lobby/lobby_service.h"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace cardgame::server::lobby {
namespace {

struct Character {
    std::size_t offset = 0;
    std::uint32_t code_point = 0;
};

bool is_unicode_space(std::uint32_t value) {
    return (value >= 0x09U && value <= 0x0DU) || value == 0x20U ||
           value == 0x85U || value == 0xA0U || value == 0x1680U ||
           (value >= 0x2000U && value <= 0x200AU) || value == 0x2028U ||
           value == 0x2029U || value == 0x202FU || value == 0x205FU ||
           value == 0x3000U;
}

bool is_control(std::uint32_t value) {
    return value <= 0x1FU || (value >= 0x7FU && value <= 0x9FU);
}

std::vector<Character> characters(std::string_view text) {
    std::vector<Character> result;
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    std::size_t index = 0;
    while (index < text.size()) {
        const unsigned char first = bytes[index];
        std::size_t count = 0;
        std::uint32_t code_point = 0;
        if (first <= 0x7FU) {
            code_point = first;
        } else if ((first & 0xE0U) == 0xC0U) {
            count = 1;
            code_point = first & 0x1FU;
        } else if ((first & 0xF0U) == 0xE0U) {
            count = 2;
            code_point = first & 0x0FU;
        } else {
            count = 3;
            code_point = first & 0x07U;
        }
        for (std::size_t offset = 1; offset <= count; ++offset) {
            code_point = (code_point << 6U) | (bytes[index + offset] & 0x3FU);
        }
        result.push_back({index, code_point});
        index += count + 1;
    }
    return result;
}

std::optional<std::string> normalize_text(std::string_view text, std::size_t maximum) {
    const std::vector<Character> decoded = characters(text);
    std::size_t first = 0;
    while (first < decoded.size() && is_unicode_space(decoded[first].code_point)) {
        ++first;
    }
    std::size_t last = decoded.size();
    while (last > first && is_unicode_space(decoded[last - 1].code_point)) {
        --last;
    }
    if (first == last || last - first > maximum) {
        return std::nullopt;
    }
    for (std::size_t index = first; index < last; ++index) {
        if (is_control(decoded[index].code_point)) {
            return std::nullopt;
        }
    }
    const std::size_t begin = decoded[first].offset;
    const std::size_t end = last == decoded.size() ? text.size() : decoded[last].offset;
    return std::string{text.substr(begin, end - begin)};
}

protocol::Message response(std::string type, std::int32_t request_id) {
    protocol::Message message;
    message.type = std::move(type);
    message.request_id = request_id;
    return message;
}

std::optional<std::uint64_t> positive_id(const boost::json::value* value) {
    if (value == nullptr || !value->is_int64() || value->as_int64() <= 0) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(value->as_int64());
}

}  // namespace

HandleResult LobbyService::handle(
    std::uint64_t session_id, const protocol::Message& request) {
    if (request.type == "EnterLobby") {
        return enter_lobby(session_id, request);
    }
    if (request.type == "ListRooms") {
        return list_rooms(session_id, request);
    }
    if (request.type == "CreateRoom") {
        return create_room(session_id, request);
    }
    if (request.type == "JoinRoom") {
        return join_room(session_id, request);
    }
    if (request.type == "LeaveRoom") {
        return leave_room(session_id, request);
    }
    return set_ready(session_id, request);
}

HandleResult LobbyService::enter_lobby(
    std::uint64_t session_id, const protocol::Message& request) {
    if (users_.find_by_session(session_id) != nullptr) {
        return {error(request.request_id, "ALREADY_IN_LOBBY", "User already entered lobby"), {}};
    }
    const auto* nickname_value = request.data.if_contains("nickname");
    if (nickname_value == nullptr || !nickname_value->is_string()) {
        return {error(request.request_id, "INVALID_NICKNAME", "Invalid nickname"), {}};
    }
    const auto nickname = normalize_text(nickname_value->as_string(), 20);
    if (!nickname) {
        return {error(request.request_id, "INVALID_NICKNAME", "Invalid nickname"), {}};
    }

    User& user = users_.create(session_id, *nickname);
    protocol::Message result = response("EnterLobbyResult", request.request_id);
    result.data = {{"user_id", user.user_id}, {"nickname", user.nickname}};
    return {std::move(result), {}};
}

HandleResult LobbyService::list_rooms(
    std::uint64_t session_id, const protocol::Message& request) {
    if (users_.find_by_session(session_id) == nullptr) {
        return {error(request.request_id, "LOBBY_REQUIRED", "Enter lobby first"), {}};
    }
    if (!request.data.empty()) {
        return {error(request.request_id, "INVALID_MESSAGE", "ListRooms data must be empty"), {}};
    }

    boost::json::array rooms;
    for (const auto& [room_id, room] : rooms_.rooms()) {
        const auto owner = std::find_if(
            room.players.begin(), room.players.end(), [&room](const RoomPlayer& player) {
                return player.user_id == room.owner_id;
            });
        rooms.emplace_back(boost::json::object{
            {"room_id", room_id},
            {"name", room.name},
            {"owner_nickname", owner->nickname},
            {"player_count", room.players.size()},
            {"max_players", room.max_players},
            {"status", "waiting"},
        });
    }
    protocol::Message result = response("ListRoomsResult", request.request_id);
    result.data = {{"rooms", std::move(rooms)}};
    return {std::move(result), {}};
}

HandleResult LobbyService::create_room(
    std::uint64_t session_id, const protocol::Message& request) {
    User* user = users_.find_by_session(session_id);
    if (user == nullptr) {
        return {error(request.request_id, "LOBBY_REQUIRED", "Enter lobby first"), {}};
    }
    if (user->room_id) {
        return {error(request.request_id, "ALREADY_IN_ROOM", "User already joined a room"), {}};
    }
    const auto* name_value = request.data.if_contains("name");
    const auto* maximum_value = request.data.if_contains("max_players");
    if (name_value == nullptr || !name_value->is_string() || maximum_value == nullptr ||
        !maximum_value->is_int64()) {
        return {error(request.request_id, "INVALID_ROOM_DATA", "Invalid room data"), {}};
    }
    const auto name = normalize_text(name_value->as_string(), 40);
    const std::int64_t maximum = maximum_value->as_int64();
    if (!name || maximum < 2 || maximum > 8) {
        return {error(request.request_id, "INVALID_ROOM_DATA", "Invalid room data"), {}};
    }
    if (rooms_.size() >= maximum_room_count) {
        return {error(request.request_id, "RESOURCE_LIMIT", "Room limit reached"), {}};
    }

    Room& room = rooms_.create(*user, *name, static_cast<int>(maximum));
    protocol::Message result = response("CreateRoomResult", request.request_id);
    result.data = {{"room_id", room.room_id}};
    std::vector<Notification> notifications = snapshot_notifications(room);
    auto lobby_notifications = room_list_changed_notifications();
    notifications.insert(
        notifications.end(),
        std::make_move_iterator(lobby_notifications.begin()),
        std::make_move_iterator(lobby_notifications.end()));
    return {std::move(result), std::move(notifications)};
}

HandleResult LobbyService::join_room(
    std::uint64_t session_id, const protocol::Message& request) {
    User* user = users_.find_by_session(session_id);
    if (user == nullptr) {
        return {error(request.request_id, "LOBBY_REQUIRED", "Enter lobby first"), {}};
    }
    if (user->room_id) {
        return {error(request.request_id, "ALREADY_IN_ROOM", "User already joined a room"), {}};
    }
    const auto room_id = positive_id(request.data.if_contains("room_id"));
    if (!room_id) {
        return {error(request.request_id, "INVALID_ROOM_DATA", "Invalid room id"), {}};
    }
    Room* room = rooms_.find(*room_id);
    if (room == nullptr) {
        return {error(request.request_id, "ROOM_NOT_FOUND", "Room not found"), {}};
    }
    if (room->players.size() >= static_cast<std::size_t>(room->max_players)) {
        return {error(request.request_id, "ROOM_FULL", "Room is full"), {}};
    }

    rooms_.join(*room, *user);
    protocol::Message result = response("JoinRoomResult", request.request_id);
    result.data = {{"room_id", room->room_id}};
    std::vector<Notification> notifications = snapshot_notifications(*room);
    auto lobby_notifications = room_list_changed_notifications();
    notifications.insert(
        notifications.end(),
        std::make_move_iterator(lobby_notifications.begin()),
        std::make_move_iterator(lobby_notifications.end()));
    return {std::move(result), std::move(notifications)};
}

HandleResult LobbyService::leave_room(
    std::uint64_t session_id, const protocol::Message& request) {
    User* user = users_.find_by_session(session_id);
    if (user == nullptr) {
        return {error(request.request_id, "LOBBY_REQUIRED", "Enter lobby first"), {}};
    }
    if (!user->room_id) {
        return {error(request.request_id, "NOT_IN_ROOM", "User is not in a room"), {}};
    }
    if (!request.data.empty()) {
        return {error(request.request_id, "INVALID_MESSAGE", "LeaveRoom data must be empty"), {}};
    }

    const LeaveOutcome outcome = rooms_.leave(*user);
    protocol::Message result = response("LeaveRoomResult", request.request_id);
    return {std::move(result), room_change_notifications(outcome)};
}

HandleResult LobbyService::set_ready(
    std::uint64_t session_id, const protocol::Message& request) {
    User* user = users_.find_by_session(session_id);
    if (user == nullptr) {
        return {error(request.request_id, "LOBBY_REQUIRED", "Enter lobby first"), {}};
    }
    if (!user->room_id) {
        return {error(request.request_id, "NOT_IN_ROOM", "User is not in a room"), {}};
    }
    const auto* ready = request.data.if_contains("ready");
    if (ready == nullptr || !ready->is_bool()) {
        return {error(request.request_id, "INVALID_MESSAGE", "Ready must be a boolean"), {}};
    }
    Room& room = *rooms_.find(*user->room_id);
    if (room.owner_id == user->user_id) {
        return {error(request.request_id, "PERMISSION_DENIED", "Room owner cannot ready"), {}};
    }

    static_cast<void>(rooms_.set_ready(room, user->user_id, ready->as_bool()));
    protocol::Message result = response("SetReadyResult", request.request_id);
    result.data = {{"ready", ready->as_bool()}};
    return {std::move(result), snapshot_notifications(room)};
}

std::vector<Notification> LobbyService::disconnect(std::uint64_t session_id) {
    User* user = users_.find_by_session(session_id);
    if (user == nullptr) {
        return {};
    }
    std::optional<LeaveOutcome> outcome;
    if (user->room_id) {
        outcome = rooms_.leave(*user);
    }
    static_cast<void>(users_.remove(session_id));
    return outcome ? room_change_notifications(*outcome) : std::vector<Notification>{};
}

protocol::Message LobbyService::error(
    std::int32_t request_id, std::string code, std::string text) const {
    protocol::Message message = response("Error", request_id);
    message.data = {{"code", std::move(code)}, {"message", std::move(text)}};
    return message;
}

protocol::Message LobbyService::room_snapshot(const Room& room) const {
    boost::json::array players;
    for (const RoomPlayer& player : room.players) {
        players.emplace_back(boost::json::object{
            {"user_id", player.user_id},
            {"nickname", player.nickname},
            {"seat", player.seat},
            {"ready", player.ready},
            {"is_owner", player.user_id == room.owner_id},
        });
    }
    protocol::Message message = response("RoomSnapshot", 0);
    message.data = {
        {"room_id", room.room_id},
        {"revision", room.revision},
        {"name", room.name},
        {"max_players", room.max_players},
        {"status", "waiting"},
        {"owner_id", room.owner_id},
        {"players", std::move(players)},
    };
    return message;
}

std::vector<Notification> LobbyService::snapshot_notifications(const Room& room) const {
    std::vector<Notification> notifications;
    const protocol::Message snapshot = room_snapshot(room);
    for (const RoomPlayer& player : room.players) {
        if (const User* user = users_.find_by_user_id(player.user_id); user != nullptr) {
            notifications.push_back({user->session_id, snapshot});
        }
    }
    return notifications;
}

std::vector<Notification> LobbyService::room_list_changed_notifications() const {
    std::vector<Notification> notifications;
    protocol::Message message = response("RoomListChanged", 0);
    for (const std::uint64_t session_id : users_.lobby_session_ids()) {
        notifications.push_back({session_id, message});
    }
    return notifications;
}

std::vector<Notification> LobbyService::room_change_notifications(
    const LeaveOutcome& outcome) const {
    std::vector<Notification> notifications;
    if (!outcome.deleted) {
        notifications = snapshot_notifications(*rooms_.find(outcome.room_id));
    }
    auto lobby_notifications = room_list_changed_notifications();
    notifications.insert(
        notifications.end(),
        std::make_move_iterator(lobby_notifications.begin()),
        std::make_move_iterator(lobby_notifications.end()));
    return notifications;
}

}  // namespace cardgame::server::lobby
