#include "cardgame/server/game/game_process_manager.h"

#include <boost/asio/buffer.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/error_code.hpp>

#include <array>
#include <cerrno>
#include <csignal>
#include <deque>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace cardgame::server::game {
namespace {

constexpr int child_read_descriptor = 3;
constexpr int child_write_descriptor = 4;

void close_if_open(int& descriptor) noexcept {
    if (descriptor >= 0) {
        while (::close(descriptor) < 0 && errno == EINTR) {
        }
        descriptor = -1;
    }
}

void make_pipe(std::array<int, 2>& descriptors) {
    if (::pipe2(descriptors.data(), O_CLOEXEC | O_NONBLOCK) != 0) {
        throw std::system_error{errno, std::generic_category(), "pipe2"};
    }
}

int duplicate_child_end(int descriptor) {
    const int duplicate = ::fcntl(descriptor, F_DUPFD_CLOEXEC, 10);
    if (duplicate < 0) {
        throw std::system_error{errno, std::generic_category(), "fcntl(F_DUPFD_CLOEXEC)"};
    }
    return duplicate;
}

void close_unneeded_child_descriptors() noexcept {
#ifdef SYS_close_range
    if (::syscall(SYS_close_range, 5U, std::numeric_limits<unsigned int>::max(), 0U) == 0) {
        return;
    }
#endif
    // close_range 不可用时使用固定保守上限；测试同时验证目标 IPC 句柄之外
    // 的显式哨兵描述符不会被继承。
    for (int descriptor = 5; descriptor < 4096; ++descriptor) {
        ::close(descriptor);
    }
}

void reap_after_manager_destruction(pid_t pid) noexcept {
    int status = 0;
    pid_t result = -1;
    do {
        result = ::waitpid(pid, &status, WNOHANG);
    } while (result < 0 && errno == EINTR);
    if (result != 0) {
        return;
    }

    // 析构只在服务组件本身结束时走此兜底。等待精确 PID 的工作交给
    // 独立 reaper，避免阻塞 Asio 线程，同时保证 SIGKILL 后不留 zombie。
    try {
        std::thread{[pid] {
            int child_status = 0;
            while (::waitpid(pid, &child_status, 0) < 0 && errno == EINTR) {
            }
        }}.detach();
    } catch (...) {
        // 极端资源耗尽时不能从析构抛异常；SIGKILL 已发送，父进程结束后
        // 该子进程仍会由 init 接管回收。
    }
}

boost::json::array package_json(const std::vector<GamePackageBinding>& packages) {
    boost::json::array result;
    result.reserve(packages.size());
    for (const GamePackageBinding& package : packages) {
        result.emplace_back(boost::json::object{
            {"id", package.id},
            {"commit", package.commit},
        });
    }
    return result;
}

boost::json::array player_json(const std::vector<GamePlayerBinding>& players) {
    boost::json::array result;
    result.reserve(players.size());
    for (const GamePlayerBinding& player : players) {
        result.emplace_back(boost::json::object{
            {"player_id", player.player_id},
            {"seat", player.seat},
            {"display_name", player.display_name},
        });
    }
    return result;
}

bool same_packages(
    const boost::json::array& actual,
    const std::vector<GamePackageBinding>& expected) {
    if (actual.size() != expected.size()) {
        return false;
    }
    for (std::size_t index = 0; index < actual.size(); ++index) {
        if (!actual[index].is_object()) {
            return false;
        }
        const auto& object = actual[index].as_object();
        const auto* id = object.if_contains("id");
        const auto* commit = object.if_contains("commit");
        if (id == nullptr || commit == nullptr || !id->is_string() ||
            !commit->is_string() || id->as_string() != expected[index].id ||
            commit->as_string() != expected[index].commit) {
            return false;
        }
    }
    return true;
}

bool roster_contains(const GameLaunchRecord& launch, std::string_view player_id) {
    for (const GamePlayerBinding& player : launch.players) {
        if (player.player_id == player_id) {
            return true;
        }
    }
    return false;
}

bool recipients_belong_to_game(
    const GameLaunchRecord& launch,
    const boost::json::object& payload) {
    const auto* value = payload.if_contains("recipients");
    if (value == nullptr || !value->is_array()) {
        return false;
    }
    for (const boost::json::value& recipient : value->as_array()) {
        if (!recipient.is_string() || !roster_contains(launch, recipient.as_string())) {
            return false;
        }
    }
    return true;
}

}  // namespace

