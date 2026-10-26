#pragma once

#include <boost/json/object.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace cardgame::server::game {

inline constexpr std::size_t game_ipc_maximum_body_size = 1'048'576;

enum class GameIpcDirection {
    parent_to_child,
    child_to_parent,
};

struct GameIpcMessage {
    std::string type;
    std::string game_id;
    std::uint64_t sequence = 0;
    std::optional<std::string> request_token;
    boost::json::object payload;
};

// GameIPC 错误码用于进程监管和测试诊断，不会直接透传给客户端。
// 不可信子进程的原始 payload 也不会拼入错误文本，避免泄露隐藏信息。
class GameIpcError final : public std::runtime_error {
public:
    GameIpcError(std::string code, std::string message);

    std::string code;
};

class GameIpcCodec final {
public:
    [[nodiscard]] static GameIpcMessage decode_body(
        std::string_view body,
        GameIpcDirection direction);
    [[nodiscard]] static std::string encode_body(
        const GameIpcMessage& message,
        GameIpcDirection direction);
    [[nodiscard]] static std::vector<std::uint8_t> encode_frame(
        const GameIpcMessage& message,
        GameIpcDirection direction);
};

// 增量帧解析器只负责 GameIPC 的 1 MiB 大端长度帧边界。
// JSON/Schema 验证由 GameIpcCodec 完成，便于在 partial read 和粘包下复用。
class GameIpcFrameDecoder final {
public:
    [[nodiscard]] std::vector<std::string> feed(
        std::span<const std::uint8_t> bytes);
    void reset() noexcept;
    [[nodiscard]] std::size_t buffered_bytes() const noexcept;

private:
    std::vector<std::uint8_t> buffer_;
};

// 每个发送方向拥有独立实例。达到 UINT64_MAX 后实例耗尽，禁止回绕。
class GameIpcSequenceGenerator final {
public:
    explicit GameIpcSequenceGenerator(std::uint64_t first = 1);
    [[nodiscard]] std::uint64_t take();

private:
    std::uint64_t next_;
    bool exhausted_ = false;
};

class GameIpcSequenceValidator final {
public:
    explicit GameIpcSequenceValidator(std::uint64_t first = 1);
    void accept(std::uint64_t sequence);
    [[nodiscard]] std::uint64_t expected() const noexcept;

private:
    std::uint64_t next_;
    bool exhausted_ = false;
};

}  // namespace cardgame::server::game
