#include "cardgame/server/application.h"

#include "cardgame/server/network/tcp_server.h"

#include <charconv>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace cardgame::server {

Application::Application(std::ostream& output) noexcept : output_(output) {}

int Application::run() {
    constexpr std::string_view default_address{"127.0.0.1"};
    constexpr unsigned short default_port = 18765;

    const char* address_environment = std::getenv("SGS_BIND_ADDRESS");
    const std::string address = address_environment == nullptr
                                    ? std::string{default_address}
                                    : std::string{address_environment};

    unsigned short port = default_port;
    if (const char* port_environment = std::getenv("SGS_PORT");
        port_environment != nullptr) {
        const std::string_view port_text{port_environment};
        unsigned int parsed_port = 0;
        const auto [end, error] = std::from_chars(
            port_text.data(), port_text.data() + port_text.size(), parsed_port);
        if (error != std::errc{} || end != port_text.data() + port_text.size() ||
            parsed_port == 0 || parsed_port > 65535) {
            output_ << "Invalid SGS_PORT: " << port_text << '\n';
            return 1;
        }
        port = static_cast<unsigned short>(parsed_port);
    }

    std::filesystem::path package_root;
    if (const char* configured = std::getenv("SGS_PACKAGE_ROOT");
        configured != nullptr && *configured != '\0') {
        package_root = configured;
    } else if (const char* state_home = std::getenv("XDG_STATE_HOME");
               state_home != nullptr && *state_home != '\0') {
        package_root = std::filesystem::path{state_home} / "sgs-server" / "packages";
    } else if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        package_root = std::filesystem::path{home} / ".local" / "state" /
                       "sgs-server" / "packages";
    } else {
        package_root = ".sgs-packages";
    }

    try {
        output_ << name << ' ' << version << '\n';
        network::TcpServer server{address, port, std::move(package_root), output_};
        server.run();
        return 0;
    } catch (const std::exception& exception) {
        output_ << "Server startup failed: " << exception.what() << '\n';
        return 1;
    }
}

}  // namespace cardgame::server
