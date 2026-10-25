#include "cardgame/server/package/package_manager.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::filesystem::path default_root() {
    if (const char* configured = std::getenv("SGS_PACKAGE_ROOT");
        configured != nullptr && *configured != '\0') {
        return configured;
    }
    if (const char* state_home = std::getenv("XDG_STATE_HOME");
        state_home != nullptr && *state_home != '\0') {
        return std::filesystem::path{state_home} / "sgs-server" / "packages";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path{home} / ".local" / "state" /
               "sgs-server" / "packages";
    }
    return ".sgs-packages";
}

void usage() {
    std::cerr
        << "usage: sgs-package-admin [--root PATH] list\n"
        << "       sgs-package-admin [--root PATH] install URL COMMIT\n"
        << "       sgs-package-admin [--root PATH] enable PACKAGE_ID\n"
        << "       sgs-package-admin [--root PATH] disable PACKAGE_ID\n"
        << "       sgs-package-admin [--root PATH] set-enabled [PACKAGE_ID ...]\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path root = default_root();
    int index = 1;
    if (index < argc && std::string_view{argv[index]} == "--root") {
        if (index + 1 >= argc) {
            usage();
            return 2;
        }
        root = argv[index + 1];
        index += 2;
    }
    if (index >= argc) {
        usage();
        return 2;
    }

    try {
        cardgame::server::package::PackageManager manager{{root}};
        const std::string_view command{argv[index++]};
        if (command == "list" && index == argc) {
            for (const auto& package : manager.installed()) {
                std::cout << package.manifest.id << ' ' << package.manifest.version << ' '
                          << package.commit << ' '
                          << (package.enabled ? "enabled" : "disabled") << '\n';
            }
            std::cout << "manifest_id " << manager.active_manifest().manifest_id << '\n';
            return 0;
        }
        if (command == "install" && index + 2 == argc) {
            const auto& installed = manager.install(argv[index], argv[index + 1]);
            std::cout << "installed " << installed.manifest.id << ' '
                      << installed.commit << '\n';
            return 0;
        }
        if (command == "enable" && index + 1 == argc) {
            const auto& active = manager.enable(argv[index]);
            std::cout << "manifest_id " << active.manifest_id << '\n';
            return 0;
        }
        if (command == "disable" && index + 1 == argc) {
            const auto& active = manager.disable(argv[index]);
            std::cout << "manifest_id " << active.manifest_id << '\n';
            return 0;
        }
        if (command == "set-enabled") {
            std::vector<std::string> ids;
            while (index < argc) {
                ids.emplace_back(argv[index++]);
            }
            const auto& active = manager.set_enabled(ids);
            std::cout << "manifest_id " << active.manifest_id << '\n';
            return 0;
        }
        usage();
        return 2;
    } catch (const cardgame::server::package::PackageError& error) {
        std::cerr << error.code << ": " << error.what() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "PACKAGE_ADMIN_FAILED: " << error.what() << '\n';
        return 1;
    }
}
