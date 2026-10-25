#include "cardgame/server/lobby/user_manager.h"

#include <utility>

namespace cardgame::server::lobby {

User& UserManager::create(std::uint64_t session_id, std::string nickname) {
    const std::uint64_t user_id = next_user_id_++;
    auto [iterator, inserted] = users_by_session_.emplace(
        session_id, User{user_id, std::move(nickname), session_id, std::nullopt});
    if (!inserted) {
        return iterator->second;
    }
    session_by_user_id_.emplace(user_id, session_id);
    return iterator->second;
}

User* UserManager::find_by_session(std::uint64_t session_id) {
    const auto iterator = users_by_session_.find(session_id);
    return iterator == users_by_session_.end() ? nullptr : &iterator->second;
}

const User* UserManager::find_by_session(std::uint64_t session_id) const {
    const auto iterator = users_by_session_.find(session_id);
    return iterator == users_by_session_.end() ? nullptr : &iterator->second;
}

User* UserManager::find_by_user_id(std::uint64_t user_id) {
    const auto session = session_by_user_id_.find(user_id);
    return session == session_by_user_id_.end()
               ? nullptr
               : find_by_session(session->second);
}

const User* UserManager::find_by_user_id(std::uint64_t user_id) const {
    const auto session = session_by_user_id_.find(user_id);
    return session == session_by_user_id_.end()
               ? nullptr
               : find_by_session(session->second);
}

std::optional<User> UserManager::remove(std::uint64_t session_id) {
    const auto iterator = users_by_session_.find(session_id);
    if (iterator == users_by_session_.end()) {
        return std::nullopt;
    }
    User removed = std::move(iterator->second);
    session_by_user_id_.erase(removed.user_id);
    users_by_session_.erase(iterator);
    return removed;
}

std::vector<std::uint64_t> UserManager::lobby_session_ids() const {
    std::vector<std::uint64_t> sessions;
    for (const auto& [session_id, user] : users_by_session_) {
        if (!user.room_id) {
            sessions.push_back(session_id);
        }
    }
    return sessions;
}

}  // namespace cardgame::server::lobby
