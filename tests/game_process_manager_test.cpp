#include "cardgame/server/game/game_process_manager.h"

#include <boost/asio/steady_timer.hpp>
#include <boost/json/string.hpp>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

using namespace cardgame::server::game;
using namespace std::chrono_literals;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error{std::string{message}};
}

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        std::string pattern = "/tmp/sgs-game-process-test.XXXXXX";
        char* result = ::mkdtemp(pattern.data());
        if (result == nullptr) throw std::runtime_error{"mkdtemp failed"};
        path_ = result;
    }
    ~TemporaryDirectory() { std::filesystem::remove_all(path_); }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

GameLaunchRecord launch(const std::filesystem::path& root, std::string id) {
    const auto package_root = root / "packages";
    std::filesystem::create_directories(package_root);
    return {
        std::move(id),
        "7",
        std::string(64, 'a'),
        {{"org.sgs.test", "1.0.0", std::string(40, '1'),
          std::filesystem::absolute(package_root), std::nullopt}},
        "standard",
        {{"player-a", 1, "甲"}, {"player-b", 2, "乙"}},
        "424242",
        boost::json::object{{"rounds", 1}},
        "init_token_00000001",
    };
}

GameProcessOptions options(
    const std::filesystem::path& executable,
    const std::filesystem::path& root,
    std::string mode) {
    return {
        std::filesystem::absolute(executable),
        std::filesystem::absolute(root),
        {"--mode", std::move(mode)},
        150ms,
        50ms,
        50ms,
        4U * 1024U * 1024U,
        256,
    };
}

struct ScenarioResult {
    GameProcessExit exit;
    int messages = 0;
};

using ReadyAction = std::function<void(
    GameProcessManager&, const GameLaunchRecord&, const GameIpcMessage&)>;

ScenarioResult run_single(
    const std::filesystem::path& executable,
    const std::filesystem::path& root,
    std::string mode,
    ReadyAction action = {},
    std::function<void(GameProcessOptions&)> configure = {},
    std::vector<std::string> extra = {}) {
    boost::asio::io_context io;
    GameProcessManager manager{io};
    GameProcessOptions settings = options(executable, root, std::move(mode));
    settings.extra_arguments.insert(settings.extra_arguments.end(),
                                    extra.begin(), extra.end());
    if (configure) configure(settings);
    std::optional<GameProcessExit> exit;
    int messages = 0;
    boost::asio::steady_timer watchdog{io};
    const std::string game_id = "game-test-" + std::to_string(::getpid()) +
                                "-" + std::to_string(reinterpret_cast<std::uintptr_t>(&io));
    const pid_t child = manager.start(
        launch(root, game_id), settings,
        [&](const GameLaunchRecord& record, const GameIpcMessage& message) {
            ++messages;
            if (action) action(manager, record, message);
        },
        [&](const GameProcessExit& result) {
            exit = result;
            watchdog.cancel();
        });

    watchdog.expires_after(4s);
    watchdog.async_wait([&](const boost::system::error_code& error) {
        if (!error && !exit.has_value()) {
            throw std::runtime_error{"game process scenario timed out"};
        }
    });
    io.run();
    require(exit.has_value(), "game process did not report exit");
    int status = 0;
    errno = 0;
    require(::waitpid(child, &status, WNOHANG) == -1 && errno == ECHILD,
            "child was not reaped exactly once");
    return {*exit, messages};
}

