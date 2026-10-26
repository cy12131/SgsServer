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

GameProcessManager::GameProcessManager(boost::asio::io_context& io_context)
    : io_context_{io_context} {
    // 管道对端关闭时 write(2) 可能产生 SIGPIPE。忽略该信号后，Asio 会以
    // broken_pipe 回调，进而只终止目标游戏进程而不是整个服务端。
    std::signal(SIGPIPE, SIG_IGN);
}

GameProcessManager::~GameProcessManager() {
    for (auto& [game_id, process] : processes_) {
        static_cast<void>(game_id);
        close_descriptors(process);
        process->initialization_timer.cancel();
        process->reap_timer.cancel();
        process->shutdown_timer.cancel();
        process->kill_timer.cancel();
        if (process->pid > 0) {
            ::kill(process->pid, SIGKILL);
            int status = 0;
            static_cast<void>(::waitpid(process->pid, &status, WNOHANG));
        }
    }
}

pid_t GameProcessManager::start(
    GameLaunchRecord launch,
    GameProcessOptions options,
    MessageHandler message_handler,
    ExitHandler exit_handler) {
    if (used_game_ids_.contains(launch.game_id)) {
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
        io_context_, std::move(launch), std::move(options),
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
    processes_.emplace(process->launch.game_id, process);
    used_game_ids_.insert(process->launch.game_id);

    begin_read(process);
    schedule_reap(process);
    process->initialization_timer.expires_after(process->options.initialization_timeout);
    process->initialization_timer.async_wait([this, process](const boost::system::error_code& error) {
        if (!error && process->state == GameProcessState::initializing) {
            fail_process(process, "IPC_INITIALIZATION_TIMEOUT");
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

void GameProcessManager::send(
    std::string_view game_id,
    std::string type,
    boost::json::object payload,
    std::optional<std::string> request_token) {
    const auto found = processes_.find(std::string{game_id});
    if (found == processes_.end() || found->second->state != GameProcessState::running) {
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

void GameProcessManager::request_stop(std::string_view game_id, std::string reason) {
    const auto found = processes_.find(std::string{game_id});
    if (found == processes_.end()) {
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
    process->shutdown_timer.async_wait([this, process](const boost::system::error_code& error) {
        if (!error && process->state != GameProcessState::reaped) {
            if (!process->failure_code.has_value()) {
                process->failure_code = "IPC_SHUTDOWN_TIMEOUT";
            }
            begin_termination(process);
        }
    });
}

void GameProcessManager::request_stop_all(std::string reason) {
    std::vector<std::string> ids;
    ids.reserve(processes_.size());
    for (const auto& [game_id, process] : processes_) {
        static_cast<void>(process);
        ids.push_back(game_id);
    }
    for (const std::string& game_id : ids) {
        request_stop(game_id, reason);
    }
}

std::optional<GameProcessSnapshot> GameProcessManager::snapshot(
    std::string_view game_id) const {
    const auto found = processes_.find(std::string{game_id});
    if (found == processes_.end()) {
        return std::nullopt;
    }
    const Process& process = *found->second;
    return GameProcessSnapshot{
        process.launch, process.pid, process.child_identity, process.state,
        process.queued_bytes, process.writes.size(), process.failure_code};
}

std::size_t GameProcessManager::active_count() const noexcept {
    return processes_.size();
}

void GameProcessManager::begin_read(const std::shared_ptr<Process>& process) {
    process->read_pipe.async_read_some(
        boost::asio::buffer(process->read_buffer),
        [this, process](const boost::system::error_code& error, std::size_t count) {
            if (error) {
                if (error != boost::asio::error::operation_aborted &&
                    process->state != GameProcessState::reaped &&
                    !process->stop_requested) {
                    fail_process(process, "IPC_PIPE_CLOSED");
                }
                return;
            }
            try {
                const auto bodies = process->frame_decoder.feed(
                    std::span<const std::uint8_t>{process->read_buffer.data(), count});
                for (const std::string& body : bodies) {
                    handle_body(process, body);
                    if (process->failure_code.has_value()) {
                        return;
                    }
                }
            } catch (const GameIpcError& exception) {
                fail_process(process, exception.code);
                return;
            }
            begin_read(process);
        });
}

void GameProcessManager::handle_body(
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

    if (process->message_handler) {
        process->message_handler(process->launch, message);
    }
    if (message.type == "GameFailed") {
        fail_process(process, "IPC_CHILD_REPORTED_FAILURE");
    } else if (message.type == "GameFinished") {
        request_stop(process->launch.game_id, "game finished");
    }
}

void GameProcessManager::enqueue(
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

void GameProcessManager::begin_write(const std::shared_ptr<Process>& process) {
    if (process->write_in_progress || process->writes.empty() ||
        process->descriptors_closed) {
        return;
    }
    process->write_in_progress = true;
    boost::asio::async_write(
        process->write_pipe,
        boost::asio::buffer(process->writes.front()),
        [this, process](const boost::system::error_code& error, std::size_t) {
            process->write_in_progress = false;
            if (error) {
                if (error != boost::asio::error::operation_aborted &&
                    process->state != GameProcessState::reaped) {
                    fail_process(process, "IPC_PIPE_CLOSED");
                }
                return;
            }
            process->queued_bytes -= process->writes.front().size();
            process->writes.pop_front();
            begin_write(process);
        });
}

void GameProcessManager::schedule_reap(const std::shared_ptr<Process>& process) {
    process->reap_timer.expires_after(std::chrono::milliseconds{10});
    process->reap_timer.async_wait([this, process](const boost::system::error_code& error) {
        if (error || process->state == GameProcessState::reaped) {
            return;
        }
        int status = 0;
        const pid_t result = ::waitpid(process->pid, &status, WNOHANG);
        if (result == process->pid) {
            reap(process, status);
        } else if (result == 0 || (result < 0 && errno == EINTR)) {
            schedule_reap(process);
        } else if (result < 0 && errno != ECHILD) {
            fail_process(process, "IPC_REAP_FAILED");
            schedule_reap(process);
        }
    });
}

void GameProcessManager::reap(
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
    if (!process->failure_code.has_value() &&
        ((!result.exited) || result.exit_code != 0)) {
        process->failure_code = "IPC_CHILD_EXITED";
    }
    result.failure_code = process->failure_code;
    result.expected = result.exited && result.exit_code == 0 &&
                      !result.failure_code.has_value();

    processes_.erase(process->launch.game_id);
    if (process->exit_handler) {
        process->exit_handler(result);
    }
}

void GameProcessManager::fail_process(
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

void GameProcessManager::begin_termination(
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

void GameProcessManager::close_descriptors(
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

}  // namespace cardgame::server::game