struct GameProcessManager::Process {
    Process(boost::asio::io_context& io, GameLaunchRecord launch_value,
            GameProcessOptions options_value, MessageHandler message_value,
            ExitHandler exit_value)
        : launch{std::move(launch_value)},
          options{std::move(options_value)},
          message_handler{std::move(message_value)},
          exit_handler{std::move(exit_value)},
          read_pipe{io},
          write_pipe{io},
          initialization_timer{io},
          reap_timer{io},
          shutdown_timer{io},
          kill_timer{io} {}

    GameLaunchRecord launch;
    GameProcessOptions options;
    MessageHandler message_handler;
    ExitHandler exit_handler;
    pid_t pid = -1;
    std::string child_identity;
    GameProcessState state = GameProcessState::starting;
    boost::asio::posix::stream_descriptor read_pipe;
    boost::asio::posix::stream_descriptor write_pipe;
    boost::asio::steady_timer initialization_timer;
    boost::asio::steady_timer reap_timer;
    boost::asio::steady_timer shutdown_timer;
    boost::asio::steady_timer kill_timer;
    std::array<std::uint8_t, 16U * 1024U> read_buffer{};
    GameIpcFrameDecoder frame_decoder;
    GameIpcSequenceGenerator outgoing_sequence;
    GameIpcSequenceValidator incoming_sequence;
    std::deque<std::vector<std::uint8_t>> writes;
    std::size_t queued_bytes = 0;
    bool write_in_progress = false;
    bool stop_requested = false;
    bool descriptors_closed = false;
    bool exit_reported = false;
    std::optional<std::string> failure_code;
    std::unordered_set<std::string> child_request_tokens;
};

struct GameProcessManager::State final
    : std::enable_shared_from_this<GameProcessManager::State> {
    explicit State(boost::asio::io_context& io_context_value)
        : io_context{io_context_value} {}

    pid_t start(
        GameLaunchRecord launch,
        GameProcessOptions options,
        MessageHandler message_handler,
        ExitHandler exit_handler);
    void send(
        std::string_view game_id,
        std::string type,
        boost::json::object payload,
        std::optional<std::string> request_token);
    void request_stop(std::string_view game_id, std::string reason);
    void request_stop_all(std::string reason);
    [[nodiscard]] std::optional<GameProcessSnapshot> snapshot(
        std::string_view game_id) const;
    [[nodiscard]] std::size_t active_count() const noexcept;
    void shutdown() noexcept;

    void begin_read(const std::shared_ptr<Process>& process);
    void handle_body(const std::shared_ptr<Process>& process, std::string_view body);
    void enqueue(const std::shared_ptr<Process>& process, GameIpcMessage message);
    void begin_write(const std::shared_ptr<Process>& process);
    void schedule_reap(const std::shared_ptr<Process>& process);
    void reap(const std::shared_ptr<Process>& process, int wait_status);
    void fail_process(const std::shared_ptr<Process>& process, std::string code);
    void begin_termination(const std::shared_ptr<Process>& process);
    void close_descriptors(const std::shared_ptr<Process>& process);

    boost::asio::io_context& io_context;
    bool shutting_down = false;
    std::unordered_map<std::string, std::shared_ptr<Process>> processes;
    std::unordered_set<std::string> used_game_ids;
};

GameProcessManager::GameProcessManager(boost::asio::io_context& io_context)
    : state_{std::make_shared<State>(io_context)} {
    // 管道对端关闭时 write(2) 可能产生 SIGPIPE。忽略该信号后，Asio 会以
    // broken_pipe 回调，进而只终止目标游戏进程而不是整个服务端。
    std::signal(SIGPIPE, SIG_IGN);
}

GameProcessManager::~GameProcessManager() {
    state_->shutdown();
}

void GameProcessManager::State::shutdown() noexcept {
    if (shutting_down) {
        return;
    }
    shutting_down = true;
    for (auto& [game_id, process] : processes) {
        static_cast<void>(game_id);
        close_descriptors(process);
        process->initialization_timer.cancel();
        process->reap_timer.cancel();
        process->shutdown_timer.cancel();
        process->kill_timer.cancel();
        if (process->pid > 0) {
            ::kill(process->pid, SIGKILL);
            reap_after_manager_destruction(process->pid);
        }
    }
    processes.clear();
}

