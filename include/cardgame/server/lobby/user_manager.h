#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace cardgame::server::lobby {

struct User {
    std::uint64_t user_id = 0;
    std::string nickname;
    std::uint64_t session_id = 0;
    std::optional<std::uint64_t> room_id;
};

// UserManager 只保存数值 Session ID，不拥有网络 Session，因而不会形成 shared_ptr 环。
class UserManager final {
public:
    [[nodiscard]] User& create(std::uint64_t session_id, std::string nickname);
    [[nodiscard]] User* find_by_session(std::uint64_t session_id);
    [[nodiscard]] const User* find_by_session(std::uint64_t session_id) const;
    [[nodiscard]] User* find_by_user_id(std::uint64_t user_id);
    [[nodiscard]] const User* find_by_user_id(std::uint64_t user_id) const;
    [[nodiscard]] std::optional<User> remove(std::uint64_t session_id);
    [[nodiscard]] std::vector<std::uint64_t> lobby_session_ids() const;

private:
    std::uint64_t next_user_id_ = 1;
    std::unordered_map<std::uint64_t, User> users_by_session_;
    std::unordered_map<std::uint64_t, std::uint64_t> session_by_user_id_;
};

}  // namespace cardgame::server::lobby
