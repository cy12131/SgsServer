#include "cardgame/server/package/package_manager.h"

#include "cardgame/server/package/package_utils.h"
#include "cardgame/server/protocol/message_codec.h"

#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <limits>
#include <regex>
#include <set>
#include <sstream>
#include <system_error>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace cardgame::server::package {
namespace {

constexpr std::string_view empty_manifest_id{
    "4f53cda18c2baa0c0354bb5f9a3ecbe5ed12ab4d8e11ba873c2f11161202b945"};
const std::regex production_url_pattern{R"(^(?:https|ssh)://[^[:space:]]+$)"};
const std::regex file_url_pattern{R"(^file://[^[:space:]]+$)"};
const std::regex sync_package_id_pattern{
    R"(^[a-z][a-z0-9]*(\.[a-z][a-z0-9_-]*)+$)"};
const std::regex sync_semver_pattern{
    R"(^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$)"};
const std::regex sync_api_pattern{R"(^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$)"};

struct ProcessResult {
    int exit_code = -1;
    std::string output;
};

std::string trim(std::string value) {
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r' ||
                              value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    return value;
}

ProcessResult run_process(const std::vector<std::string>& arguments) {
    if (arguments.empty()) {
        throw PackageError{"GIT_FAILED", "empty process argument vector"};
    }
    std::array<int, 2> pipe_descriptors{};
    if (::pipe2(pipe_descriptors.data(), O_CLOEXEC) != 0) {
        throw PackageError{"GIT_FAILED", "cannot create Git output pipe"};
    }
    const pid_t child = ::fork();
    if (child < 0) {
        ::close(pipe_descriptors[0]);
        ::close(pipe_descriptors[1]);
        throw PackageError{"GIT_FAILED", "cannot fork Git process"};
    }
    if (child == 0) {
        ::dup2(pipe_descriptors[1], STDOUT_FILENO);
        ::dup2(pipe_descriptors[1], STDERR_FILENO);
        ::close(pipe_descriptors[0]);
        ::close(pipe_descriptors[1]);
        ::setenv("GIT_TERMINAL_PROMPT", "0", 1);
        ::setenv("GIT_CONFIG_NOSYSTEM", "1", 1);
        ::setenv("GIT_CONFIG_GLOBAL", "/dev/null", 1);
        std::vector<char*> argv;
        argv.reserve(arguments.size() + 1);
        for (const std::string& argument : arguments) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        ::execvp(argv.front(), argv.data());
        _exit(127);
    }

    ::close(pipe_descriptors[1]);
    ProcessResult result;
    std::array<char, 4096> buffer{};
    while (true) {
        const ssize_t count = ::read(pipe_descriptors[0], buffer.data(), buffer.size());
        if (count == 0) {
            break;
        }
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (result.output.size() < 65536) {
            const std::size_t accepted = std::min<std::size_t>(
                static_cast<std::size_t>(count), 65536 - result.output.size());
            result.output.append(buffer.data(), accepted);
        }
    }
    ::close(pipe_descriptors[0]);
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    }
    return result;
}

void require_process(const std::vector<std::string>& arguments, std::string_view operation) {
    const ProcessResult result = run_process(arguments);
    if (result.exit_code != 0) {
        throw PackageError{
            "GIT_FAILED",
            std::string{operation} + " failed (exit " +
                std::to_string(result.exit_code) + "): " + trim(result.output)};
    }
}

std::vector<std::string> git_base(bool allow_file) {
    return {"git", "-c", "core.hooksPath=/dev/null", "-c",
            allow_file ? "protocol.file.allow=always" : "protocol.file.allow=never",
            "-c", "protocol.ext.allow=never"};
}

void secure_clone(
    const std::string& repository_url,
    const std::string& commit,
    const std::filesystem::path& destination,
    bool allow_file) {
    std::vector<std::string> clone = git_base(allow_file);
    clone.insert(clone.end(), {"clone", "--no-checkout", "--no-local", "--",
                               repository_url, destination.string()});
    require_process(clone, "git clone");

    std::vector<std::string> checkout = git_base(allow_file);
    checkout.insert(checkout.end(), {"-C", destination.string(), "checkout", "--detach", commit});
    require_process(checkout, "git checkout");

    std::vector<std::string> verify = git_base(allow_file);
    verify.insert(verify.end(), {"-C", destination.string(), "rev-parse", "HEAD"});
    const ProcessResult result = run_process(verify);
    if (result.exit_code != 0 || trim(result.output) != commit) {
        throw PackageError{"GIT_COMMIT_MISMATCH", "checked out Git commit does not match lock"};
    }
}

