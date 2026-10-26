#include "cardgame/server/package/package_manifest.h"

#include "cardgame/server/package/package_utils.h"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <fcntl.h>
#include <limits>
#include <regex>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <tuple>
#include <unistd.h>

namespace cardgame::server::package {
namespace {

constexpr std::size_t maximum_manifest_bytes = 1024 * 1024;
const std::regex package_id_pattern{
    R"(^[a-z][a-z0-9]*(\.[a-z][a-z0-9_-]*)+$)", std::regex::ECMAScript};
const std::regex semver_pattern{
    R"(^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?(?:\+[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?$)",
    std::regex::ECMAScript};
const std::regex api_pattern{R"(^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$)"};
const std::regex range_pattern{
    R"(^(?:\*|(?:=|>=|<=|>|<)?(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)(?:-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?(?: +(?:=|>=|<=|>|<)?(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)(?:-[0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*)?)*)$)"};

struct SemVer {
    std::string major;
    std::string minor;
    std::string patch;
    std::vector<std::string> prerelease;
};

[[noreturn]] void invalid(std::string message) {
    throw PackageError{"INVALID_PACKAGE_MANIFEST", std::move(message)};
}

bool within_root(
    const std::filesystem::path& canonical_root,
    const std::filesystem::path& candidate) {
    const std::filesystem::path resolved = std::filesystem::canonical(candidate);
    auto root = canonical_root.begin();
    auto value = resolved.begin();
    while (root != canonical_root.end() && value != resolved.end() && *root == *value) {
        ++root;
        ++value;
    }
    return root == canonical_root.end();
}

class ManifestDescriptor final {
public:
    explicit ManifestDescriptor(int descriptor) noexcept : descriptor_(descriptor) {}
    ~ManifestDescriptor() {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
    }

    ManifestDescriptor(const ManifestDescriptor&) = delete;
    ManifestDescriptor& operator=(const ManifestDescriptor&) = delete;

