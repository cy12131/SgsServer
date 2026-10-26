#include "cardgame/server/game/game_ipc.h"

#include <boost/json/array.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace cardgame::server::game;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error{std::string{message}};
}

template <typename Function>
void expect_error(std::string_view code, Function&& function) {
    try {
        function();
    } catch (const GameIpcError& error) {
        require(error.code == code,
                "unexpected GameIPC error code: " + error.code);
        return;
    }
    throw std::runtime_error{"expected GameIpcError " + std::string{code}};
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error{"cannot read vector " + path.string()};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

boost::json::array packages() {
    return {boost::json::object{
        {"id", "org.sgs.base"},
        {"commit", std::string(40, '1')},
    }};
}

boost::json::array players() {
    return {
        boost::json::object{{"player_id", "player-a"}, {"seat", 1}, {"display_name", "甲"}},
        boost::json::object{{"player_id", "player-b"}, {"seat", 2}, {"display_name", "乙"}},
    };
}

GameIpcMessage message(
    std::string type,
    std::uint64_t sequence,
    boost::json::object payload,
    std::optional<std::string> token = std::nullopt) {
    return {std::move(type), "game-test-0001", sequence,
            std::move(token), std::move(payload)};
}

std::vector<std::pair<GameIpcMessage, GameIpcDirection>> all_messages() {
    const std::string token = "request_00000001";
    return {
        {message("InitGame", 1, {
            {"room_id", "7"}, {"manifest_id", std::string(64, 'a')},
            {"mode_id", "standard"}, {"seed", "42"}, {"players", players()},
            {"packages", packages()}, {"settings", boost::json::object{}}}, token),
         GameIpcDirection::parent_to_child},
        {message("GameReady", 1, {
            {"mode_id", "standard"}, {"runtime_api_version", "1.0-draft"},
            {"loaded_packages", packages()}}, token),
         GameIpcDirection::child_to_parent},
        {message("PlayerAction", 2, {
            {"player_id", "player-a"}, {"action_type", "play"},
            {"data", boost::json::array{1, 2}}}),
         GameIpcDirection::parent_to_child},
        {message("PlayerRequest", 2, {
            {"recipients", boost::json::array{"player-b"}},
            {"request_type", "choose"}, {"deadline_ms", 1000},
            {"reply_mode", "all"}, {"required_replies", 1},
            {"cancel_policy", "use_default"},
            {"disconnect_policy", "use_default"},
            {"public_data", boost::json::object{}},
            {"private_data", boost::json::object{{"player-b", boost::json::object{}}}},
            {"default_replies", boost::json::object{{"player-b", boost::json::object{}}}}}, token),
         GameIpcDirection::child_to_parent},
        {message("PlayerReply", 3, {
            {"player_id", "player-b"}, {"resolution", "reply"},
            {"data", nullptr}}, token),
         GameIpcDirection::parent_to_child},
        {message("GameOutput", 3, {
            {"recipients", boost::json::array{"player-a"}},
            {"output_type", "view_snapshot"},
            {"data", boost::json::object{{"revision", "1"}, {"state", boost::json::object{}}}}}),
         GameIpcDirection::child_to_parent},
        {message("GameFinished", 4, {{"result", boost::json::object{}}}),
         GameIpcDirection::child_to_parent},
        {message("GameFailed", 4, {{"error", boost::json::object{
            {"code", "RULE_FAILED"}, {"message", "safe failure"},
            {"retryable", false}}}}),
         GameIpcDirection::child_to_parent},
        {message("Shutdown", 4, {{"reason", "server shutdown"}}),
         GameIpcDirection::parent_to_child},
    };
}

void test_architecture_vectors() {
    const std::filesystem::path root{GAME_IPC_VECTOR_DIR};
    const GameIpcMessage init = GameIpcCodec::decode_body(
        read_file(root / "init-game.json"), GameIpcDirection::parent_to_child);
    require(init.type == "InitGame" && init.sequence == 1 &&
                init.payload.at("room_id").as_string() == "1",
            "InitGame architecture vector mismatch");
    const GameIpcMessage ready = GameIpcCodec::decode_body(
        read_file(root / "game-ready.json"), GameIpcDirection::child_to_parent);
    require(ready.type == "GameReady" && ready.request_token == init.request_token,
            "GameReady architecture vector mismatch");
}

void test_all_message_types() {
    for (const auto& [source, direction] : all_messages()) {
        const std::string body = GameIpcCodec::encode_body(source, direction);
        const GameIpcMessage decoded = GameIpcCodec::decode_body(body, direction);
        require(decoded.type == source.type && decoded.game_id == source.game_id &&
                    decoded.sequence == source.sequence &&
                    decoded.request_token == source.request_token,
                "GameIPC round trip mismatch");
        const GameIpcDirection opposite =
            direction == GameIpcDirection::parent_to_child
                ? GameIpcDirection::child_to_parent
                : GameIpcDirection::parent_to_child;
        expect_error("IPC_DIRECTION_INVALID", [&] {
            static_cast<void>(GameIpcCodec::decode_body(body, opposite));
        });
    }
}

void test_rejections() {
    expect_error("IPC_FRAME_INVALID", [] {
        static_cast<void>(GameIpcCodec::decode_body(""));
    });
    expect_error("IPC_JSON_INVALID", [] {
        static_cast<void>(GameIpcCodec::decode_body("{not-json"));
    });
    expect_error("IPC_JSON_INVALID", [] {
        static_cast<void>(GameIpcCodec::decode_body(
            "{\"ipc_version\":\"1\",\"ipc_version\":\"1\",\"type\":\"Shutdown\",\"game_id\":\"game-test-0001\",\"sequence\":\"1\",\"payload\":{\"reason\":\"x\"}}"));
    });
    std::string invalid_utf8 =
        "{\"ipc_version\":\"1\",\"type\":\"Shutdown\",\"game_id\":\"game-test-0001\",\"sequence\":\"1\",\"payload\":{\"reason\":\"";
    invalid_utf8.push_back(static_cast<char>(0xFF));
    invalid_utf8 += "\"}}";
    expect_error("IPC_JSON_INVALID", [&] {
        static_cast<void>(GameIpcCodec::decode_body(invalid_utf8));
    });
    expect_error("IPC_MESSAGE_TYPE_INVALID", [] {
        static_cast<void>(GameIpcCodec::decode_body(
            "{\"ipc_version\":\"1\",\"type\":\"TenthMessage\",\"game_id\":\"game-test-0001\",\"sequence\":\"1\",\"payload\":{}}"));
    });
    expect_error("IPC_SCHEMA_INVALID", [] {
        static_cast<void>(GameIpcCodec::decode_body(
            "{\"ipc_version\":\"1\",\"type\":\"Shutdown\",\"game_id\":\"game-test-0001\",\"sequence\":\"1\",\"payload\":{\"reason\":\"x\"},\"extra\":true}"));
    });
    expect_error("IPC_SCHEMA_INVALID", [] {
        static_cast<void>(GameIpcCodec::decode_body(
            "{\"ipc_version\":\"1\",\"type\":\"Shutdown\",\"game_id\":\"game-test-0001\",\"sequence\":\"18446744073709551616\",\"payload\":{\"reason\":\"x\"}}"));
    });
    expect_error("IPC_FRAME_INVALID", [] {
        std::string oversized(game_ipc_maximum_body_size + 1, 'x');
        static_cast<void>(GameIpcCodec::decode_body(oversized));
    });
    expect_error("IPC_FRAME_INVALID", [] {
        GameIpcMessage oversized = message("PlayerAction", 1, {
            {"player_id", "player-a"}, {"action_type", "bulk"},
            {"data", std::string(game_ipc_maximum_body_size, 'x')},
        });
        static_cast<void>(GameIpcCodec::encode_frame(
            oversized, GameIpcDirection::parent_to_child));
    });
}

void test_framing() {
    const auto values = all_messages();
    const auto first = GameIpcCodec::encode_frame(values.front().first, values.front().second);
    const auto last = GameIpcCodec::encode_frame(values.back().first, values.back().second);

    GameIpcFrameDecoder fragmented;
    std::vector<std::string> bodies;
    for (const std::uint8_t byte : first) {
        auto next = fragmented.feed(std::span<const std::uint8_t>{&byte, 1});
        bodies.insert(bodies.end(), next.begin(), next.end());
    }
    require(bodies.size() == 1 &&
                GameIpcCodec::decode_body(bodies.front()).type == "InitGame",
            "partial frame was not reassembled");

    std::vector<std::uint8_t> combined = first;
    combined.insert(combined.end(), last.begin(), last.end());
    GameIpcFrameDecoder sticky;
    const auto joined = sticky.feed(combined);
    require(joined.size() == 2, "multiple frames were not split");

    for (const std::array<std::uint8_t, 4> header : {
             std::array<std::uint8_t, 4>{0, 0, 0, 0},
             std::array<std::uint8_t, 4>{0, 16, 0, 1}}) {
        GameIpcFrameDecoder invalid;
        expect_error("IPC_FRAME_INVALID", [&] {
            static_cast<void>(invalid.feed(header));
        });
    }
}

void test_sequences() {
    GameIpcSequenceValidator parent_to_child;
    GameIpcSequenceValidator child_to_parent;
    parent_to_child.accept(1);
    child_to_parent.accept(1);
    parent_to_child.accept(2);
    child_to_parent.accept(2);
    expect_error("IPC_SEQUENCE_INVALID", [&] { parent_to_child.accept(2); });

    GameIpcSequenceValidator gap;
    expect_error("IPC_SEQUENCE_INVALID", [&] { gap.accept(2); });

    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    GameIpcSequenceValidator boundary{maximum};
    boundary.accept(maximum);
    expect_error("IPC_SEQUENCE_INVALID", [&] { boundary.accept(maximum); });
    GameIpcSequenceGenerator generator{maximum};
    require(generator.take() == maximum, "maximum sequence was not emitted");
    expect_error("IPC_SEQUENCE_INVALID", [&] { static_cast<void>(generator.take()); });
}

}  // namespace

int main() {
    try {
        test_architecture_vectors();
        test_all_message_types();
        test_rejections();
        test_framing();
        test_sequences();
        std::cout << "GameIPC codec tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "GameIPC codec test failed: " << error.what() << '\n';
        return 1;
    }
}