std::filesystem::path new_staging_path(const std::filesystem::path& root) {
    static std::uint64_t counter = 0;
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return root / (std::to_string(::getpid()) + "-" + std::to_string(ticks) + "-" +
                   std::to_string(++counter));
}

class StagingGuard final {
public:
    explicit StagingGuard(std::filesystem::path path) : path_(std::move(path)) {}
    ~StagingGuard() {
        if (!released_) {
            std::error_code ignored;
            std::filesystem::remove_all(path_, ignored);
        }
    }
    void release() noexcept { released_ = true; }

private:
    std::filesystem::path path_;
    bool released_ = false;
};

boost::json::object sync_package_json(const SyncPackage& package) {
    boost::json::array dependencies;
    for (const std::string& dependency : package.dependencies) {
        dependencies.emplace_back(dependency);
    }
    return {{"id", package.id},
            {"version", package.version},
            {"api_version", package.api_version},
            {"repository_url", package.repository_url},
            {"commit", package.commit},
            {"manifest_sha256", package.manifest_sha256},
            {"dependencies", std::move(dependencies)}};
}

bool valid_repository_url(std::string_view url, bool allow_file) {
    const std::string owned{url};
    bool production = std::regex_match(owned, production_url_pattern);
    if (production) {
        production = std::all_of(url.begin(), url.end(), [](unsigned char ch) {
            return ch >= 0x21U && ch <= 0x7eU;
        });
        const std::size_t authority_begin = owned.starts_with("https://") ? 8 : 6;
        const std::size_t authority_end = owned.find_first_of("/?#", authority_begin);
        const std::string_view authority = std::string_view{owned}.substr(
            authority_begin,
            authority_end == std::string::npos
                ? std::string::npos
                : authority_end - authority_begin);
        const std::size_t at = authority.rfind('@');
        if (owned.starts_with("https://") && at != std::string_view::npos) {
            production = false;  // v1 不持久化或下发 URL 用户信息/凭据。
        }
        const std::string_view host_port =
            at == std::string_view::npos ? authority : authority.substr(at + 1);
        if (authority.empty() || host_port.empty()) {
            production = false;
        } else if (host_port.front() == '[') {
            const std::size_t bracket = host_port.find(']');
            if (bracket == std::string_view::npos || bracket == 1 ||
                (bracket + 1 < host_port.size() && host_port[bracket + 1] != ':')) {
                production = false;
            }
        } else {
            const std::size_t colon = host_port.rfind(':');
            const std::string_view host = host_port.substr(0, colon);
            if (host.empty() || !std::all_of(host.begin(), host.end(), [](unsigned char ch) {
                    return std::isalnum(ch) || ch == '.' || ch == '-' || ch == '%';
                })) {
                production = false;
            }
        }
    }
    return production ||
           (allow_file && std::regex_match(owned, file_url_pattern));
}

bool exact_keys(
    const boost::json::object& object,
    std::initializer_list<std::string_view> allowed,
    std::initializer_list<std::string_view> required) {
    for (const std::string_view key : required) {
        if (!object.contains(key)) return false;
    }
    for (const auto& member : object) {
        if (std::find(allowed.begin(), allowed.end(), member.key()) == allowed.end()) {
            return false;
        }
    }
    return true;
}

}  // namespace

boost::json::array sync_packages_json(const std::vector<SyncPackage>& packages) {
    boost::json::array result;
    result.reserve(packages.size());
    for (const SyncPackage& package : packages) {
        result.push_back(sync_package_json(package));
    }
    return result;
}

PackageManager::PackageManager(Options options)
    : options_(std::move(options)),
      installs_root_(options_.root / "installed"),
      staging_root_(options_.root / ".staging"),
      registry_path_(options_.root / "registry.json") {
    if (options_.root.empty() || options_.sync_timeout_ms < 1000 ||
        options_.sync_timeout_ms > 1800000) {
        throw PackageError{"INVALID_PACKAGE_CONFIGURATION", "invalid package manager options"};
    }
    std::filesystem::create_directories(installs_root_);
    std::filesystem::create_directories(staging_root_);
    load_registry();
}

