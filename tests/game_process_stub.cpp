#include "cardgame/server/game/game_ipc.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using cardgame::server::game::GameIpcCodec;
using cardgame::server::game::GameIpcDirection;
using cardgame::server::game::GameIpcFrameDecoder;
using cardgame::server::game::GameIpcMessage;

struct Arguments {
    std::string game_id;
    int read_descriptor = -1;
    int write_descriptor = -1;
    std::string mode = "normal";
    int sentinel_descriptor = -1;
};

Arguments parse_arguments(int count, char** values) {
    Arguments result;
    for (int index = 1; index + 1 < count; index += 2) {
        const std::string_view name{values[index]};
        const std::string value{values[index + 1]};
        if (name == "--game-id") result.game_id = value;
        else if (name == "--ipc-read") result.read_descriptor = std::stoi(value);
        else if (name == "--ipc-write") result.write_descriptor = std::stoi(value);
        else if (name == "--mode") result.mode = value;
        else if (name == "--sentinel-fd") result.sentinel_descriptor = std::stoi(value);
        else if (name == "--package-root") static_cast<void>(value);
        else throw std::runtime_error{"unknown argument"};
    }
    if (result.game_id.empty() || result.read_descriptor < 0 ||
        result.write_descriptor < 0) {
        throw std::runtime_error{"missing IPC arguments"};
    }
    return result;
}

void wait_descriptor(int descriptor, short events) {
    pollfd poll_descriptor{descriptor, events, 0};
    while (true) {
        const int result = ::poll(&poll_descriptor, 1, 5000);
        if (result > 0) return;
        if (result < 0 && errno == EINTR) continue;
        throw std::runtime_error{"IPC poll failed or timed out"};
    }
}

std::string read_body(int descriptor) {
    GameIpcFrameDecoder decoder;
    std::array<std::uint8_t, 7> buffer{};
    while (true) {
        wait_descriptor(descriptor, POLLIN);
        const ssize_t count = ::read(descriptor, buffer.data(), buffer.size());
        if (count == 0) throw std::runtime_error{"IPC EOF"};
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            throw std::runtime_error{"IPC read failed"};
        }
        auto bodies = decoder.feed(std::span<const std::uint8_t>{
            buffer.data(), static_cast<std::size_t>(count)});
        if (!bodies.empty()) return std::move(bodies.front());
    }
}

void write_all(int descriptor, const std::uint8_t* bytes, std::size_t size) {
    std::size_t written = 0;
    while (written < size) {
        wait_descriptor(descriptor, POLLOUT);
        const std::size_t request = std::min<std::size_t>(3, size - written);
        const ssize_t count = ::write(descriptor, bytes + written, request);
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            throw std::runtime_error{"IPC write failed"};
        }
        written += static_cast<std::size_t>(count);
    }
}

void write_frame(int descriptor, const GameIpcMessage& message) {
    const auto frame = GameIpcCodec::encode_frame(
        message, GameIpcDirection::child_to_parent);
    write_all(descriptor, frame.data(), frame.size());
}

void write_raw_frame(int descriptor, std::string_view body) {
    const auto size = static_cast<std::uint32_t>(body.size());
    std::array<std::uint8_t, 4> header{
        static_cast<std::uint8_t>((size >> 24U) & 0xFFU),
        static_cast<std::uint8_t>((size >> 16U) & 0xFFU),
        static_cast<std::uint8_t>((size >> 8U) & 0xFFU),
        static_cast<std::uint8_t>(size & 0xFFU),
    };
    write_all(descriptor, header.data(), header.size());
    write_all(descriptor, reinterpret_cast<const std::uint8_t*>(body.data()), body.size());
}

GameIpcMessage ready_from(const GameIpcMessage& init, std::uint64_t sequence = 1) {
    GameIpcMessage ready;
    ready.type = "GameReady";
    ready.game_id = init.game_id;
    ready.sequence = sequence;
    ready.request_token = init.request_token;
    ready.payload = {
        {"mode_id", init.payload.at("mode_id")},
        {"runtime_api_version", "1.0-draft"},
        {"loaded_packages", init.payload.at("packages")},
    };
    return ready;
}

void wait_forever() {
    while (true) {
        ::pause();
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Arguments arguments = parse_arguments(argc, argv);
        if (arguments.sentinel_descriptor >= 0 &&
            (::fcntl(arguments.sentinel_descriptor, F_GETFD) >= 0 || errno != EBADF)) {
            return 31;
        }

        const GameIpcMessage init = GameIpcCodec::decode_body(
            read_body(arguments.read_descriptor),
            GameIpcDirection::parent_to_child);
        if (init.type != "InitGame" || init.game_id != arguments.game_id) return 32;

        if (arguments.mode == "crash") return 23;
        if (arguments.mode == "invalid-json") {
            write_raw_frame(arguments.write_descriptor, "{not-json");
            wait_forever();
        }
        if (arguments.mode == "unknown-type") {
            write_raw_frame(arguments.write_descriptor,
                "{\"ipc_version\":\"1\",\"type\":\"Unknown\",\"game_id\":\"game-test-0001\",\"sequence\":\"1\",\"payload\":{}}");
            wait_forever();
        }
        if (arguments.mode == "oversize") {
            const std::array<std::uint8_t, 4> header{0x00, 0x10, 0x00, 0x01};
            write_all(arguments.write_descriptor, header.data(), header.size());
            wait_forever();
        }
        if (arguments.mode == "truncated") {
            const std::array<std::uint8_t, 4> header{0x00, 0x00, 0x00, 0x40};
            const std::array<std::uint8_t, 3> partial{'{', '"', 'x'};
            write_all(arguments.write_descriptor, header.data(), header.size());
            write_all(arguments.write_descriptor, partial.data(), partial.size());
            ::close(arguments.write_descriptor);
            wait_forever();
        }
        if (arguments.mode == "gap") {
            write_frame(arguments.write_descriptor, ready_from(init, 2));
            wait_forever();
        }
        if (arguments.mode == "binding-mismatch") {
            GameIpcMessage ready = ready_from(init);
            ready.payload["mode_id"] = "wrong-mode";
            write_frame(arguments.write_descriptor, ready);
            wait_forever();
        }
        if (arguments.mode == "close-output") {
            ::close(arguments.write_descriptor);
            wait_forever();
        }
        if (arguments.mode == "no-ready") {
            wait_forever();
        }

        write_frame(arguments.write_descriptor, ready_from(init));
        if (arguments.mode == "duplicate") {
            GameIpcMessage output;
            output.type = "GameOutput";
            output.game_id = init.game_id;
            output.sequence = 1;
            output.payload = {
                {"recipients", boost::json::array{"player-a"}},
                {"output_type", "notice"},
                {"data", boost::json::object{}},
            };
            write_frame(arguments.write_descriptor, output);
            wait_forever();
        }
        if (arguments.mode == "close-input") {
            ::close(arguments.read_descriptor);
            wait_forever();
        }
        if (arguments.mode == "exit-after-ready") {
            return 0;
        }
        if (arguments.mode == "ready-then-stall") {
            wait_forever();
        }
        if (arguments.mode == "ignore-shutdown") {
            std::signal(SIGTERM, SIG_IGN);
            static_cast<void>(read_body(arguments.read_descriptor));
            wait_forever();
        }

        const GameIpcMessage shutdown = GameIpcCodec::decode_body(
            read_body(arguments.read_descriptor),
            GameIpcDirection::parent_to_child);
        if (shutdown.type != "Shutdown" || shutdown.sequence != 2) return 33;
        return 0;
    } catch (...) {
        return 100;
    }
}
