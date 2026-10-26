#include "cardgame/server/package/package_manager.h"
#include "cardgame/server/package/package_sync_service.h"
#include "cardgame/server/package/package_utils.h"

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>

#include <array>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

using cardgame::server::package::PackageError;
using cardgame::server::package::PackageManager;
using cardgame::server::package::SyncPackage;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error{message};
    }
}

void write_file(const std::filesystem::path& path, std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!output) {
        throw std::runtime_error{"cannot write fixture: " + path.string()};
    }
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void replace_registry_directory(
    const std::filesystem::path& registry,
    std::string directory) {
    boost::json::value parsed = boost::json::parse(read_file(registry));
    parsed.as_object().at("packages").as_array().front().as_object()["directory"] =
        std::move(directory);
    write_file(registry, boost::json::serialize(parsed));
}

std::string run(const std::vector<std::string>& arguments) {
    std::array<int, 2> descriptors{};
    if (::pipe(descriptors.data()) != 0) throw std::runtime_error{"pipe failed"};
    const pid_t child = ::fork();
    if (child == 0) {
        ::dup2(descriptors[1], STDOUT_FILENO);
        ::dup2(descriptors[1], STDERR_FILENO);
        ::close(descriptors[0]);
        ::close(descriptors[1]);
        std::vector<char*> argv;
        for (const std::string& argument : arguments) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        ::execvp(argv[0], argv.data());
        _exit(127);
    }
    if (child < 0) throw std::runtime_error{"fork failed"};
    ::close(descriptors[1]);
    std::string output;
    std::array<char, 1024> buffer{};
    while (true) {
        const ssize_t count = ::read(descriptors[0], buffer.data(), buffer.size());
        if (count == 0) break;
        if (count < 0) {
            if (errno == EINTR) continue;
            break;
        }
        output.append(buffer.data(), static_cast<std::size_t>(count));
    }
    ::close(descriptors[0]);
    int status = 0;
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        throw std::runtime_error{"fixture command failed: " + output};
    }
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
        output.pop_back();
    }
    return output;
}

std::string create_repository(
    const std::filesystem::path& root,
    std::string_view name,
    std::string_view manifest,
    bool create_manifest = true) {
    const std::filesystem::path repository = root / std::string{name};
    std::filesystem::create_directories(repository);
    if (create_manifest) {
        write_file(repository / "manifest.json", manifest);
    } else {
        write_file(repository / "README.md", "missing manifest fixture\n");
    }
    write_file(repository / "server/init.lua", "return {}\n");
    write_file(repository / "client/Main.qml", "import QtQuick\nItem {}\n");
    write_file(repository / "assets/data.txt", "fixture\n");
    run({"git", "-C", repository.string(), "init", "-b", "main"});
    run({"git", "-C", repository.string(), "config", "user.name", "SGS Test"});
    run({"git", "-C", repository.string(), "config", "user.email", "test@invalid"});
    run({"git", "-C", repository.string(), "add", "."});
    run({"git", "-C", repository.string(), "commit", "-m", "fixture"});
    return run({"git", "-C", repository.string(), "rev-parse", "HEAD"});
}

std::string manifest(
    std::string_view id,
    std::string_view version = "1.0.0",
    std::string_view dependencies = "[]",
    std::string_view extra = "\"server\":{\"entry\":\"server/init.lua\"}") {
    return "{\"schema_version\":1,\"id\":\"" + std::string{id} +
           "\",\"name\":\"Fixture\",\"version\":\"" +
           std::string{version} +
           "\",\"api_version\":\"1.0\",\"dependencies\":" +
           std::string{dependencies} + "," + std::string{extra} + "}";
}

std::string file_url(const std::filesystem::path& path) {
    return "file://" + path.string();
}

template <typename Function>
void expect_error(std::string_view code, Function&& function) {
    try {
        function();
    } catch (const PackageError& error) {
        require(error.code == code,
                "expected " + std::string{code} + ", got " + error.code);
        return;
    }
    throw std::runtime_error{"expected PackageError " + std::string{code}};
}

PackageManager::Options options(const std::filesystem::path& root) {
    PackageManager::Options result;
    result.root = root;
    result.allow_file_repositories_for_tests = true;
    return result;
}