    [[nodiscard]] int get() const noexcept { return descriptor_; }
    [[nodiscard]] bool close() noexcept {
        const int descriptor = descriptor_;
        descriptor_ = -1;
        return ::close(descriptor) == 0;
    }

private:
    int descriptor_;
};

std::string read_manifest(
    const std::filesystem::path& package_root,
    const std::filesystem::path& path) {
    std::error_code status_error;
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(path, status_error);
    if ((!status_error && !std::filesystem::exists(status)) ||
        status_error == std::errc::no_such_file_or_directory) {
        throw PackageError{"PACKAGE_MANIFEST_MISSING", "manifest.json is missing"};
    }
    if (status_error) {
        throw PackageError{
            "PACKAGE_MANIFEST_READ_FAILED", "cannot inspect manifest.json"};
    }
    if (std::filesystem::is_symlink(status) ||
        !std::filesystem::is_regular_file(status)) {
        invalid("manifest.json must be a regular non-symlink file");
    }
    try {
        if (!within_root(std::filesystem::canonical(package_root), path)) {
            invalid("manifest.json escapes package root");
        }
    } catch (const std::filesystem::filesystem_error& error) {
        throw PackageError{
            "PACKAGE_MANIFEST_READ_FAILED",
            std::string{"cannot resolve manifest.json: "} + error.what()};
    }

    const int opened = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (opened < 0) {
        if (errno == ENOENT) {
            throw PackageError{"PACKAGE_MANIFEST_MISSING", "manifest.json is missing"};
        }
        throw PackageError{
            "PACKAGE_MANIFEST_READ_FAILED", "cannot open manifest.json safely"};
    }
    ManifestDescriptor descriptor{opened};

    struct stat metadata {};
    if (::fstat(descriptor.get(), &metadata) != 0 || !S_ISREG(metadata.st_mode)) {
        invalid("manifest.json must remain a regular file while reading");
    }

    std::string bytes;
    if (metadata.st_size > 0) {
        if (static_cast<std::uintmax_t>(metadata.st_size) > maximum_manifest_bytes) {
            invalid("manifest.json exceeds the 1 MiB validation limit");
        }
        bytes.reserve(static_cast<std::size_t>(metadata.st_size));
    }
    std::array<char, 8192> buffer{};
    while (true) {
        const ssize_t count = ::read(descriptor.get(), buffer.data(), buffer.size());
        if (count == 0) {
            break;
        }
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw PackageError{
                "PACKAGE_MANIFEST_READ_FAILED", "cannot read manifest.json"};
        }
        if (bytes.size() + static_cast<std::size_t>(count) > maximum_manifest_bytes) {
            invalid("manifest.json exceeds the 1 MiB validation limit");
        }
        bytes.append(buffer.data(), static_cast<std::size_t>(count));
    }
    if (!descriptor.close()) {
        throw PackageError{
            "PACKAGE_MANIFEST_READ_FAILED", "cannot close manifest.json after reading"};
    }
    if ((!bytes.empty() && bytes.starts_with("\xef\xbb\xbf")) || !is_valid_utf8(bytes)) {
        invalid("manifest.json must be UTF-8 without BOM");
    }
    return bytes;
}

bool exact_keys(
    const boost::json::object& object,
    std::initializer_list<std::string_view> allowed,
    std::initializer_list<std::string_view> required) {
    for (const std::string_view key : required) {
        if (!object.contains(key)) {
            return false;
        }
    }
    for (const auto& member : object) {
        if (std::find(allowed.begin(), allowed.end(), member.key()) == allowed.end()) {
            return false;
        }
    }
    return true;
}

std::string required_string(
    const boost::json::object& object, std::string_view key, std::size_t maximum) {
    const auto* value = object.if_contains(key);
    if (value == nullptr || !value->is_string()) {
        invalid(std::string{key} + " must be a string");
    }
    const std::string result{value->as_string()};
    std::size_t length = 0;
    if (!is_valid_utf8(result, &length) || length == 0 || length > maximum) {
        invalid(std::string{key} + " has an invalid Unicode length");
    }
    return result;
}

bool valid_relative_path(std::string_view text, std::string_view suffix = {}) {
    std::size_t length = 0;
    if (!is_valid_utf8(text, &length) || length == 0 || length > 240 ||
        text.front() == '/' || text.find('\\') != std::string_view::npos ||
        text.find('\0') != std::string_view::npos ||
        (!suffix.empty() && !text.ends_with(suffix))) {
        return false;
    }
    const std::size_t colon = text.find(':');
    if (colon != std::string_view::npos && colon > 0 &&
        std::isalpha(static_cast<unsigned char>(text.front()))) {
        bool scheme = true;
        for (std::size_t index = 1; index < colon; ++index) {
            const unsigned char ch = static_cast<unsigned char>(text[index]);
            if (!std::isalnum(ch) && ch != '+' && ch != '.' && ch != '-') {
                scheme = false;
                break;
            }
        }
        if (scheme) {
            return false;
        }
    }
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const std::size_t end = text.find('/', begin);
        const std::string_view segment = text.substr(
            begin, end == std::string_view::npos ? text.size() - begin : end - begin);
        if (segment == "." || segment == "..") {
            return false;
        }
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return true;
}

void validate_declared_path(
    const std::filesystem::path& root,
    std::string_view path,
    bool must_be_file) {
    const std::filesystem::path candidate = root / std::filesystem::path{path};
    std::error_code error;
    const auto status = std::filesystem::status(candidate, error);
    if (error || !std::filesystem::exists(status) ||
        (must_be_file && !std::filesystem::is_regular_file(status))) {
        invalid("declared package path does not exist: " + std::string{path});
    }
    if (!within_root(std::filesystem::canonical(root), candidate)) {
        invalid("declared package path escapes package root: " + std::string{path});
    }
}

std::vector<std::string> split(std::string_view text, char separator) {
    std::vector<std::string> values;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const std::size_t end = text.find(separator, begin);
        values.emplace_back(text.substr(
            begin, end == std::string_view::npos ? text.size() - begin : end - begin));
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return values;
}

SemVer parse_semver(std::string_view text) {
    std::cmatch match;
    const std::string owned{text};
    if (!std::regex_match(owned.c_str(), match, semver_pattern)) {
        throw PackageError{"INVALID_PACKAGE_VERSION", "invalid SemVer: " + owned};
    }
    SemVer version;
    version.major = match[1].str();
    version.minor = match[2].str();
    version.patch = match[3].str();
    if (match[4].matched) {
        version.prerelease = split(match[4].str(), '.');
    }
    return version;
}

bool numeric_identifier(std::string_view value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](char ch) {
        return ch >= '0' && ch <= '9';
    });
}

int compare_decimal(std::string_view left, std::string_view right) {
    const std::size_t left_nonzero = left.find_first_not_of('0');
    const std::size_t right_nonzero = right.find_first_not_of('0');
    left.remove_prefix(std::min(left_nonzero, left.size() - 1));
    right.remove_prefix(std::min(right_nonzero, right.size() - 1));
    if (left.size() != right.size()) return left.size() < right.size() ? -1 : 1;
    if (left == right) return 0;
    return left < right ? -1 : 1;
}