const InstalledPackage& PackageManager::install(
    std::string repository_url, std::string commit) {
    if (!valid_repository_url(
            repository_url, options_.allow_file_repositories_for_tests)) {
        throw PackageError{"INVALID_REPOSITORY_URL", "repository URL must use https:// or ssh://"};
    }
    if (!is_lower_hex_hash(commit, 40) && !is_lower_hex_hash(commit, 64)) {
        throw PackageError{"INVALID_GIT_COMMIT", "commit must be a full lowercase Git object ID"};
    }

    const std::filesystem::path staging = new_staging_path(staging_root_);
    StagingGuard guard{staging};
    secure_clone(repository_url, commit, staging,
                 options_.allow_file_repositories_for_tests);
    InstalledPackage inspected = inspect_checkout(
        staging, std::move(repository_url), std::move(commit), false);
    for (const InstalledPackage& existing : installed_) {
        if (existing.manifest.id == inspected.manifest.id) {
            if (existing.repository_url == inspected.repository_url &&
                existing.commit == inspected.commit) {
                return existing;
            }
            throw PackageError{
                "DUPLICATE_PACKAGE_ID",
                "package id is already registered: " + inspected.manifest.id};
        }
    }

    const std::filesystem::path relative =
        std::filesystem::path{inspected.manifest.id} / inspected.commit;
    const std::filesystem::path final_directory = installs_root_ / relative;
    if (std::filesystem::exists(final_directory)) {
        throw PackageError{"PACKAGE_INSTALL_CONFLICT", "package install directory already exists"};
    }
    std::filesystem::create_directories(final_directory.parent_path());
    std::filesystem::rename(staging, final_directory);
    guard.release();
    inspected.directory = final_directory;
    std::vector<InstalledPackage> candidate = installed_;
    candidate.push_back(inspected);
    try {
        persist_registry(candidate);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(final_directory, ignored);
        throw;
    }
    installed_ = std::move(candidate);
    return installed_.back();
}

const ActiveManifest& PackageManager::set_enabled(
    const std::vector<std::string>& desired_ids) {
    const std::vector<std::size_t> order = resolve_enabled(desired_ids);
    ActiveManifest candidate_manifest = build_active(order);
    std::set<std::string> desired(desired_ids.begin(), desired_ids.end());
    if (desired.size() != desired_ids.size()) {
        throw PackageError{"DUPLICATE_PACKAGE_ID", "enabled package id is duplicated"};
    }
    std::vector<InstalledPackage> candidate = installed_;
    for (InstalledPackage& package : candidate) {
        package.enabled = desired.contains(package.manifest.id);
    }
    persist_registry(candidate);
    const bool changed = candidate_manifest.manifest_id != active_.manifest_id;
    installed_ = std::move(candidate);
    active_ = std::move(candidate_manifest);
    if (changed && on_manifest_changed_) {
        on_manifest_changed_(active_.manifest_id);
    }
    return active_;
}

const ActiveManifest& PackageManager::enable(std::string_view package_id) {
    std::vector<std::string> desired;
    for (const InstalledPackage& package : installed_) {
        if (package.enabled || package.manifest.id == package_id) {
            desired.push_back(package.manifest.id);
        }
    }
    if (std::find(desired.begin(), desired.end(), package_id) == desired.end()) {
        throw PackageError{"PACKAGE_NOT_INSTALLED", "package is not installed"};
    }
    return set_enabled(desired);
}

const ActiveManifest& PackageManager::disable(std::string_view package_id) {
    std::vector<std::string> desired;
    bool found = false;
    for (const InstalledPackage& package : installed_) {
        if (package.manifest.id == package_id) {
            found = true;
        } else if (package.enabled) {
            desired.push_back(package.manifest.id);
        }
    }
    if (!found) {
        throw PackageError{"PACKAGE_NOT_INSTALLED", "package is not installed"};
    }
    return set_enabled(desired);
}

