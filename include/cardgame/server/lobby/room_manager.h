#pragma once

#include "cardgame/server/lobby/user_manager.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace cardgame::server::lobby {

inline constexpr std::size_t maximum_room_count = 100;

struct RoomPlayer {
    std::uint64_t user_id = 0;
    std::string nickname;
    int seat = 0;
    bool ready = false;
};

struct Room {
    std::uint64_t room_id = 0;
    std::string name;
    std::uint64_t owner_id = 0;
    int max_players = 0;
    std::uint64_t revision = 1;
    std::vector<RoomPlayer> players;
};

struct LeaveOutcome {
    std::uint64_t room_id = 0;
    bool deleted = false;
};

class RoomManager final {
public:
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] Room* find(std::uint64_t room_id);
    [[nodiscard]] const Room* find(std::uint64_t room_id) const;
    [[nodiscard]] const std::map<std::uint64_t, Room>& rooms() const noexcept;

    [[nodiscard]] Room& create(User& owner, std::string name, int max_players);
    void join(Room& room, User& user);
    [[nodiscard]] LeaveOutcome leave(User& user);
    [[nodiscard]] bool set_ready(Room& room, std::uint64_t user_id, bool ready);

private:
    [[nodiscard]] static int next_available_seat(const Room& room);

    std::uint64_t next_room_id_ = 1;
    std::map<std::uint64_t, Room> rooms_;
};

}  // namespace cardgame::server::lobby