pid_t GameProcessManager::State::start(
    GameLaunchRecord launch,
    GameProcessOptions options,
    MessageHandler message_handler,
    ExitHandler exit_handler) {
    if (shutting_down) {
        throw std::logic_error{"game process manager is shutting down"};
    }
    if (used_game_ids.contains(launch.game_id)) {
        throw std::invalid_argument{"game_id cannot be reused in one server process"};
    }
    if (!options.executable.is_absolute() || ::access(options.executable.c_str(), X_OK) != 0) {
        throw std::invalid_argument{"runtime executable must be an executable absolute path"};
    }
    if (!options.working_directory.is_absolute() ||
        !std::filesystem::is_directory(options.working_directory)) {
        throw std::invalid_argument{"runtime working directory must be an existing absolute path"};
    }
    if (!launch.package_root.is_absolute() ||
        options.maximum_queued_bytes == 0 || options.maximum_queued_frames == 0) {
        throw std::invalid_argument{"runtime package root and queue limits are invalid"};
    }

    auto process = std::make_shared<Process>(
        io_context, std::move(launch), std::move(options),
        std::move(message_handler), std::move(exit_handler));

    // fork 前先完成所有可能分配内存的 argv/env 构造。子进程只调用
    // async-signal-safe 的 fd/chdir/exec 系统调用。
    std::vector<std::string> arguments{
        process->options.executable.string(),
        "--game-id", process->launch.game_id,
        "--package-root", process->launch.package_root.string(),
        "--ipc-read", std::to_string(child_read_descriptor),
        "--ipc-write", std::to_string(child_write_descriptor),
    };
    arguments.insert(arguments.end(), process->options.extra_arguments.begin(),
                     process->options.extra_arguments.end());
    std::vector<char*> argv;
    argv.reserve(arguments.size() + 1);
    for (std::string& argument : arguments) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);
    std::array<std::string, 3> environment_values{
        "PATH=/usr/bin:/bin", "LANG=C.UTF-8", "TZ=UTC"};
    std::array<char*, 4> environment{
        environment_values[0].data(), environment_values[1].data(),
        environment_values[2].data(), nullptr};

    std::array<int, 2> parent_to_child{-1, -1};
    std::array<int, 2> child_to_parent{-1, -1};
    int child_read = -1;
    int child_write = -1;
    try {
        make_pipe(parent_to_child);
        make_pipe(child_to_parent);
        child_read = duplicate_child_end(parent_to_child[0]);
        child_write = duplicate_child_end(child_to_parent[1]);
        close_if_open(parent_to_child[0]);
        close_if_open(child_to_parent[1]);
    } catch (...) {
        close_if_open(parent_to_child[0]);
        close_if_open(parent_to_child[1]);
        close_if_open(child_to_parent[0]);
        close_if_open(child_to_parent[1]);
        close_if_open(child_read);
        close_if_open(child_write);
        throw;
    }

    const pid_t child = ::fork();
    if (child == 0) {
        if (::chdir(process->options.working_directory.c_str()) != 0 ||
            ::dup2(child_read, child_read_descriptor) < 0 ||
            ::dup2(child_write, child_write_descriptor) < 0) {
            _exit(126);
        }
        close_unneeded_child_descriptors();
        ::execve(process->options.executable.c_str(), argv.data(), environment.data());
        _exit(127);
    }
    close_if_open(child_read);
    close_if_open(child_write);
    if (child < 0) {
        close_if_open(parent_to_child[1]);
        close_if_open(child_to_parent[0]);
        throw std::system_error{errno, std::generic_category(), "fork"};
    }

    process->pid = child;
    process->child_identity = "pid:" + std::to_string(child);
    process->state = GameProcessState::initializing;
    process->read_pipe.assign(child_to_parent[0]);
    process->write_pipe.assign(parent_to_child[1]);
    processes.emplace(process->launch.game_id, process);
    used_game_ids.insert(process->launch.game_id);

    begin_read(process);
    schedule_reap(process);
    process->initialization_timer.expires_after(process->options.initialization_timeout);
    const auto initialization_state = shared_from_this();
    process->initialization_timer.async_wait([initialization_state, process](const boost::system::error_code& error) {
        if (initialization_state->shutting_down) {
            return;
        }
        if (!error && process->state == GameProcessState::initializing) {
            initialization_state->fail_process(process, "IPC_INITIALIZATION_TIMEOUT");
        }
    });

    GameIpcMessage init;
    init.type = "InitGame";
    init.game_id = process->launch.game_id;
    init.sequence = process->outgoing_sequence.take();
    init.request_token = process->launch.init_token;
    init.payload = {
        {"room_id", process->launch.room_id},
        {"manifest_id", process->launch.manifest_id},
        {"mode_id", process->launch.mode_id},
        {"seed", process->launch.seed},
        {"players", player_json(process->launch.players)},
        {"packages", package_json(process->launch.packages)},
        {"settings", process->launch.settings},
    };
    enqueue(process, std::move(init));
    return child;
}