void PackageManager::reload_registry() {
    std::vector<InstalledPackage> previous_installed = installed_;
    ActiveManifest previous_active = active_;
    try {
        load_registry();
    } catch (...) {
        installed_ = std::move(previous_installed);
        active_ = std::move(previous_active);
        throw;
    }
    if (active_.manifest_id != previous_active.manifest_id && on_manifest_changed_) {
        on_manifest_changed_(active_.manifest_id);
    }
}

const std::vector<InstalledPackage>& PackageManager::installed() const noexcept {
    return installed_;
}

const ActiveManifest& PackageManager::active_manifest() const noexcept {
    return active_;
}

void PackageManager::set_manifest_changed_handler(ManifestChangedHandler handler) {
    on_manifest_changed_ = std::move(handler);
}

std::string PackageManager::calculate_manifest_id(
    const std::vector<SyncPackage>& packages) {
    std::vector<SyncPackage> sorted = packages;
    std::sort(sorted.begin(), sorted.end(), [](const SyncPackage& left, const SyncPackage& right) {
        return left.id < right.id;
    });
    std::set<std::string> ids;
    boost::json::array canonical_packages;
    for (SyncPackage& package : sorted) {
        if (!ids.insert(package.id).second) {
            throw PackageError{"DUPLICATE_PACKAGE_ID", "sync manifest has duplicate package id"};
        }
        std::sort(package.dependencies.begin(), package.dependencies.end());
        if (std::adjacent_find(package.dependencies.begin(), package.dependencies.end()) !=
            package.dependencies.end()) {
            throw PackageError{"DUPLICATE_DEPENDENCY", "sync manifest has duplicate dependency id"};
        }
        canonical_packages.push_back(sync_package_json(package));
    }
    const std::string canonical = canonicalize_json(canonical_packages);
    const std::string digest = sha256(canonical);
    if (packages.empty() && digest != empty_manifest_id) {
        throw PackageError{"MANIFEST_DIGEST_FAILED", "empty manifest vector mismatch"};
    }
    return digest;
}

ActiveManifest PackageManager::validate_sync_manifest(
    std::vector<SyncPackage> packages,
    int sync_timeout_ms,
    bool allow_file_repositories_for_tests) {
    if (packages.size() > 256 || sync_timeout_ms < 1000 || sync_timeout_ms > 1800000) {
        throw PackageError{"INVALID_PACKAGE_CONFIGURATION", "sync manifest limits are invalid"};
    }
    for (const SyncPackage& package : packages) {
        if (package.id.size() < 3 || package.id.size() > 128 ||
            !std::regex_match(package.id, sync_package_id_pattern) ||
            !std::regex_match(package.version, sync_semver_pattern) ||
            !std::regex_match(package.api_version, sync_api_pattern) ||
            !valid_repository_url(
                package.repository_url, allow_file_repositories_for_tests) ||
            (!is_lower_hex_hash(package.commit, 40) &&
             !is_lower_hex_hash(package.commit, 64)) ||
            !is_lower_hex_hash(package.manifest_sha256, 64) ||
            package.dependencies.size() > 128) {
            throw PackageError{"INVALID_PACKAGE_MANIFEST",
                               "sync package fields violate Package Sync v1"};
        }
        for (const std::string& dependency : package.dependencies) {
            if (dependency.size() < 3 || dependency.size() > 128 ||
                !std::regex_match(dependency, sync_package_id_pattern)) {
                throw PackageError{"INVALID_PACKAGE_MANIFEST",
                                   "sync dependency id is invalid"};
            }
        }
    }
    ActiveManifest result;
    result.manifest_id = calculate_manifest_id(packages);
    result.packages = std::move(packages);
    result.sync_timeout_ms = sync_timeout_ms;

    protocol::Message response;
    response.type = "PackageManifestResult";
    response.request_id = std::numeric_limits<std::int32_t>::max();
    response.data = {{"manifest_id", result.manifest_id},
                     {"packages", sync_packages_json(result.packages)},
                     {"sync_timeout_ms", result.sync_timeout_ms}};
    try {
        static_cast<void>(protocol::MessageCodec::encode_frame(response));
    } catch (const std::length_error&) {
        throw PackageError{
            "PACKAGE_MANIFEST_TOO_LARGE",
            "active package manifest exceeds the SGS/1 65536-byte body limit"};
    }
    return result;
}