int compare(const SemVer& left, const SemVer& right) {
    for (const auto& [left_number, right_number] :
         std::array<std::pair<std::string_view, std::string_view>, 3>{
             std::pair{std::string_view{left.major}, std::string_view{right.major}},
             std::pair{std::string_view{left.minor}, std::string_view{right.minor}},
             std::pair{std::string_view{left.patch}, std::string_view{right.patch}}}) {
        const int core_result = compare_decimal(left_number, right_number);
        if (core_result != 0) return core_result;
    }
    if (left.prerelease.empty() || right.prerelease.empty()) {
        if (left.prerelease.empty() == right.prerelease.empty()) return 0;
        return left.prerelease.empty() ? 1 : -1;
    }
    const std::size_t count = std::min(left.prerelease.size(), right.prerelease.size());
    for (std::size_t index = 0; index < count; ++index) {
        const std::string& a = left.prerelease[index];
        const std::string& b = right.prerelease[index];
        const bool a_number = numeric_identifier(a);
        const bool b_number = numeric_identifier(b);
        if (a_number && b_number) {
            const int number_result = compare_decimal(a, b);
            if (number_result != 0) return number_result;
        } else if (a_number != b_number) {
            return a_number ? -1 : 1;
        } else if (a != b) {
            return a < b ? -1 : 1;
        }
    }
    if (left.prerelease.size() == right.prerelease.size()) return 0;
    return left.prerelease.size() < right.prerelease.size() ? -1 : 1;
}

bool api_compatible(std::string_view supported, std::string_view required) {
    std::cmatch supported_match;
    std::cmatch required_match;
    const std::string supported_text{supported};
    const std::string required_text{required};
    if (!std::regex_match(supported_text.c_str(), supported_match, api_pattern) ||
        !std::regex_match(required_text.c_str(), required_match, api_pattern)) {
        return false;
    }
    const std::string supported_minor = supported_match[2].str();
    const std::string required_minor = required_match[2].str();
    return supported_match[1].str() == required_match[1].str() &&
           (supported_minor.size() > required_minor.size() ||
            (supported_minor.size() == required_minor.size() &&
             supported_minor >= required_minor));
}

}  // namespace

