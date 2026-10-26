#pragma once

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace cardgame::server::package {

struct PackageError final : std::runtime_error {
    PackageError(std::string error_code, std::string message)
        : std::runtime_error(std::move(message)), code(std::move(error_code)) {}

    std::string code;
};

struct Dependency {
    std::string id;
    std::string version_range;
};

struct PackageManifest {
    std::string id;
    std::string name;
    std::string version;
    std::string api_version;
    std::vector<Dependency> dependencies;
    std::optional<std::string> server_entry;
    std::optional<std::string> client_qml_entry;
    std::vector<std::string> resources;
};

// 对 manifest.json 做等价于 package-manifest.schema.json 的严格检查，
// 再检查声明路径和符号链接均留在 package_root 内。
[[nodiscard]] PackageManifest load_and_validate_manifest(
    const std::filesystem::path& package_root,
    std::string_view supported_api_version,
    std::string* raw_sha256 = nullptr);

[[nodiscard]] bool version_satisfies(
    std::string_view version, std::string_view range);

}  // namespace cardgame::server::package