InstalledPackage PackageManager::inspect_checkout(
    const std::filesystem::path& directory,
    std::string repository_url,
    std::string commit,
    bool enabled) const {
    if (!valid_repository_url(
            repository_url, options_.allow_file_repositories_for_tests) ||
        (!is_lower_hex_hash(commit, 40) && !is_lower_hex_hash(commit, 64))) {
        throw PackageError{"INVALID_PACKAGE_REGISTRY", "invalid repository URL or commit lock"};
    }
    std::vector<std::string> verify = git_base(
        options_.allow_file_repositories_for_tests);
    verify.insert(verify.end(), {"-C", directory.string(), "rev-parse", "HEAD"});
    const ProcessResult result = run_process(verify);
    if (result.exit_code != 0 || trim(result.output) != commit) {
        throw PackageError{"GIT_COMMIT_MISMATCH", "installed checkout does not match registry commit"};
    }
    std::string manifest_sha256;
    PackageManifest manifest = load_and_validate_manifest(
        directory, options_.supported_api_version, &manifest_sha256);
    return {std::move(repository_url), std::move(commit), std::move(manifest_sha256),
            directory, std::move(manifest), enabled};
}

std::vector<std::size_t> PackageManager::resolve_enabled(
    const std::vector<std::string>& desired_ids) const {
    std::map<std::string, std::size_t> by_id;
    for (std::size_t index = 0; index < installed_.size(); ++index) {
        if (!by_id.emplace(installed_[index].manifest.id, index).second) {
            throw PackageError{"DUPLICATE_PACKAGE_ID", "installed package id is duplicated"};
        }
    }
    std::set<std::string> desired(desired_ids.begin(), desired_ids.end());
    if (desired.size() != desired_ids.size()) {
        throw PackageError{"DUPLICATE_PACKAGE_ID", "enabled package id is duplicated"};
    }
    for (const std::string& id : desired) {
        if (!by_id.contains(id)) {
            throw PackageError{"PACKAGE_NOT_INSTALLED", "enabled package is not installed: " + id};
        }
    }

    enum class Visit { unseen, visiting, complete };
    std::map<std::string, Visit> visits;
    std::vector<std::size_t> order;
    std::function<void(const std::string&)> visit = [&](const std::string& id) {
        if (visits[id] == Visit::visiting) {
            throw PackageError{"CYCLIC_DEPENDENCY", "cyclic dependency at " + id};
        }
        if (visits[id] == Visit::complete) return;
        visits[id] = Visit::visiting;
        const InstalledPackage& package = installed_[by_id.at(id)];
        std::vector<Dependency> dependencies = package.manifest.dependencies;
        std::sort(dependencies.begin(), dependencies.end(),
                  [](const Dependency& left, const Dependency& right) {
                      return left.id < right.id;
                  });
        for (const Dependency& dependency : dependencies) {
            const auto found = by_id.find(dependency.id);
            if (found == by_id.end() || !desired.contains(dependency.id)) {
                throw PackageError{
                    "MISSING_DEPENDENCY", id + " requires " + dependency.id};
            }
            const InstalledPackage& required = installed_[found->second];
            if (!version_satisfies(required.manifest.version, dependency.version_range)) {
                throw PackageError{
                    "DEPENDENCY_VERSION_MISMATCH",
                    id + " requires " + dependency.id + " " + dependency.version_range};
            }
            visit(dependency.id);
        }
        visits[id] = Visit::complete;
        order.push_back(by_id.at(id));
    };
    for (const std::string& id : desired) {
        visit(id);
    }
    return order;
}

ActiveManifest PackageManager::build_active(
    const std::vector<std::size_t>& order) const {
    std::vector<SyncPackage> packages;
    packages.reserve(order.size());
    for (const std::size_t index : order) {
        const InstalledPackage& installed = installed_.at(index);
        SyncPackage package{installed.manifest.id,
                            installed.manifest.version,
                            installed.manifest.api_version,
                            installed.repository_url,
                            installed.commit,
                            installed.manifest_sha256,
                            {}};
        for (const Dependency& dependency : installed.manifest.dependencies) {
            package.dependencies.push_back(dependency.id);
        }
        std::sort(package.dependencies.begin(), package.dependencies.end());
        packages.push_back(std::move(package));
    }
    return validate_sync_manifest(
        std::move(packages), options_.sync_timeout_ms,
        options_.allow_file_repositories_for_tests);
}