PackageManifest load_and_validate_manifest(
    const std::filesystem::path& package_root,
    std::string_view supported_api_version,
    std::string* raw_sha256) {
    const std::filesystem::path manifest_path = package_root / "manifest.json";
    const std::string raw = read_manifest(package_root, manifest_path);
    if (raw_sha256 != nullptr) {
        *raw_sha256 = sha256(raw);
    }
    boost::system::error_code parse_error;
    boost::json::value parsed = boost::json::parse(raw, parse_error);
    if (parse_error || !parsed.is_object()) {
        invalid("manifest.json must contain a JSON object");
    }
    const boost::json::object& object = parsed.as_object();
    if (!exact_keys(
            object,
            {"schema_version", "id", "name", "version", "api_version",
             "dependencies", "server", "client", "resources"},
            {"schema_version", "id", "name", "version", "api_version",
             "dependencies"})) {
        invalid("manifest fields do not match Package Manifest v1 schema");
    }
    const auto* schema = object.if_contains("schema_version");
    if (schema == nullptr || !schema->is_int64()) {
        invalid("schema_version must be integer 1");
    }
    if (schema->as_int64() != 1) {
        throw PackageError{"UNSUPPORTED_PACKAGE_SCHEMA", "unsupported package schema_version"};
    }

    PackageManifest manifest;
    manifest.id = required_string(object, "id", 128);
    manifest.name = required_string(object, "name", 80);
    manifest.version = required_string(
        object, "version", std::numeric_limits<std::size_t>::max());
    manifest.api_version = required_string(
        object, "api_version", std::numeric_limits<std::size_t>::max());
    if (manifest.id.size() < 3 || !std::regex_match(manifest.id, package_id_pattern)) {
        invalid("id does not match Package Manifest v1");
    }
    if (!std::regex_match(manifest.version, semver_pattern)) {
        invalid("version is not SemVer 2.0.0");
    }
    if (!std::regex_match(manifest.api_version, api_pattern)) {
        invalid("api_version must be MAJOR.MINOR");
    }
    if (!api_compatible(supported_api_version, manifest.api_version)) {
        throw PackageError{"INCOMPATIBLE_PACKAGE_API", "package api_version is not supported"};
    }

    const auto* dependencies = object.if_contains("dependencies");
    if (dependencies == nullptr || !dependencies->is_array() ||
        dependencies->as_array().size() > 128) {
        invalid("dependencies must be an array with at most 128 entries");
    }
    std::set<std::string> dependency_ids;
    for (const auto& dependency_value : dependencies->as_array()) {
        if (!dependency_value.is_object() ||
            !exact_keys(dependency_value.as_object(), {"id", "version"}, {"id", "version"})) {
            invalid("dependency must contain exactly id and version");
        }
        Dependency dependency{
            required_string(dependency_value.as_object(), "id", 128),
            required_string(dependency_value.as_object(), "version", 128)};
        if (dependency.id.size() < 3 ||
            !std::regex_match(dependency.id, package_id_pattern) ||
            !std::regex_match(dependency.version_range, range_pattern)) {
            invalid("dependency id or version range is invalid");
        }
        if (!dependency_ids.insert(dependency.id).second) {
            invalid("dependency id is duplicated: " + dependency.id);
        }
        manifest.dependencies.push_back(std::move(dependency));
    }

    if (const auto* server = object.if_contains("server")) {
        if (!server->is_object() ||
            !exact_keys(server->as_object(), {"entry"}, {"entry"})) {
            invalid("server must contain exactly entry");
        }
        manifest.server_entry = required_string(server->as_object(), "entry", 240);
        if (!valid_relative_path(*manifest.server_entry, ".lua")) {
            invalid("server.entry is not a valid relative Lua path");
        }
    }
    if (const auto* client = object.if_contains("client")) {
        if (!client->is_object() ||
            !exact_keys(client->as_object(), {"qml_entry"}, {"qml_entry"})) {
            invalid("client must contain exactly qml_entry");
        }
        manifest.client_qml_entry =
            required_string(client->as_object(), "qml_entry", 240);
        if (!valid_relative_path(*manifest.client_qml_entry, ".qml")) {
            invalid("client.qml_entry is not a valid relative QML path");
        }
    }
    if (const auto* resources = object.if_contains("resources")) {
        if (!resources->is_array() || resources->as_array().empty() ||
            resources->as_array().size() > 256) {
            invalid("resources must contain 1..256 paths");
        }
        std::set<std::string> unique;
        for (const auto& resource : resources->as_array()) {
            if (!resource.is_string()) {
                invalid("resource path must be a string");
            }
            std::string path{resource.as_string()};
            if (!valid_relative_path(path) || !unique.insert(path).second) {
                invalid("resource path is invalid or duplicated");
            }
            manifest.resources.push_back(std::move(path));
        }
    }
    if (!manifest.server_entry && !manifest.client_qml_entry && manifest.resources.empty()) {
        invalid("package must declare server, client, or resources");
    }

    try {
        if (manifest.server_entry) {
            validate_declared_path(package_root, *manifest.server_entry, true);
        }
        if (manifest.client_qml_entry) {
            validate_declared_path(package_root, *manifest.client_qml_entry, true);
        }
        for (const std::string& resource : manifest.resources) {
            validate_declared_path(package_root, resource, false);
        }
    } catch (const std::filesystem::filesystem_error& error) {
        invalid(std::string{"failed to validate declared path: "} + error.what());
    }
    return manifest;
}

bool version_satisfies(std::string_view version_text, std::string_view range) {
    const SemVer version = parse_semver(version_text);
    if (range == "*") {
        return true;
    }
    std::istringstream input(std::string{range});
    std::string comparator;
    while (input >> comparator) {
        std::string operation;
        if (comparator.starts_with(">=") || comparator.starts_with("<=") ||
            comparator.starts_with("==")) {
            operation = comparator.substr(0, 2);
            comparator.erase(0, 2);
        } else if (!comparator.empty() &&
                   (comparator[0] == '>' || comparator[0] == '<' ||
                    comparator[0] == '=')) {
            operation = comparator.substr(0, 1);
            comparator.erase(0, 1);
        } else {
            operation = "=";
        }
        const int result = compare(version, parse_semver(comparator));
        const bool matched = operation == ">=" ? result >= 0 :
                             operation == "<=" ? result <= 0 :
                             operation == ">" ? result > 0 :
                             operation == "<" ? result < 0 : result == 0;
        if (!matched) {
            return false;
        }
    }
    return true;
}

}  // namespace cardgame::server::package