void test_normal_and_fd_isolation(
    const std::filesystem::path& executable,
    const std::filesystem::path& root) {
    const int base = ::open("/dev/null", O_RDONLY);
    const int sentinel = base < 0 ? -1 : ::fcntl(base, F_DUPFD, 10);
    if (base >= 0) ::close(base);
    require(sentinel >= 10, "could not allocate inheritance sentinel fd");
    const ScenarioResult result = run_single(
        executable, root / "normal", "normal",
        [](GameProcessManager& manager, const GameLaunchRecord& record,
           const GameIpcMessage& message) {
            require(message.type == "GameReady", "normal child did not become ready");
            const auto snapshot = manager.snapshot(record.game_id);
            require(snapshot.has_value() && snapshot->state == GameProcessState::running &&
                        snapshot->launch.room_id == "7" &&
                        snapshot->launch.manifest_id == std::string(64, 'a') &&
                        snapshot->launch.players.size() == 2 &&
                        snapshot->child_identity.starts_with("pid:"),
                    "immutable launch binding was not retained");
            manager.request_stop(record.game_id, "test complete");
        }, {}, {"--sentinel-fd", std::to_string(sentinel)});
    ::close(sentinel);
    require(result.exit.expected && result.exit.exit_code == 0 &&
                !result.exit.failure_code.has_value() && result.messages == 1,
            "normal child did not exit cleanly");
}

void test_failures(
    const std::filesystem::path& executable,
    const std::filesystem::path& root) {
    const struct Case {
        std::string mode;
        std::string code;
        int expected_exit;
    } cases[] = {
        {"crash", "IPC_PIPE_CLOSED", 23},
        {"invalid-json", "IPC_JSON_INVALID", -1},
        {"unknown-type", "IPC_MESSAGE_TYPE_INVALID", -1},
        {"oversize", "IPC_FRAME_INVALID", -1},
        {"truncated", "IPC_PIPE_CLOSED", -1},
        {"gap", "IPC_SEQUENCE_INVALID", -1},
        {"duplicate", "IPC_SEQUENCE_INVALID", -1},
        {"binding-mismatch", "IPC_BINDING_MISMATCH", -1},
        {"close-output", "IPC_PIPE_CLOSED", -1},
        {"no-ready", "IPC_INITIALIZATION_TIMEOUT", -1},
    };
    for (const Case& test : cases) {
        const ScenarioResult result = run_single(
            executable, root / test.mode, test.mode, {},
            [&](GameProcessOptions& value) {
                if (test.mode == "no-ready") value.initialization_timeout = 30ms;
            });
        require(result.exit.failure_code == test.code,
                "failure mode " + test.mode + " expected " + test.code +
                    ", got " + result.exit.failure_code.value_or("<none>"));
        if (test.expected_exit >= 0) {
            require(result.exit.exited && result.exit.exit_code == test.expected_exit,
                    "crash exit code was not preserved");
        }
    }

    const ScenarioResult early_exit = run_single(
        executable, root / "exit-after-ready", "exit-after-ready");
    require((early_exit.exit.failure_code == "IPC_CHILD_EXITED" ||
             early_exit.exit.failure_code == "IPC_PIPE_CLOSED") &&
                early_exit.exit.exited && early_exit.exit.exit_code == 0 &&
                !early_exit.exit.expected,
            "zero exit before a terminal message was treated as success");
}

void test_parent_write_pipe_closed(
    const std::filesystem::path& executable,
    const std::filesystem::path& root) {
    const ScenarioResult result = run_single(
        executable, root / "close-input", "close-input",
        [](GameProcessManager& manager, const GameLaunchRecord& record,
           const GameIpcMessage&) {
            manager.send(record.game_id, "PlayerAction", {
                {"player_id", "player-a"},
                {"action_type", "probe-closed-input"},
                {"data", std::string(128U * 1024U, 'x')},
            });
        });
    require(result.exit.failure_code == "IPC_PIPE_CLOSED",
            "closed child input pipe was not reported to the target game");
}