void test_vectors() {
    using namespace cardgame::server::package;
    require(sha256("abc") ==
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "SHA-256 abc vector mismatch");
    const boost::json::value standard = boost::json::object{
        {"numbers", boost::json::array{
            333333333.33333329, 1e30, 4.50, 2e-3, 1e-27}},
        {"string", std::string{"\xe2\x82\xac" "$\x0f\nA'B\"\\\\\"/"}},
        {"literals", boost::json::array{nullptr, true, false}}};
    const std::string canonical = canonicalize_json(standard);
    const std::string vector_hash = sha256(canonical);
    require(vector_hash ==
                "2d5e01a318d0f0879ab568c4be289c8b1f64ef8921a53c6277d5e069978baacb",
            "RFC 8785 section 3.2.2 vector mismatch: " + vector_hash + " " + canonical);
    const boost::json::value sorting = boost::json::object{
        {"€", "Euro Sign"},
        {"\r", "Carriage Return"},
        {"דּ", "Hebrew Letter Dalet With Dagesh"},
        {"1", "One"},
        {"😀", "Emoji: Grinning Face"},
        {"\xc2\x80", "Control"},
        {"ö", "Latin Small Letter O With Diaeresis"}};
    const std::string sorted = canonicalize_json(sorting);
    std::size_t previous = 0;
    for (const std::string_view value : {
             "Carriage Return", "One", "Control",
             "Latin Small Letter O With Diaeresis", "Euro Sign",
             "Emoji: Grinning Face", "Hebrew Letter Dalet With Dagesh"}) {
        const std::size_t position = sorted.find(value);
        require(position != std::string::npos && position >= previous,
                "RFC 8785 section 3.2.3 key sorting mismatch");
        previous = position;
    }

    require(PackageManager::calculate_manifest_id({}) ==
                "4f53cda18c2baa0c0354bb5f9a3ecbe5ed12ab4d8e11ba873c2f11161202b945",
            "empty manifest id mismatch");
    const std::vector<SyncPackage> packages{
        {"org.sgs.standard", "1.0.0", "1.0",
         "https://github.com/cy12131/sgs-standard-package.git",
         std::string(40, '2'), std::string(64, 'b'), {"org.sgs.base"}},
        {"org.sgs.base", "1.0.0", "1.0",
         "https://github.com/cy12131/sgs-base-package.git",
         std::string(40, '1'), std::string(64, 'a'), {}},
    };
    require(PackageManager::calculate_manifest_id(packages) ==
                "1b773ff47e081a15bc050331b8beabe6e1e6ee001301ccc11c7249bfe70cf4d8",
            "architecture multi-package manifest vector mismatch");

    SyncPackage invalid_url = packages.front();
    invalid_url.repository_url = "https:///missing-authority.git";
    expect_error("INVALID_PACKAGE_MANIFEST", [&] {
        static_cast<void>(PackageManager::validate_sync_manifest({invalid_url}, 30000));
    });

    require(version_satisfies("10.0.0", ">2.0.0") &&
                !version_satisfies("2.0.0", ">10.0.0") &&
                version_satisfies("1.0.0", ">1.0.0-alpha"),
            "SemVer precedence comparison mismatch");
}

void test_install_and_persistence(const std::filesystem::path& root) {
    const auto repo = root / "valid-repo";
    const std::string commit = create_repository(root, "valid-repo", manifest("org.sgs.base"));
    PackageManager manager{options(root / "state")};
    const auto& installed = manager.install(file_url(repo), commit);
    require(installed.commit == commit && installed.manifest.id == "org.sgs.base",
            "installed package did not lock expected commit");
    const std::string before = manager.active_manifest().manifest_id;
    const auto& active = manager.enable("org.sgs.base");
    require(active.packages.size() == 1 && active.packages[0].commit == commit,
            "enabled manifest is incorrect");
    require(active.manifest_id != before, "enable did not change manifest id");

    PackageManager reloaded{options(root / "state")};
    require(reloaded.installed().size() == 1 && reloaded.installed()[0].enabled,
            "persistent registry did not restore enabled package");
    require(reloaded.active_manifest().manifest_id == active.manifest_id,
            "persistent registry restored a different active manifest");

    const std::string valid_manifest_id = manager.active_manifest().manifest_id;
    write_file(root / "state/registry.json", "{not-json");
    expect_error("INVALID_PACKAGE_REGISTRY", [&] { manager.reload_registry(); });
    require(manager.active_manifest().manifest_id == valid_manifest_id &&
                manager.installed().size() == 1,
            "failed registry reload did not preserve in-memory configuration");
}