void GameProcessManager::State::send(
    std::string_view game_id,
    std::string type,
    boost::json::object payload,
    std::optional<std::string> request_token) {
    const auto found = processes.find(std::string{game_id});
    if (found == processes.end() || found->second->state != GameProcessState::running) {
        throw std::invalid_argument{"game process is not running"};
    }
    GameIpcMessage message;
    message.type = std::move(type);
    message.game_id = found->second->launch.game_id;
    message.sequence = found->second->outgoing_sequence.take();
    message.request_token = std::move(request_token);
    message.payload = std::move(payload);
    enqueue(found->second, std::move(message));
}

void GameProcessManager::State::request_stop(std::string_view game_id, std::string reason) {
    const auto found = processes.find(std::string{game_id});
    if (found == processes.end()) {
        return;
    }
    const std::shared_ptr<Process>& process = found->second;
    if (process->stop_requested || process->state == GameProcessState::reaped ||
        process->state == GameProcessState::terminating) {
        return;
    }
    process->stop_requested = true;
    GameIpcMessage shutdown;
    shutdown.type = "Shutdown";
    shutdown.game_id = process->launch.game_id;
    shutdown.sequence = process->outgoing_sequence.take();
    shutdown.payload = {{"reason", std::move(reason)}};
    enqueue(process, std::move(shutdown));
    if (process->failure_code.has_value()) {
        return;
    }
    process->state = GameProcessState::finishing;
    process->shutdown_timer.expires_after(process->options.shutdown_grace);
    const auto shutdown_state = shared_from_this();
    process->shutdown_timer.async_wait([shutdown_state, process](const boost::system::error_code& error) {
        if (shutdown_state->shutting_down) {
            return;
        }
        if (!error && process->state != GameProcessState::reaped) {
            if (!process->failure_code.has_value()) {
                process->failure_code = "IPC_SHUTDOWN_TIMEOUT";
            }
            shutdown_state->begin_termination(process);
        }
    });
}

void GameProcessManager::State::request_stop_all(std::string reason) {
    std::vector<std::string> ids;
    ids.reserve(processes.size());
    for (const auto& [game_id, process] : processes) {
        static_cast<void>(process);
        ids.push_back(game_id);
    }
    for (const std::string& game_id : ids) {
        request_stop(game_id, reason);
    }
}

std::optional<GameProcessSnapshot> GameProcessManager::State::snapshot(
    std::string_view game_id) const {
    const auto found = processes.find(std::string{game_id});
    if (found == processes.end()) {
        return std::nullopt;
    }
    const Process& process = *found->second;
    return GameProcessSnapshot{
        process.launch, process.pid, process.child_identity, process.state,
        process.queued_bytes, process.writes.size(), process.failure_code};
}

std::size_t GameProcessManager::State::active_count() const noexcept {
    return processes.size();
}

void GameProcessManager::State::begin_read(const std::shared_ptr<Process>& process) {
    const auto state = shared_from_this();
    process->read_pipe.async_read_some(
        boost::asio::buffer(process->read_buffer),
        [state, process](const boost::system::error_code& error, std::size_t count) {
            if (state->shutting_down) {
                return;
            }
            if (error) {
                if (error != boost::asio::error::operation_aborted &&
                    process->state != GameProcessState::reaped &&
                    !process->stop_requested) {
                    state->fail_process(process, "IPC_PIPE_CLOSED");
                }
                return;
            }
            try {
                const auto bodies = process->frame_decoder.feed(
                    std::span<const std::uint8_t>{process->read_buffer.data(), count});
                for (const std::string& body : bodies) {
                    state->handle_body(process, body);
                    if (state->shutting_down ||
                        process->failure_code.has_value()) {
                        return;
                    }
                }
            } catch (const GameIpcError& exception) {
                state->fail_process(process, exception.code);
                return;
            }
            if (!state->shutting_down) {
                state->begin_read(process);
            }
        });
}

