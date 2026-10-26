#pragma once

#include "cardgame/server/package/package_manager.h"
#include "cardgame/server/protocol/message_codec.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

namespace cardgame::server::package {

class PackageSyncService final {
public:
    using ChangedHandler = std::function<void(const std::string&)>;

    explicit PackageSyncService(PackageManager& manager);

    [[nodiscard]] protocol::Message handle(
        std::uint64_t session_id, const protocol::Message& request);
    [[nodiscard]] bool is_ready(std::uint64_t session_id) const;
    void disconnect(std::uint64_t session_id);
    void set_changed_handler(ChangedHandler handler);

private:
    [[nodiscard]] protocol::Message get_manifest(
        const protocol::Message& request) const;
    [[nodiscard]] protocol::Message package_ready(
        std::uint64_t session_id, const protocol::Message& request);

    PackageManager& manager_;
    std::unordered_map<std::uint64_t, std::string> ready_manifest_by_session_;
    ChangedHandler on_changed_;
};

}  // namespace cardgame::server::package