void test_invalid_installations(const std::filesystem::path& root) {
    PackageManager manager{options(root / "state-invalid")};

    const auto missing_repo = root / "missing-repo";
    const std::string missing_commit = create_repository(
        root, "missing-repo", "", false);
    expect_error("PACKAGE_MANIFEST_MISSING", [&] {
        static_cast<void>(manager.install(file_url(missing_repo), missing_commit));
    });

    const auto traversal_repo = root / "traversal-repo";
    const std::string traversal_commit = create_repository(
        root, "traversal-repo",
        manifest("org.sgs.traversal", "1.0.0", "[]", "\"resources\":[\"../escape\"]"));
    expect_error("INVALID_PACKAGE_MANIFEST", [&] {
        static_cast<void>(manager.install(file_url(traversal_repo), traversal_commit));
    });

    const auto symlink_repo = root / "symlink-repo";
    static_cast<void>(create_repository(
        root, "symlink-repo",
        manifest("org.sgs.symlink", "1.0.0", "[]",
                 R"("resources":["assets/escape"])")));
    write_file(root / "outside.txt", "outside package root\n");
    std::filesystem::create_symlink("../../outside.txt", symlink_repo / "assets/escape");
    run({"git", "-C", symlink_repo.string(), "add", "."});
    run({"git", "-C", symlink_repo.string(), "commit", "-m", "add escaping symlink"});
    const std::string symlink_commit =
        run({"git", "-C", symlink_repo.string(), "rev-parse", "HEAD"});
    expect_error("INVALID_PACKAGE_MANIFEST", [&] {
        static_cast<void>(manager.install(file_url(symlink_repo), symlink_commit));
    });

    expect_error("GIT_FAILED", [&] {
        static_cast<void>(manager.install(
            file_url(root / "does-not-exist"), std::string(40, '1')));
    });
    require(manager.installed().empty() && manager.active_manifest().packages.empty(),
            "failed installs changed valid configuration");
}

void test_checkout_integrity(const std::filesystem::path& root) {
    const auto run_case = [&](std::string_view case_name, auto&& pollute) {
        const std::filesystem::path case_root = root / std::string{case_name};
        const std::filesystem::path repo = case_root / "repo";
        std::string commit = create_repository(
            case_root, "repo", manifest("org.sgs." + std::string{case_name}));
        if (case_name == "ignored") {
            write_file(repo / ".gitignore", "*.cache\n");
            run({"git", "-C", repo.string(), "add", ".gitignore"});
            run({"git", "-C", repo.string(), "commit", "-m", "ignore fixture"});
            commit = run({"git", "-C", repo.string(), "rev-parse", "HEAD"});
        }

        const std::filesystem::path state = case_root / "state";
        PackageManager manager{options(state)};
        const auto& installed = manager.install(file_url(repo), commit);
        const std::filesystem::path checkout = installed.directory;
        const std::string package_id = installed.manifest.id;
        const std::string expected_manifest = manager.enable(package_id).manifest_id;
        const std::string registry_before = read_file(state / "registry.json");

        pollute(checkout);
        expect_error("PACKAGE_CHECKOUT_DIRTY", [&] { manager.reload_registry(); });
        require(manager.active_manifest().manifest_id == expected_manifest,
                "dirty checkout changed the running in-memory generation");
        require(read_file(state / "registry.json") == registry_before,
                "dirty checkout validation changed registry.json");
    };

    run_case("tracked", [](const std::filesystem::path& checkout) {
        write_file(checkout / "server/init.lua", "return { dirty = true }\n");
    });
    run_case("staged", [](const std::filesystem::path& checkout) {
        write_file(checkout / "server/init.lua", "return { staged = true }\n");
        run({"git", "-C", checkout.string(), "add", "server/init.lua"});
    });
    run_case("untracked", [](const std::filesystem::path& checkout) {
        write_file(checkout / "assets/untracked.txt", "untracked\n");
    });
    run_case("ignored", [](const std::filesystem::path& checkout) {
        write_file(checkout / "assets/generated.cache", "ignored but unsafe\n");
    });
}