void test_backpressure(
    const std::filesystem::path& executable,
    const std::filesystem::path& root) {
    const std::string data(32U * 1024U, 'x');
    const ScenarioResult result = run_single(
        executable, root / "backpressure", "ready-then-stall",
        [&](GameProcessManager& manager, const GameLaunchRecord& record,
            const GameIpcMessage&) {
            for (int index = 0; index < 32; ++index) {
                const auto current = manager.snapshot(record.game_id);
                if (!current.has_value() || current->failure_code.has_value()) break;
                manager.send(record.game_id, "PlayerAction", {
                    {"player_id", "player-a"},
                    {"action_type", "bulk"},
                    {"data", data},
                });
            }
        },
        [](GameProcessOptions& value) {
            value.maximum_queued_bytes = 96U * 1024U;
            value.maximum_queued_frames = 8;
        });
    require(result.exit.failure_code == "IPC_BACKPRESSURE",
            "write queue overflow did not fail only the target game");

    const ScenarioResult frame_result = run_single(
        executable, root / "backpressure-frames", "ready-then-stall",
        [](GameProcessManager& manager, const GameLaunchRecord& record,
           const GameIpcMessage&) {
            for (int index = 0; index < 16; ++index) {
                const auto current = manager.snapshot(record.game_id);
                if (!current.has_value() || current->failure_code.has_value()) break;
                manager.send(record.game_id, "PlayerAction", {
                    {"player_id", "player-a"},
                    {"action_type", "small"},
                    {"data", index},
                });
            }
        },
        [](GameProcessOptions& value) {
            value.maximum_queued_bytes = 4U * 1024U * 1024U;
            value.maximum_queued_frames = 3;
        });
    require(frame_result.exit.failure_code == "IPC_BACKPRESSURE",
            "frame-count queue limit did not terminate the target game");

    const ScenarioResult default_bytes = run_single(
        executable, root / "backpressure-default-bytes", "ready-then-stall",
        [](GameProcessManager& manager, const GameLaunchRecord& record,
           const GameIpcMessage&) {
            const std::string near_limit(900U * 1024U, 'b');
            for (int index = 0; index < 8; ++index) {
                const auto current = manager.snapshot(record.game_id);
                if (!current.has_value() || current->failure_code.has_value()) break;
                manager.send(record.game_id, "PlayerAction", {
                    {"player_id", "player-a"},
                    {"action_type", "default-byte-limit"},
                    {"data", near_limit},
                });
            }
        });
    require(default_bytes.exit.failure_code == "IPC_BACKPRESSURE",
            "default 4 MiB queue limit did not terminate the target game");

    const ScenarioResult default_frames = run_single(
        executable, root / "backpressure-default-frames", "ready-then-stall",
        [](GameProcessManager& manager, const GameLaunchRecord& record,
           const GameIpcMessage&) {
            for (int index = 0; index < 300; ++index) {
                const auto current = manager.snapshot(record.game_id);
                if (!current.has_value() || current->failure_code.has_value()) break;
                manager.send(record.game_id, "PlayerAction", {
                    {"player_id", "player-a"},
                    {"action_type", "default-frame-limit"},
                    {"data", index},
                });
            }
        });
    require(default_frames.exit.failure_code == "IPC_BACKPRESSURE",
            "default 256-frame queue limit did not terminate the target game");

    const GameProcessOptions defaults;
    require(defaults.maximum_queued_bytes == 4U * 1024U * 1024U &&
                defaults.maximum_queued_frames == 256,
            "GameIPC default backpressure limits drifted from the contract");
}

void test_forced_shutdown(
    const std::filesystem::path& executable,
    const std::filesystem::path& root) {
    const ScenarioResult result = run_single(
        executable, root / "shutdown", "ignore-shutdown",
        [](GameProcessManager& manager, const GameLaunchRecord& record,
           const GameIpcMessage&) {
            manager.request_stop(record.game_id, "forced shutdown test");
            manager.request_stop(record.game_id, "duplicate stop must be idempotent");
        },
        [](GameProcessOptions& value) {
            value.shutdown_grace = 30ms;
            value.terminate_grace = 30ms;
        });
    require(result.exit.failure_code == "IPC_SHUTDOWN_TIMEOUT" &&
                result.exit.termination_signal == SIGKILL,
            "unresponsive child was not force-reaped after grace periods");
}

