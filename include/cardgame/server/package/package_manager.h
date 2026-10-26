#pragma once

#include "cardgame/server/package/package_manifest.h"

#include <boost/json/array.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cardgame::server::package {

struct SyncPackage {
    std::string id;
    std::string version;
    std::string api_version;
    std::string repository_url;
    std::string commit;
    std::string manifest_sha256;
    std::vector<std::string> dependencies;
};

struct ActiveManifest {
    std::string manifest_id;
    std::vector<SyncPackage> packages;
    int sync_timeout_ms = 30000;
};

struct InstalledPackage {
    std::string repository_url;
    std::string commit;
    std::string manifest_sha256;
    std::filesystem::path directory;
    PackageManifest manifest;
    bool enabled = false;
};

struct RuntimePackageRoot {
    std::string id;
    std::string version;
    std::string commit;
    std::filesystem::path root;
    std::optional<std::filesystem::path> server_entry;
};

// 调用方在服务器启动期间按值取得一次快照。该对象不引用 registry.json，
// 后续管理员修改不会改变已创建游戏可见的资源包集合。
struct PackageGeneration {
    std::string manifest_id;
    std::vector<RuntimePackageRoot> packages;
};

class PackageManager final {
public:
    struct Options {
        std::filesystem::path root;
        std::string supported_api_version = "1.0";
        int sync_timeout_ms = 30000;
        // 仅供受控测试夹具；生产配置必须保持 false。
        bool allow_file_repositories_for_tests = false;
    };

    using ManifestChangedHandler = std::function<void(const std::string&)>;

    explicit PackageManager(Options options);

    // install 先在独立 staging 目录下载、锁定并验证；任何失败都不会改注册表。
    [[nodiscard]] const InstalledPackage& install(
        std::string repository_url, std::string commit);

    // desired_ids 是完整启用集合。依赖图、帧大小和持久化全部成功后才切换。
    [[nodiscard]] const ActiveManifest& set_enabled(
        const std::vector<std::string>& desired_ids);
    [[nodiscard]] const ActiveManifest& enable(std::string_view package_id);
    [[nodiscard]] const ActiveManifest& disable(std::string_view package_id);
    // 重新读取由 sgs-package-admin 原子替换的注册表；失败时恢复内存旧状态。
    void reload_registry();

    [[nodiscard]] const std::vector<InstalledPackage>& installed() const noexcept;
    [[nodiscard]] const ActiveManifest& active_manifest() const noexcept;
    [[nodiscard]] PackageGeneration freeze_generation() const;

    void set_manifest_changed_handler(ManifestChangedHandler handler);

    [[nodiscard]] static std::string calculate_manifest_id(
        const std::vector<SyncPackage>& packages);
    [[nodiscard]] static ActiveManifest validate_sync_manifest(
        std::vector<SyncPackage> packages,
        int sync_timeout_ms,
        bool allow_file_repositories_for_tests = false);

private:
    [[nodiscard]] InstalledPackage inspect_checkout(
        const std::filesystem::path& directory,
        std::string repository_url,
        std::string commit,
        bool enabled) const;
    [[nodiscard]] std::vector<std::size_t> resolve_enabled(
        const std::vector<std::string>& desired_ids) const;
    [[nodiscard]] ActiveManifest build_active(
        const std::vector<std::size_t>& order) const;
    [[nodiscard]] const ActiveManifest& set_enabled_locked(
        const std::vector<std::string>& desired_ids);
    void load_registry();
    void persist_registry(const std::vector<InstalledPackage>& packages) const;

    Options options_;
    std::filesystem::path installs_root_;
    std::filesystem::path staging_root_;
    std::filesystem::path registry_path_;
    std::filesystem::path registry_lock_path_;
    std::vector<InstalledPackage> installed_;
    ActiveManifest active_;
    ManifestChangedHandler on_manifest_changed_;
};

[[nodiscard]] boost::json::array sync_packages_json(
    const std::vector<SyncPackage>& packages);

}  // namespace cardgame::server::package