void test_canonical_registry_directories(const std::filesystem::path& root) {
    {
        const std::filesystem::path case_root = root / "canonical";
        const std::filesystem::path repo = case_root / "repo";
        const std::string commit = create_repository(
            case_root, "repo", manifest("org.sgs.canonical"));
        const std::filesystem::path state = case_root / "state";
        PackageManager manager{options(state)};
        const auto& installed = manager.install(file_url(repo), commit);
        require(installed.directory ==
                    state / "installed" / "org.sgs.canonical" / commit,
                "legal install did not use <package-id>/<commit>");
        static_cast<void>(manager.enable("org.sgs.canonical"));
        PackageManager reloaded{options(state)};
        require(reloaded.active_manifest().packages.size() == 1,
                "legal canonical package did not reload");

        const std::string generation = manager.active_manifest().manifest_id;
        replace_registry_directory(state / "registry.json", "legacy-flat");
        expect_error("INVALID_PACKAGE_REGISTRY", [&] { manager.reload_registry(); });
        require(manager.active_manifest().manifest_id == generation,
                "non-canonical registry path changed the running generation");
    }

    {
        const std::filesystem::path case_root = root / "directory-symlink";
        const std::filesystem::path repo = case_root / "repo";
        const std::string commit = create_repository(
            case_root, "repo", manifest("org.sgs.directory_symlink"));
        const std::filesystem::path state = case_root / "state";
        PackageManager manager{options(state)};
        const auto& installed = manager.install(file_url(repo), commit);
        const std::filesystem::path checkout = installed.directory;
        const std::string package_id = installed.manifest.id;
        static_cast<void>(manager.enable(package_id));
        const std::string generation = manager.active_manifest().manifest_id;
        const std::filesystem::path outside = case_root / "outside-checkout";
        std::filesystem::rename(checkout, outside);
        std::filesystem::create_directory_symlink(outside, checkout);

        expect_error("INVALID_PACKAGE_REGISTRY", [&] { manager.reload_registry(); });
        require(manager.active_manifest().manifest_id == generation,
                "symlink escape changed the running generation");
    }
}

void test_manifest_symlink_rejected(const std::filesystem::path& root) {
    const std::filesystem::path case_root = root / "manifest-symlink";
    const std::filesystem::path repo = case_root / "repo";
    static_cast<void>(create_repository(
        case_root, "repo", manifest("org.sgs.manifest_symlink")));
    const std::string manifest_bytes = read_file(repo / "manifest.json");
    write_file(repo / "real-manifest.json", manifest_bytes);
    std::filesystem::remove(repo / "manifest.json");
    std::filesystem::create_symlink("real-manifest.json", repo / "manifest.json");
    run({"git", "-C", repo.string(), "add", "-A"});
    run({"git", "-C", repo.string(), "commit", "-m", "symlink manifest"});
    const std::string commit = run({"git", "-C", repo.string(), "rev-parse", "HEAD"});

    PackageManager manager{options(case_root / "state")};
    expect_error("INVALID_PACKAGE_MANIFEST", [&] {
        static_cast<void>(manager.install(file_url(repo), commit));
    });
    require(manager.installed().empty(),
            "manifest symlink failure registered a package");
}

void test_dependencies(const std::filesystem::path& root) {
    {
        PackageManager manager{options(root / "ordered-state")};
        const auto base = root / "ordered-base";
        const auto application = root / "ordered-application";
        const std::string base_commit = create_repository(
            root, "ordered-base", manifest("org.sgs.ordered_base", "10.0.0"));
        const std::string application_commit = create_repository(
            root, "ordered-application", manifest(
                "org.sgs.ordered_application", "1.0.0",
                R"([{"id":"org.sgs.ordered_base","version":">2.0.0"}])"));
        static_cast<void>(manager.install(file_url(application), application_commit));
        static_cast<void>(manager.install(file_url(base), base_commit));
        const auto& active = manager.set_enabled(
            {"org.sgs.ordered_application", "org.sgs.ordered_base"});
        require(active.packages.size() == 2 &&
                    active.packages[0].id == "org.sgs.ordered_base" &&
                    active.packages[1].id == "org.sgs.ordered_application",
                "dependency load order is not deterministic/topological");
    }
    {
        PackageManager manager{options(root / "missing-state")};
        const auto repo = root / "needs-repo";
        const std::string commit = create_repository(
            root, "needs-repo", manifest(
                "org.sgs.needs", "1.0.0",
                R"([{"id":"org.sgs.absent","version":">=1.0.0 <2.0.0"}])"));
        static_cast<void>(manager.install(file_url(repo), commit));
        expect_error("MISSING_DEPENDENCY", [&] {
            static_cast<void>(manager.enable("org.sgs.needs"));
        });
        require(manager.active_manifest().packages.empty(),
                "missing dependency changed active manifest");
    }
    {
        PackageManager manager{options(root / "cycle-state")};
        const auto a_repo = root / "cycle-a";
        const auto b_repo = root / "cycle-b";
        const std::string a_commit = create_repository(
            root, "cycle-a", manifest(
                "org.sgs.cycle_a", "1.0.0",
                R"([{"id":"org.sgs.cycle_b","version":"1.0.0"}])"));
        const std::string b_commit = create_repository(
            root, "cycle-b", manifest(
                "org.sgs.cycle_b", "1.0.0",
                R"([{"id":"org.sgs.cycle_a","version":"1.0.0"}])"));
        static_cast<void>(manager.install(file_url(a_repo), a_commit));
        static_cast<void>(manager.install(file_url(b_repo), b_commit));
        expect_error("CYCLIC_DEPENDENCY", [&] {
            static_cast<void>(manager.set_enabled(
                {"org.sgs.cycle_a", "org.sgs.cycle_b"}));
        });
    }
    {
        PackageManager manager{options(root / "duplicate-state")};
        const auto first = root / "duplicate-a";
        const auto second = root / "duplicate-b";
        const std::string first_commit = create_repository(
            root, "duplicate-a", manifest("org.sgs.duplicate"));
        const std::string second_commit = create_repository(
            root, "duplicate-b", manifest("org.sgs.duplicate", "2.0.0"));
        static_cast<void>(manager.install(file_url(first), first_commit));
        expect_error("DUPLICATE_PACKAGE_ID", [&] {
            static_cast<void>(manager.install(file_url(second), second_commit));
        });
    }
}