void GameProcessManager::State::handle_body(
    const std::shared_ptr<Process>& process,
    std::string_view body) {
    GameIpcMessage message = GameIpcCodec::decode_body(
        body, GameIpcDirection::child_to_parent);
    process->incoming_sequence.accept(message.sequence);
    if (message.game_id != process->launch.game_id) {
        fail_process(process, "IPC_BINDING_MISMATCH");
        return;
    }
    if (message.type == "GameReady") {
        const auto* mode = message.payload.if_contains("mode_id");
        const auto* packages = message.payload.if_contains("loaded_packages");
        if (process->state != GameProcessState::initializing ||
            message.request_token != process->launch.init_token ||
            mode == nullptr || !mode->is_string() ||
            mode->as_string() != process->launch.mode_id || packages == nullptr ||
            !packages->is_array() ||
            !same_packages(packages->as_array(), process->launch.packages)) {
            fail_process(process, "IPC_BINDING_MISMATCH");
            return;
        }
        process->initialization_timer.cancel();
        process->state = GameProcessState::running;
    } else if (process->state != GameProcessState::running) {
        fail_process(process, "IPC_STATE_INVALID");
        return;
    }

    if ((message.type == "PlayerRequest" || message.type == "GameOutput") &&
        !recipients_belong_to_game(process->launch, message.payload)) {
        fail_process(process, "IPC_BINDING_MISMATCH");
        return;
    }
    if (message.type == "PlayerRequest" &&
        !process->child_request_tokens.insert(*message.request_token).second) {
        fail_process(process, "IPC_TOKEN_REUSED");
        return;
    }

    if (message.type == "GameFailed") {
        fail_process(process, "IPC_CHILD_REPORTED_FAILURE");
    } else if (message.type == "GameFinished") {
        request_stop(process->launch.game_id, "game finished");
    }
    // 先完成框架生命周期转换，再交给观察回调。即使回调销毁管理器，
    // 当前函数返回前也不会再访问管理器状态。
    if (process->message_handler) {
        process->message_handler(process->launch, message);
    }
}

void GameProcessManager::State::enqueue(
    const std::shared_ptr<Process>& process,
    GameIpcMessage message) {
    std::vector<std::uint8_t> frame;
    try {
        frame = GameIpcCodec::encode_frame(
            message, GameIpcDirection::parent_to_child);
    } catch (const GameIpcError& exception) {
        fail_process(process, exception.code);
        return;
    }
    if (process->writes.size() >= process->options.maximum_queued_frames ||
        frame.size() > process->options.maximum_queued_bytes -
                           std::min(process->queued_bytes,
                                    process->options.maximum_queued_bytes)) {
        fail_process(process, "IPC_BACKPRESSURE");
        return;
    }
    process->queued_bytes += frame.size();
    process->writes.push_back(std::move(frame));
    begin_write(process);
}

void GameProcessManager::State::begin_write(const std::shared_ptr<Process>& process) {
    if (process->write_in_progress || process->writes.empty() ||
        process->descriptors_closed) {
        return;
    }
    process->write_in_progress = true;
    const auto state = shared_from_this();
    boost::asio::async_write(
        process->write_pipe,
        boost::asio::buffer(process->writes.front()),
        [state, process](const boost::system::error_code& error, std::size_t) {
            if (state->shutting_down) {
                return;
            }
            process->write_in_progress = false;
            if (error) {
                if (error != boost::asio::error::operation_aborted &&
                    process->state != GameProcessState::reaped) {
                    state->fail_process(process, "IPC_PIPE_CLOSED");
                }
                return;
            }
            process->queued_bytes -= process->writes.front().size();
            process->writes.pop_front();
            state->begin_write(process);
        });
}

void GameProcessManager::State::schedule_reap(const std::shared_ptr<Process>& process) {
    process->reap_timer.expires_after(std::chrono::milliseconds{10});
    const auto state = shared_from_this();
    process->reap_timer.async_wait([state, process](const boost::system::error_code& error) {
        if (state->shutting_down) {
            return;
        }
        if (error || process->state == GameProcessState::reaped) {
            return;
        }
        int status = 0;
        const pid_t result = ::waitpid(process->pid, &status, WNOHANG);
        if (result == process->pid) {
            state->reap(process, status);
        } else if (result == 0 || (result < 0 && errno == EINTR)) {
            state->schedule_reap(process);
        } else if (result < 0 && errno != ECHILD) {
            state->fail_process(process, "IPC_REAP_FAILED");
            state->schedule_reap(process);
        }
    });
}

