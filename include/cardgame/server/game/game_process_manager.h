#pragma once

#include "cardgame/server/game/game_ipc.h"

#include <boost/asio/io_context.hpp>
#include <boost/json/object.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cardgame::server::game {

struct GamePackageBinding {
    std::string id;
    std::string commit;

    bool operator==(const GamePackageBinding&) const = default;
};

struct GamePlayerBinding {
    std::string player_id;
    std::uint8_t seat = 0;
    std::string display_name;

    bool operator==(const GamePlayerBinding&) const = default;
};

// 启动记录在 fork 前按值复制并在子进程生命周期内保持不可变。
// 它不包含 Session、socket 或可写 Room，因此子进程无法绕过主服务器边界。
struct GameLaunchRecord {
    std::string game_id;
    std::string room_id;
    std::string manifest_id;
    std::vector<GamePackageBinding> packages;
    std::string mode_id;
    std::vector<GamePlayerBinding> players;
    std::string seed;
    boost::json::object settings;
    std::string init_token;
    std::filesystem::path package_root;
};

enum class GameProcessState {
    starting,
    initializing,
    running,
    finishing,
    failed,
    terminating,
    reaped,
};

struct GameProcessOptions {
    std::filesystem::path executable;
    std::filesystem::path working_directory;
    std::vector<std::string> extra_arguments;
    std::chrono::milliseconds initialization_timeout{5000};
    std::chrono::milliseconds shutdown_grace{500};
    std::chrono::milliseconds terminate_grace{500};
    std::size_t maximum_queued_bytes = 4U * 1024U * 1024U;
    std::size_t maximum_queued_frames = 256;
};

struct GameProcessSnapshot {
    GameLaunchRecord launch;
    pid_t pid = -1;
    std::string child_identity;
    GameProcessState state = GameProcessState::starting;
    std::size_t queued_bytes = 0;
    std::size_t queued_frames = 0;
    std::optional<std::string> failure_code;
};

struct GameProcessExit {
    GameLaunchRecord launch;
    pid_t pid = -1;
    std::string child_identity;
    bool exited = false;
    int exit_code = -1;
    int termination_signal = 0;
    bool expected = false;
    std::optional<std::string> failure_code;
};

class GameProcessManager final {
public:
    using MessageHandler =
        std::function<void(const GameLaunchRecord&, const GameIpcMessage&)>;
    using ExitHandler = std::function<void(const GameProcessExit&)>;

    explicit GameProcessManager(boost::asio::io_context& io_context);
    ~GameProcessManager();

    GameProcessManager(const GameProcessManager&) = delete;
    GameProcessManager& operator=(const GameProcessManager&) = delete;

    pid_t start(
        GameLaunchRecord launch,
        GameProcessOptions options,
        MessageHandler message_handler,
        ExitHandler exit_handler);

    // sequence 和 game_id 由父进程覆盖生成，调用者不能自行指定 wire 身份。
    void send(
        std::string_view game_id,
        std::string type,
        boost::json::object payload,
        std::optional<std::string> request_token = std::nullopt);

    // 先排队 Shutdown；宽限期后依次 SIGTERM、SIGKILL，整个流程均为异步。
    void request_stop(std::string_view game_id, std::string reason);
    void request_stop_all(std::string reason);

    [[nodiscard]] std::optional<GameProcessSnapshot> snapshot(
        std::string_view game_id) const;
    [[nodiscard]] std::size_t active_count() const noexcept;

private:
    struct Process;

    void begin_read(const std::shared_ptr<Process>& process);
    void handle_body(const std::shared_ptr<Process>& process, std::string_view body);
    void enqueue(const std::shared_ptr<Process>& process, GameIpcMessage message);
    void begin_write(const std::shared_ptr<Process>& process);
    void schedule_reap(const std::shared_ptr<Process>& process);
    void reap(const std::shared_ptr<Process>& process, int wait_status);
    void fail_process(
        const std::shared_ptr<Process>& process,
        std::string code);
    void begin_termination(const std::shared_ptr<Process>& process);
    void close_descriptors(const std::shared_ptr<Process>& process);

    boost::asio::io_context& io_context_;
    // 异步回调先检查此令牌，避免管理器析构后再访问裸 this。
    // GameProcessManager 仍按服务端现有单线程事件模型调用。
    std::shared_ptr<std::atomic_bool> callback_lifetime_ =
        std::make_shared<std::atomic_bool>(true);
    std::unordered_map<std::string, std::shared_ptr<Process>> processes_;
    std::unordered_set<std::string> used_game_ids_;
};

}  // namespace cardgame::server::game