void test_frame_limit_and_sync_state(const std::filesystem::path& root) {
    PackageManager manager{options(root / "sync-state")};
    const std::string previous_manifest_id = manager.active_manifest().manifest_id;
    std::vector<SyncPackage> oversized;
    for (int index = 0; index < 256; ++index) {
        const std::string number = std::to_string(index);
        oversized.push_back({
            "org.sgs.package_" + std::string(110 - number.size(), 'x') + number,
            "1.0.0", "1.0",
            "https://example.invalid/" + std::string(180, 'u') + number,
            std::string(40, '1'), std::string(64, 'a'), {}});
    }
    expect_error("PACKAGE_MANIFEST_TOO_LARGE", [&] {
        static_cast<void>(PackageManager::validate_sync_manifest(oversized, 30000));
    });
    require(manager.active_manifest().manifest_id == previous_manifest_id,
            "oversized candidate changed the previous active manifest");

    cardgame::server::package::PackageSyncService service{manager};
    using cardgame::server::protocol::Message;
    Message get{1, "GetPackageManifest", 1, {}};
    const Message manifest_result = service.handle(7, get);
    require(manifest_result.type == "PackageManifestResult" &&
                manifest_result.data.at("manifest_id").as_string() ==
                    manager.active_manifest().manifest_id,
            "GetPackageManifest failed");
    Message mismatch{1, "PackageReady", 2, {{"manifest_id", std::string(64, 'b')}}};
    const Message mismatch_result = service.handle(7, mismatch);
    require(!mismatch_result.data.at("ready").as_bool() && !service.is_ready(7),
            "mismatched PackageReady was accepted");
    Message ready{1, "PackageReady", 3,
                  {{"manifest_id", manager.active_manifest().manifest_id}}};
    require(service.handle(7, ready).data.at("ready").as_bool() && service.is_ready(7),
            "matching PackageReady was rejected");

    const auto repo = root / "changed-repo";
    const std::string commit = create_repository(root, "changed-repo", manifest("org.sgs.changed"));
    static_cast<void>(manager.install(file_url(repo), commit));
    bool notified = false;
    service.set_changed_handler([&](const std::string&) { notified = true; });
    static_cast<void>(manager.enable("org.sgs.changed"));
    require(notified && !service.is_ready(7),
            "manifest change did not notify or invalidate session readiness");
    service.disconnect(7);
    require(!service.is_ready(7), "disconnect retained package readiness");
}

}  // namespace

int main() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("sgs-package-test-" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    try {
        test_vectors();
        test_install_and_persistence(root);
        test_invalid_installations(root);
        test_checkout_integrity(root);
        test_canonical_registry_directories(root);
        test_manifest_symlink_rejected(root);
        test_dependencies(root);
        test_frame_limit_and_sync_state(root);
        std::filesystem::remove_all(root);
        std::cout << "Package manager tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Package manager test failed: " << error.what() << '\n';
        std::filesystem::remove_all(root);
        return 1;
    }
}