void test_spawn_failure(
    const std::filesystem::path& root) {
    const auto invalid = root / "invalid-executable";
    {
        std::ofstream output(invalid);
        output << "this is not an executable format\n";
    }
    ::chmod(invalid.c_str(), 0755);
    const ScenarioResult result = run_single(
        invalid, root / "spawn-failure", "normal");
    require((result.exit.failure_code == "IPC_CHILD_EXITED" ||
             result.exit.failure_code == "IPC_PIPE_CLOSED") &&
                result.exit.exited && result.exit.exit_code == 127,
            "exec failure was not asynchronously reaped: " +
                result.exit.failure_code.value_or("<none>") + "/" +
                std::to_string(result.exit.exit_code));
}

void test_multiple_children_and_duplicate_id(
    const std::filesystem::path& executable,
    const std::filesystem::path& root) {
    boost::asio::io_context io;
    GameProcessManager manager{io};
    int exits = 0;
    std::vector<pid_t> children;
    boost::asio::steady_timer watchdog{io};
    auto exit_handler = [&](const GameProcessExit& result) {
        require(result.expected, "parallel child did not exit normally");
        ++exits;
        if (exits == 2) watchdog.cancel();
    };
    auto message_handler = [&](const GameLaunchRecord& record, const GameIpcMessage&) {
        manager.request_stop(record.game_id, "parallel test complete");
    };
    GameLaunchRecord first = launch(root / "multi-a", "game-multi-a");
    GameLaunchRecord second = launch(root / "multi-b", "game-multi-b");
    children.push_back(manager.start(first, options(executable, root, "normal"),
                                     message_handler, exit_handler));
    children.push_back(manager.start(second, options(executable, root, "normal"),
                                     message_handler, exit_handler));
    bool duplicate_rejected = false;
    try {
        static_cast<void>(manager.start(
            first, options(executable, root, "normal"), message_handler, exit_handler));
    } catch (const std::invalid_argument&) {
        duplicate_rejected = true;
    }
    require(duplicate_rejected, "duplicate game_id was accepted");

    watchdog.expires_after(4s);
    watchdog.async_wait([&](const boost::system::error_code& error) {
        if (!error && exits != 2) throw std::runtime_error{"parallel children timed out"};
    });
    io.run();
    require(exits == 2 && manager.active_count() == 0 && children[0] != children[1],
            "parallel children were not independently owned and reaped");
    for (const pid_t child : children) {
        int status = 0;
        errno = 0;
        require(::waitpid(child, &status, WNOHANG) == -1 && errno == ECHILD,
                "parallel child left a zombie");
    }
}

void test_event_loop_remains_responsive(
    const std::filesystem::path& executable,
    const std::filesystem::path& root) {
    boost::asio::io_context io;
    GameProcessManager manager{io};
    GameProcessOptions settings = options(executable, root, "no-ready");
    settings.initialization_timeout = 80ms;
    bool heartbeat_fired = false;
    std::optional<GameProcessExit> exit;
    boost::asio::steady_timer heartbeat{io};
    boost::asio::steady_timer watchdog{io};

    heartbeat.expires_after(5ms);
    heartbeat.async_wait([&](const boost::system::error_code& error) {
        if (!error) heartbeat_fired = true;
    });
    static_cast<void>(manager.start(
        launch(root / "responsive", "game-responsive"), settings,
        [](const GameLaunchRecord&, const GameIpcMessage&) {},
        [&](const GameProcessExit& result) {
            exit = result;
            watchdog.cancel();
        }));
    watchdog.expires_after(4s);
    watchdog.async_wait([&](const boost::system::error_code& error) {
        if (!error && !exit.has_value()) {
            throw std::runtime_error{"event-loop responsiveness test timed out"};
        }
    });

    io.run();
    require(heartbeat_fired, "child supervision blocked the Asio event loop");
    require(exit.has_value() &&
                exit->failure_code == "IPC_INITIALIZATION_TIMEOUT",
            "responsive event-loop scenario did not reap the stalled child");
}

