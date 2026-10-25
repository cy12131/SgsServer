#include "cardgame/server/package/package_sync_service.h"

#include "cardgame/server/package/package_utils.h"

#include <boost/json/object.hpp>

#include <algorithm>

namespace cardgame::server::package {
namespace {

protocol::Message invalid(std::int32_t request_id, std::string message) {
    protocol::Message response;
    response.type = "Error";
    response.request_id = request_id;
    response.data = {{"code", "INVALID_MESSAGE"}, {"message", std::move(message)}};
    return response;
}

bool only_keys(
    const boost::json::object& object,
    std::initializer_list<std::string_view> allowed) {
    for (const auto& member : object) {
        if (std::find(allowed.begin(), allowed.end(), member.key()) == allowed.end()) {
            return false;
        }
    }
    return true;
}

}  // namespace

PackageSyncService::PackageSyncService(PackageManager& manager) : manager_(manager) {
    manager_.set_manifest_changed_handler([this](const std::string& manifest_id) {
        ready_manifest_by_session_.clear();
        if (on_changed_) {
            on_changed_(manifest_id);
        }
    });
}

protocol::Message PackageSyncService::handle(
    std::uint64_t session_id, const protocol::Message& request) {
    if (request.type == "GetPackageManifest") {
        return get_manifest(request);
    }
    return package_ready(session_id, request);
}

bool PackageSyncService::is_ready(std::uint64_t session_id) const {
    const auto found = ready_manifest_by_session_.find(session_id);
    return found != ready_manifest_by_session_.end() &&
           found->second == manager_.active_manifest().manifest_id;
}

void PackageSyncService::disconnect(std::uint64_t session_id) {
    ready_manifest_by_session_.erase(session_id);
}

void PackageSyncService::set_changed_handler(ChangedHandler handler) {
    on_changed_ = std::move(handler);
}

protocol::Message PackageSyncService::get_manifest(
    const protocol::Message& request) const {
    if (!only_keys(request.data, {"known_manifest_id"})) {
        return invalid(request.request_id, "Invalid GetPackageManifest data");
    }
    if (const auto* known = request.data.if_contains("known_manifest_id");
        known != nullptr &&
        (!known->is_string() ||
         !is_lower_hex_hash(std::string_view{known->as_string()}, 64))) {
        return invalid(request.request_id, "known_manifest_id must be a SHA-256 digest");
    }
    const ActiveManifest& active = manager_.active_manifest();
    protocol::Message response;
    response.type = "PackageManifestResult";
    response.request_id = request.request_id;
    response.data = {{"manifest_id", active.manifest_id},
                     {"packages", sync_packages_json(active.packages)},
                     {"sync_timeout_ms", active.sync_timeout_ms}};
    return response;
}

protocol::Message PackageSyncService::package_ready(
    std::uint64_t session_id, const protocol::Message& request) {
    if (!only_keys(request.data, {"manifest_id"})) {
        return invalid(request.request_id, "Invalid PackageReady data");
    }
    const auto* digest = request.data.if_contains("manifest_id");
    if (digest == nullptr || !digest->is_string() ||
        !is_lower_hex_hash(std::string_view{digest->as_string()}, 64)) {
        return invalid(request.request_id, "manifest_id must be a SHA-256 digest");
    }
    protocol::Message response;
    response.type = "PackageReadyResult";
    response.request_id = request.request_id;
    if (digest->as_string() != manager_.active_manifest().manifest_id) {
        ready_manifest_by_session_.erase(session_id);
        response.data = {
            {"ready", false},
            {"error",
             {{"code", "MANIFEST_ID_MISMATCH"},
              {"message",
               "Client package manifest does not match the current server requirement"},
              {"retryable", true}}}};
        return response;
    }
    ready_manifest_by_session_[session_id] = std::string{digest->as_string()};
    response.data = {{"ready", true}};
    return response;
}

}  // namespace cardgame::server::package
