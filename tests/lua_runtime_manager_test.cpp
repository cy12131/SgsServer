#include "cardgame/server/game/game_process_manager.h"

#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

using namespace cardgame::server::game;
using namespace std::chrono_literals;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error{std::string{message}};
}

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        std::string pattern = "/tmp/sgs-lua-manager-test.XXXXXX";
        const char* result = ::mkdtemp(pattern.data());
        if (result == nullptr) throw std::runtime_error{"mkdtemp failed"};
        path_ = result;
    }
    ~TemporaryDirectory() { std::filesystem::remove_all(path_); }
    const std::filesystem::path& path() const { return path_; }
private:
    std::filesystem::path path_;
};

GameLaunchRecord launch(const std::filesystem::path& root, std::string game_id) {
    return {
        std::move(game_id), "42", std::string(64, 'a'),
        {{"org.sgs.runtime-test", "1.0.0", std::string(40, '1'), root,
          std::filesystem::path{"server.lua"}}},
        "standard",
        {{"player-1", 1, "甲"}, {"player-2", 2, "乙"}},
        "1234", {}, "init_runtime_manager_0001",
    };
}

GameProcessOptions options(const std::filesystem::path& runtime,
                           const std::filesystem::path& working) {
    GameProcessOptions result;
    result.executable = std::filesystem::absolute(runtime);
    result.working_directory = std::filesystem::absolute(working);
    result.initialization_timeout = 500ms;
    result.shutdown_grace = 200ms;
    result.terminate_grace = 200ms;
    result.maximum_log_line_bytes = 128;
    result.maximum_log_records_per_second = 4;
    return result;
}

void test_log_flood(const std::filesystem::path& runtime,
                    const std::filesystem::path& temporary) {
    const auto root = temporary / "flood-package";
    std::filesystem::create_directories(root);
    std::ofstream script{root / "server.lua"};
    script << "for i=1,2000 do print('PRIVATE_PLAYER_DATA') end\n";
    script << "print('" << std::string(5000, 'x') << "')\n";
    script << "local p=Runtime.registerPackage({id='org.sgs.runtime-test',version='1.0.0'})\n";
    script << "p:registerGameMode({id='standard',min_players=2,max_players=8,"
              "create_logic=function(ctx) return {} end})\n";
    script.close();

    boost::asio::io_context io;
    GameProcessManager manager{io};
    boost::asio::steady_timer settle{io};
    boost::asio::steady_timer watchdog{io};
    std::optional<GameProcessSnapshot> observed;
    std::optional<GameProcessExit> exit;
    bool leaked_private_data = false;
    std::size_t safe_records = 0;
    const std::string game_id = "game-log-flood";
    manager.start(
        launch(std::filesystem::canonical(root), game_id), options(runtime, temporary),
        [&](const GameLaunchRecord&, const GameIpcMessage& message) {
            if (message.type != "GameReady") return;
            settle.expires_after(50ms);
            settle.async_wait([&](const boost::system::error_code& error) {
                if (error) return;
                observed = manager.snapshot(game_id);
                manager.request_stop(game_id, "test complete");
            });
        },
        [&](const GameProcessExit& result) {
            exit = result;
            watchdog.cancel();
        },
        [&](const GameLaunchRecord&, std::string_view record) {
            ++safe_records;
            leaked_private_data = leaked_private_data ||
                                  record.find("PRIVATE_PLAYER_DATA") != std::string_view::npos;
        });
    watchdog.expires_after(5s);
    watchdog.async_wait([&](const boost::system::error_code& error) {
        if (!error) throw std::runtime_error{"Lua log flood scenario timed out"};
    });
    io.run();

    require(exit && exit->expected, "Lua Runtime did not exit normally");
    require(observed && observed->log_bytes_read > 10'000,
            "bounded logger did not drain Runtime output");
    require(observed->log_records_suppressed > 0 && observed->log_lines_truncated > 0,
            "bounded logger did not enforce rate and line limits");
    require(safe_records <= 4 && !leaked_private_data,
            "Runtime logger exposed content or exceeded its output rate");
}

void test_initialization_timeout(const std::filesystem::path& runtime,
                                 const std::filesystem::path& temporary) {
    const auto root = temporary / "timeout-package";
    std::filesystem::create_directories(root);
    std::ofstream{root / "server.lua"} << "while true do end\n";
    boost::asio::io_context io;
    GameProcessManager manager{io};
    auto settings = options(runtime, temporary);
    settings.initialization_timeout = 80ms;
    std::optional<GameProcessExit> exit;
    manager.start(
        launch(std::filesystem::canonical(root), "game-init-timeout"), settings,
        [](const GameLaunchRecord&, const GameIpcMessage&) {},
        [&](const GameProcessExit& result) { exit = result; });
    io.run();
    require(exit && exit->failure_code == "IPC_INITIALIZATION_TIMEOUT",
            "Lua initialization timeout was not reaped deterministically");
}

void test_initialization_failure(const std::filesystem::path& runtime,
                                 const std::filesystem::path& temporary) {
    const auto root = temporary / "failure-package";
    std::filesystem::create_directories(root);
    std::ofstream{root / "server.lua"} << "this is not valid Lua !!!\n";
    boost::asio::io_context io;
    GameProcessManager manager{io};
    std::optional<GameProcessExit> exit;
    manager.start(
        launch(std::filesystem::canonical(root), "game-init-failure"),
        options(runtime, temporary),
        [](const GameLaunchRecord&, const GameIpcMessage&) {},
        [&](const GameProcessExit& result) { exit = result; });
    io.run();
    require(exit && exit->failure_code == "IPC_CHILD_REPORTED_FAILURE",
            "Lua initialization failure was not reported through GameFailed");
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error{"expected sgs-lua-runtime path"};
        TemporaryDirectory temporary;
        test_log_flood(argv[1], temporary.path());
        test_initialization_timeout(argv[1], temporary.path());
        test_initialization_failure(argv[1], temporary.path());
        std::cout << "Lua Runtime manager tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Lua Runtime manager test failed: " << exception.what() << '\n';
        return 1;
    }
}