void test_manager_destruction_with_pending_callbacks(
    const std::filesystem::path& executable,
    const std::filesystem::path& root) {
    boost::asio::io_context io;
    pid_t child = -1;
    {
        auto manager = std::make_unique<GameProcessManager>(io);
        GameProcessOptions settings = options(executable, root, "no-ready");
        settings.initialization_timeout = 5s;
        child = manager->start(
            launch(root / "destroy-pending", "game-destroy-pending"), settings,
            [](const GameLaunchRecord&, const GameIpcMessage&) {},
            [](const GameProcessExit&) {});
    }
    io.run();

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::filesystem::exists("/proc/" + std::to_string(child)) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(2ms);
    }
    require(!std::filesystem::exists("/proc/" + std::to_string(child)),
            "manager destruction left a child or zombie behind");
}

void test_manager_destruction_from_exit_callback(
    const std::filesystem::path& executable,
    const std::filesystem::path& root) {
    boost::asio::io_context io;
    auto manager = std::make_unique<GameProcessManager>(io);
    int exit_notifications = 0;
    boost::asio::steady_timer watchdog{io};
    static_cast<void>(manager->start(
        launch(root / "destroy-in-callback", "game-destroy-in-callback"),
        options(executable, root, "normal"),
        [&](const GameLaunchRecord& record, const GameIpcMessage&) {
            manager->request_stop(record.game_id, "destroy callback test");
        },
        [&](const GameProcessExit& result) {
            require(result.expected, "callback destruction child did not exit normally");
            ++exit_notifications;
            manager.reset();
            watchdog.cancel();
        }));
    watchdog.expires_after(4s);
    watchdog.async_wait([&](const boost::system::error_code& error) {
        if (!error) throw std::runtime_error{"callback destruction timed out"};
    });
    io.run();
    require(exit_notifications == 1 && manager == nullptr,
            "manager destruction caused duplicate or missing exit notification");
}

void test_manager_destruction_from_message_callback(
    const std::filesystem::path& executable,
    const std::filesystem::path& root) {
    boost::asio::io_context io;
    auto manager = std::make_unique<GameProcessManager>(io);
    int messages = 0;
    const pid_t child = manager->start(
        launch(root / "destroy-in-message", "game-destroy-in-message"),
        options(executable, root, "normal"),
        [&](const GameLaunchRecord&, const GameIpcMessage&) {
            ++messages;
            manager.reset();
        },
        [](const GameProcessExit&) {});

    io.run();

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::filesystem::exists("/proc/" + std::to_string(child)) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(2ms);
    }
    require(messages == 1 && manager == nullptr,
            "message callback did not destroy the manager exactly once");
    require(!std::filesystem::exists("/proc/" + std::to_string(child)),
            "message callback destruction left a child or zombie behind");
}

std::size_t descriptor_count() {
    return static_cast<std::size_t>(std::distance(
        std::filesystem::directory_iterator{"/proc/self/fd"},
        std::filesystem::directory_iterator{}));
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error{"expected game process stub path"};
        TemporaryDirectory temporary;
        const auto executable = std::filesystem::absolute(argv[1]);
        const std::size_t descriptors_before = descriptor_count();
        test_normal_and_fd_isolation(executable, temporary.path());
        test_failures(executable, temporary.path());
        test_parent_write_pipe_closed(executable, temporary.path());
        test_backpressure(executable, temporary.path());
        test_forced_shutdown(executable, temporary.path());
        test_spawn_failure(temporary.path());
        test_multiple_children_and_duplicate_id(executable, temporary.path());
        test_event_loop_remains_responsive(executable, temporary.path());
        test_manager_destruction_with_pending_callbacks(executable, temporary.path());
        test_manager_destruction_from_exit_callback(executable, temporary.path());
        test_manager_destruction_from_message_callback(executable, temporary.path());
        const std::size_t descriptors_after = descriptor_count();
        require(descriptors_after <= descriptors_before + 1,
                "game process tests leaked file descriptors");
        std::cout << "Game process manager tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Game process manager test failed: " << error.what() << '\n';
        return 1;
    }
}
