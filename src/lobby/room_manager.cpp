#include "cardgame/server/lobby/room_manager.h"

#include <algorithm>
#include <utility>

namespace cardgame::server::lobby {

std::size_t RoomManager::size() const noexcept {
    return rooms_.size();
}

Room* RoomManager::find(std::uint64_t room_id) {
    const auto iterator = rooms_.find(room_id);
    return iterator == rooms_.end() ? nullptr : &iterator->second;
}

const Room* RoomManager::find(std::uint64_t room_id) const {
    const auto iterator = rooms_.find(room_id);
    return iterator == rooms_.end() ? nullptr : &iterator->second;
}

const std::map<std::uint64_t, Room>& RoomManager::rooms() const noexcept {
    return rooms_;
}

Room& RoomManager::create(User& owner, std::string name, int max_players) {
    const std::uint64_t room_id = next_room_id_++;
    Room room;
    room.room_id = room_id;
    room.name = std::move(name);
    room.owner_id = owner.user_id;
    room.max_players = max_players;
    room.players.push_back({owner.user_id, owner.nickname, 1, false});
    owner.room_id = room_id;
    return rooms_.emplace(room_id, std::move(room)).first->second;
}

void RoomManager::join(Room& room, User& user) {
    room.players.push_back(
        {user.user_id, user.nickname, next_available_seat(room), false});
    user.room_id = room.room_id;
    ++room.revision;
}

LeaveOutcome RoomManager::leave(User& user) {
    const std::uint64_t room_id = *user.room_id;
    Room& room = rooms_.at(room_id);
    const bool was_owner = room.owner_id == user.user_id;
    std::erase_if(room.players, [&user](const RoomPlayer& player) {
        return player.user_id == user.user_id;
    });
    user.room_id.reset();

    if (room.players.empty()) {
        rooms_.erase(room_id);
        return {room_id, true};
    }
    if (was_owner) {
        room.owner_id = room.players.front().user_id;
        room.players.front().ready = false;
    }
    ++room.revision;
    return {room_id, false};
}

bool RoomManager::set_ready(Room& room, std::uint64_t user_id, bool ready) {
    const auto player = std::find_if(
        room.players.begin(), room.players.end(), [user_id](const RoomPlayer& candidate) {
            return candidate.user_id == user_id;
        });
    if (player->ready == ready) {
        return false;
    }
    player->ready = ready;
    ++room.revision;
    return true;
}

int RoomManager::next_available_seat(const Room& room) {
    for (int seat = 1; seat <= room.max_players; ++seat) {
        const bool used = std::any_of(
            room.players.begin(), room.players.end(), [seat](const RoomPlayer& player) {
                return player.seat == seat;
            });
        if (!used) {
            return seat;
        }
    }
    return 0;
}

}  // namespace cardgame::server::lobby