void PackageManager::load_registry() {
    if (!std::filesystem::exists(registry_path_)) {
        installed_.clear();
        active_ = validate_sync_manifest({}, options_.sync_timeout_ms);
        return;
    }
    std::ifstream input(registry_path_, std::ios::binary);
    std::ostringstream bytes;
    bytes << input.rdbuf();
    boost::system::error_code error;
    boost::json::value parsed = boost::json::parse(bytes.str(), error);
    if (error || !parsed.is_object()) {
        throw PackageError{"INVALID_PACKAGE_REGISTRY", "registry.json is invalid JSON"};
    }
    const boost::json::object& root = parsed.as_object();
    if (!exact_keys(root, {"schema_version", "packages"}, {"schema_version", "packages"}) ||
        !root.at("schema_version").is_int64() || root.at("schema_version").as_int64() != 1 ||
        !root.at("packages").is_array()) {
        throw PackageError{"INVALID_PACKAGE_REGISTRY", "registry.json has invalid fields"};
    }
    std::vector<InstalledPackage> loaded;
    for (const auto& item : root.at("packages").as_array()) {
        if (!item.is_object() ||
            !exact_keys(item.as_object(),
                        {"id", "repository_url", "commit", "manifest_sha256",
                         "directory", "enabled"},
                        {"id", "repository_url", "commit", "manifest_sha256",
                         "directory", "enabled"})) {
            throw PackageError{"INVALID_PACKAGE_REGISTRY", "registry package entry is invalid"};
        }
        const auto& object = item.as_object();
        if (!object.at("id").is_string() || !object.at("repository_url").is_string() ||
            !object.at("commit").is_string() ||
            !object.at("manifest_sha256").is_string() ||
            !object.at("directory").is_string() || !object.at("enabled").is_bool()) {
            throw PackageError{"INVALID_PACKAGE_REGISTRY", "registry package types are invalid"};
        }
        const std::filesystem::path relative{std::string{object.at("directory").as_string()}};
        if (relative.empty() || relative.is_absolute() ||
            relative.lexically_normal().string().starts_with("..")) {
            throw PackageError{"INVALID_PACKAGE_REGISTRY", "registry path escapes package root"};
        }
        InstalledPackage inspected = inspect_checkout(
            installs_root_ / relative,
            std::string{object.at("repository_url").as_string()},
            std::string{object.at("commit").as_string()},
            object.at("enabled").as_bool());
        if (inspected.manifest.id != object.at("id").as_string()) {
            throw PackageError{"INVALID_PACKAGE_REGISTRY", "registry id differs from manifest"};
        }
        if (inspected.manifest_sha256 != object.at("manifest_sha256").as_string()) {
            throw PackageError{"PACKAGE_MANIFEST_HASH_MISMATCH",
                               "installed manifest.json differs from registry hash"};
        }
        loaded.push_back(std::move(inspected));
    }
    installed_ = std::move(loaded);
    std::vector<std::string> enabled;
    for (const InstalledPackage& package : installed_) {
        if (package.enabled) enabled.push_back(package.manifest.id);
    }
    active_ = build_active(resolve_enabled(enabled));
}

void PackageManager::persist_registry(
    const std::vector<InstalledPackage>& packages) const {
    boost::json::array entries;
    for (const InstalledPackage& package : packages) {
        std::filesystem::path relative = std::filesystem::relative(
            package.directory, installs_root_);
        entries.push_back({{"id", package.manifest.id},
                           {"repository_url", package.repository_url},
                           {"commit", package.commit},
                           {"manifest_sha256", package.manifest_sha256},
                           {"directory", relative.generic_string()},
                           {"enabled", package.enabled}});
    }
    const std::string serialized = boost::json::serialize(
        boost::json::object{{"schema_version", 1}, {"packages", std::move(entries)}});
    const std::filesystem::path temporary = registry_path_.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
        output.flush();
        if (!output) {
            throw PackageError{"REGISTRY_WRITE_FAILED", "cannot write package registry"};
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, registry_path_, error);
    if (error) {
        std::filesystem::remove(temporary);
        throw PackageError{"REGISTRY_WRITE_FAILED", "cannot atomically replace package registry"};
    }
}

}  // namespace cardgame::server::package