void GameProcessManager::State::reap(
    const std::shared_ptr<Process>& process,
    int wait_status) {
    if (process->exit_reported) {
        return;
    }
    process->exit_reported = true;
    process->state = GameProcessState::reaped;
    close_descriptors(process);
    process->initialization_timer.cancel();
    process->reap_timer.cancel();
    process->shutdown_timer.cancel();
    process->kill_timer.cancel();

    GameProcessExit result;
    result.launch = process->launch;
    result.pid = process->pid;
    result.child_identity = process->child_identity;
    result.exited = WIFEXITED(wait_status);
    if (result.exited) {
        result.exit_code = WEXITSTATUS(wait_status);
    }
    if (WIFSIGNALED(wait_status)) {
        result.termination_signal = WTERMSIG(wait_status);
    }
    // 只有父进程已进入有序停止流程且子进程以 0 退出，才是预期退出。
    // Runtime 在 GameFinished/Shutdown 之前自行以 0 退出仍属于协议失败。
    if (!process->failure_code.has_value() &&
        ((!result.exited) || result.exit_code != 0 || !process->stop_requested)) {
        process->failure_code = "IPC_CHILD_EXITED";
    }
    result.failure_code = process->failure_code;
    result.expected = result.exited && result.exit_code == 0 &&
                      !result.failure_code.has_value();

    processes.erase(process->launch.game_id);
    if (process->exit_handler) {
        process->exit_handler(result);
    }
}

void GameProcessManager::State::fail_process(
    const std::shared_ptr<Process>& process,
    std::string code) {
    if (process->state == GameProcessState::reaped ||
        process->state == GameProcessState::terminating ||
        process->failure_code.has_value()) {
        return;
    }
    process->failure_code = std::move(code);
    process->state = GameProcessState::failed;
    process->initialization_timer.cancel();
    process->shutdown_timer.cancel();
    begin_termination(process);
}

void GameProcessManager::State::begin_termination(
    const std::shared_ptr<Process>& process) {
    if (process->state == GameProcessState::reaped) {
        return;
    }
    process->state = GameProcessState::terminating;
    close_descriptors(process);
    if (process->pid > 0) {
        ::kill(process->pid, SIGTERM);
    }
    process->kill_timer.expires_after(process->options.terminate_grace);
    process->kill_timer.async_wait([process](const boost::system::error_code& error) {
        if (!error && process->state != GameProcessState::reaped && process->pid > 0) {
            ::kill(process->pid, SIGKILL);
        }
    });
}

void GameProcessManager::State::close_descriptors(
    const std::shared_ptr<Process>& process) {
    if (process->descriptors_closed) {
        return;
    }
    process->descriptors_closed = true;
    boost::system::error_code ignored;
    process->read_pipe.cancel(ignored);
    process->read_pipe.close(ignored);
    process->write_pipe.cancel(ignored);
    process->write_pipe.close(ignored);
}

pid_t GameProcessManager::start(
    GameLaunchRecord launch,
    GameProcessOptions options,
    MessageHandler message_handler,
    ExitHandler exit_handler) {
    return state_->start(
        std::move(launch), std::move(options),
        std::move(message_handler), std::move(exit_handler));
}

void GameProcessManager::send(
    std::string_view game_id,
    std::string type,
    boost::json::object payload,
    std::optional<std::string> request_token) {
    state_->send(game_id, std::move(type), std::move(payload),
                 std::move(request_token));
}

void GameProcessManager::request_stop(std::string_view game_id, std::string reason) {
    state_->request_stop(game_id, std::move(reason));
}

void GameProcessManager::request_stop_all(std::string reason) {
    state_->request_stop_all(std::move(reason));
}

std::optional<GameProcessSnapshot> GameProcessManager::snapshot(
    std::string_view game_id) const {
    return state_->snapshot(game_id);
}

std::size_t GameProcessManager::active_count() const noexcept {
    return state_->active_count();
}

}  // namespace cardgame::server::game
