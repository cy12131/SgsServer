#include "cardgame/server/game/game_ipc.h"
#include "lua/lua54_abi.h"

#include <boost/json/parse.hpp>

#include <array>
#include <cerrno>
#include <deque>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <poll.h>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

using cardgame::server::game::GameIpcCodec;
using cardgame::server::game::GameIpcDirection;
using cardgame::server::game::GameIpcFrameDecoder;
using cardgame::server::game::GameIpcMessage;
using cardgame::server::game::GameIpcSequenceGenerator;
using cardgame::server::game::GameIpcSequenceValidator;

struct PackageBinding {
    std::string id;
    std::string version;
    std::string commit;
    std::filesystem::path root;
    std::optional<std::filesystem::path> server_entry;
};

struct PlayerBinding {
    std::string player_id;
    std::uint8_t seat{};
    std::string display_name;
};

struct Arguments {
    std::string game_id;
    std::string room_id;
    std::string manifest_id;
    std::string mode_id;
    int read_descriptor = -1;
    int write_descriptor = -1;
    std::vector<PackageBinding> packages;
    std::vector<PlayerBinding> players;
};

struct ModeRegistration {
    int function_reference = -1;
    int minimum_players = 2;
    int maximum_players = 8;
};

struct RuntimeContext {
    Arguments arguments;
    const PackageBinding* loading_package = nullptr;
    std::map<std::string, ModeRegistration> modes;
    std::set<std::string> registered_packages;
    boost::json::object settings;
};

[[noreturn]] void fail(std::string_view message) {
    throw std::runtime_error{std::string{message}};
}

const boost::json::object& parse_object(std::string_view text) {
    // The returned reference is only used by callers that immediately copy fields;
    // keep the parsed value in thread-local storage for the duration of that call.
    thread_local boost::json::value value;
    value = boost::json::parse(text);
    if (!value.is_object()) fail("runtime binding argument must be an object");
    return value.as_object();
}

std::string json_string(const boost::json::object& object, std::string_view key) {
    const auto* value = object.if_contains(key);
    if (value == nullptr || !value->is_string()) fail("runtime binding field is invalid");
    return std::string{value->as_string()};
}

PackageBinding parse_package(std::string_view text) {
    const auto& object = parse_object(text);
    if (object.size() != 5 || !object.contains("id") || !object.contains("version") ||
        !object.contains("commit") || !object.contains("root") ||
        !object.contains("server_entry")) {
        fail("runtime package binding fields are invalid");
    }
    PackageBinding package{json_string(object, "id"), json_string(object, "version"),
                           json_string(object, "commit"), json_string(object, "root"),
                           std::nullopt};
    const auto& entry = object.at("server_entry");
    if (!entry.is_null()) {
        if (!entry.is_string()) fail("runtime package entry is invalid");
        package.server_entry = std::filesystem::path{std::string{entry.as_string()}};
    }
    return package;
}

PlayerBinding parse_player(std::string_view text) {
    const auto& object = parse_object(text);
    if (object.size() != 3 || !object.contains("player_id") ||
        !object.contains("seat") || !object.contains("display_name") ||
        !object.at("seat").is_int64()) {
        fail("runtime player binding fields are invalid");
    }
    const auto seat = object.at("seat").as_int64();
    if (seat < 1 || seat > 8) fail("runtime player seat is invalid");
    return {json_string(object, "player_id"), static_cast<std::uint8_t>(seat),
            json_string(object, "display_name")};
}

Arguments parse_arguments(int count, char** values) {
    Arguments result;
    for (int index = 1; index < count; index += 2) {
        if (index + 1 >= count) fail("runtime argument value is missing");
        const std::string_view name{values[index]};
        const std::string value{values[index + 1]};
        if (name == "--game-id") result.game_id = value;
        else if (name == "--room-id") result.room_id = value;
        else if (name == "--manifest-id") result.manifest_id = value;
        else if (name == "--mode-id") result.mode_id = value;
        else if (name == "--ipc-read") result.read_descriptor = std::stoi(value);
        else if (name == "--ipc-write") result.write_descriptor = std::stoi(value);
        else if (name == "--package") result.packages.push_back(parse_package(value));
        else if (name == "--player") result.players.push_back(parse_player(value));
        else fail("unknown runtime argument");
    }
    if (result.game_id.empty() || result.room_id.empty() || result.manifest_id.empty() ||
        result.mode_id.empty() || result.read_descriptor < 0 ||
        result.write_descriptor < 0 || result.packages.empty() ||
        result.players.size() < 2 || result.players.size() > 8) {
        fail("runtime launch context is incomplete");
    }
    return result;
}

bool path_is_within(const std::filesystem::path& root,
                    const std::filesystem::path& candidate) {
    auto left = root.begin();
    auto right = candidate.begin();
    for (; left != root.end(); ++left, ++right) {
        if (right == candidate.end() || *left != *right) return false;
    }
    return true;
}

void validate_package_paths(Arguments& arguments) {
    for (auto& package : arguments.packages) {
        if (!package.root.is_absolute() ||
            std::filesystem::is_symlink(std::filesystem::symlink_status(package.root))) {
            fail("package root must be an absolute physical directory");
        }
        package.root = std::filesystem::canonical(package.root);
        if (!std::filesystem::is_directory(package.root)) fail("package root is not a directory");
        if (!package.server_entry) continue;
        if (package.server_entry->is_absolute() ||
            package.server_entry->lexically_normal() != *package.server_entry) {
            fail("package entry must be a normalized relative path");
        }
        const auto declared = package.root / *package.server_entry;
        if (std::filesystem::is_symlink(std::filesystem::symlink_status(declared))) {
            fail("package entry cannot be a symbolic link");
        }
        const auto entry = std::filesystem::canonical(declared);
        if (!path_is_within(package.root, entry) || !std::filesystem::is_regular_file(entry)) {
            fail("package entry escapes its owner root");
        }
    }
}

void wait_descriptor(int descriptor, short events) {
    pollfd item{descriptor, events, 0};
    while (true) {
        const int result = ::poll(&item, 1, -1);
        if (result > 0) return;
        if (result < 0 && errno == EINTR) continue;
        fail("runtime IPC poll failed");
    }
}

GameIpcMessage read_message(
    int descriptor,
    GameIpcFrameDecoder& decoder,
    std::deque<std::string>& pending_bodies) {
    std::array<std::uint8_t, 4096> bytes{};
    while (true) {
        if (!pending_bodies.empty()) {
            std::string body = std::move(pending_bodies.front());
            pending_bodies.pop_front();
            return GameIpcCodec::decode_body(body, GameIpcDirection::parent_to_child);
        }
        wait_descriptor(descriptor, POLLIN);
        const ssize_t count = ::read(descriptor, bytes.data(), bytes.size());
        if (count == 0) fail("runtime IPC closed");
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            fail("runtime IPC read failed");
        }
        auto bodies = decoder.feed({bytes.data(), static_cast<std::size_t>(count)});
        for (std::string& body : bodies) {
            pending_bodies.push_back(std::move(body));
        }
    }
}

void write_message(int descriptor, const GameIpcMessage& message) {
    const auto frame = GameIpcCodec::encode_frame(message, GameIpcDirection::child_to_parent);
    std::size_t offset = 0;
    while (offset < frame.size()) {
        wait_descriptor(descriptor, POLLOUT);
        const ssize_t count = ::write(descriptor, frame.data() + offset, frame.size() - offset);
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            fail("runtime IPC write failed");
        }
        offset += static_cast<std::size_t>(count);
    }
}

std::string lua_string(lua_State* state, int table, const char* field) {
    lua_getfield(state, table, field);
    if (lua_type(state, -1) != lua_type_string) fail("Lua registration string is invalid");
    std::size_t size = 0;
    const char* value = lua_tolstring(state, -1, &size);
    std::string result{value, size};
    lua_pop(state, 1);
    return result;
}

int lua_integer(lua_State* state, int table, const char* field) {
    lua_getfield(state, table, field);
    int is_number = 0;
    const auto value = lua_tointegerx(state, -1, &is_number);
    lua_pop(state, 1);
    if (!is_number) fail("Lua registration integer is invalid");
    return static_cast<int>(value);
}

RuntimeContext* context_from_upvalue(lua_State* state) {
    return static_cast<RuntimeContext*>(lua_touserdata(state, lua_registry_index - 1));
}

int lua_register_mode(lua_State* state) {
    try {
        RuntimeContext* context = context_from_upvalue(state);
        // registerGameMode is a package method: pkg:registerGameMode{...}.
        if (context == nullptr || lua_type(state, 1) != lua_type_table ||
            lua_type(state, 2) != lua_type_table) {
            fail("registerGameMode requires a table");
        }
        const std::string id = lua_string(state, 2, "id");
        const int minimum = lua_integer(state, 2, "min_players");
        const int maximum = lua_integer(state, 2, "max_players");
        if (minimum < 2 || maximum > 8 || minimum > maximum || context->modes.contains(id)) {
            fail("game mode limits or identity are invalid");
        }
        lua_getfield(state, 2, "create_logic");
        if (lua_type(state, -1) != lua_type_function) fail("create_logic must be a function");
        const int reference = luaL_ref(state, lua_registry_index);
        context->modes.emplace(id, ModeRegistration{reference, minimum, maximum});
        return 0;
    } catch (const std::exception& exception) {
        lua_pushstring(state, exception.what());
        return lua_error(state);
    }
}

int lua_register_package(lua_State* state) {
    try {
        RuntimeContext* context = context_from_upvalue(state);
        if (context == nullptr || context->loading_package == nullptr ||
            lua_type(state, 1) != lua_type_table) {
            fail("registerPackage called outside package entry");
        }
        if (lua_string(state, 1, "id") != context->loading_package->id ||
            lua_string(state, 1, "version") != context->loading_package->version) {
            fail("Lua package identity does not match generation");
        }
        if (!context->registered_packages.insert(context->loading_package->id).second) {
            fail("Lua package identity was registered more than once");
        }
        lua_createtable(state, 0, 1);
        lua_pushlightuserdata(state, context);
        lua_pushcclosure(state, lua_register_mode, 1);
        lua_setfield(state, -2, "registerGameMode");
        return 1;
    } catch (const std::exception& exception) {
        lua_pushstring(state, exception.what());
        return lua_error(state);
    }
}

std::string lua_error_text(lua_State* state) {
    std::size_t size = 0;
    const char* value = lua_tolstring(state, -1, &size);
    return value == nullptr ? "Lua runtime error" : std::string{value, size};
}

void push_json(lua_State* state, const boost::json::value& value) {
    if (value.is_null()) lua_pushnil(state);
    else if (value.is_bool()) lua_pushboolean(state, value.as_bool());
    else if (value.is_int64()) lua_pushinteger(state, value.as_int64());
    else if (value.is_uint64()) lua_pushinteger(state, static_cast<lua_Integer>(value.as_uint64()));
    else if (value.is_double()) lua_pushnumber(state, value.as_double());
    else if (value.is_string()) {
        const auto& text = value.as_string();
        lua_pushlstring(state, text.data(), text.size());
    } else if (value.is_array()) {
        const auto& array = value.as_array();
        lua_createtable(state, static_cast<int>(array.size()), 0);
        for (std::size_t index = 0; index < array.size(); ++index) {
            push_json(state, array[index]);
            lua_rawseti(state, -2, static_cast<lua_Integer>(index + 1));
        }
    } else {
        const auto& object = value.as_object();
        lua_createtable(state, 0, static_cast<int>(object.size()));
        for (const auto& field : object) {
            push_json(state, field.value());
            lua_setfield(state, -2, std::string{field.key()}.c_str());
        }
    }
}

void load_packages(RuntimeContext& context, lua_State* state) {
    if (state == nullptr) fail("cannot create Lua state");
    luaL_requiref(state, "_G", luaopen_base, 1);
    lua_pop(state, 1);
    // 规则集合和有序玩家数据需要纯内存 table 库；不开放 package、io、
    // os、debug，也不提供任何动态库或文件加载入口。
    luaL_requiref(state, "table", luaopen_table, 1);
    lua_pop(state, 1);
    lua_pushnil(state); lua_setglobal(state, "dofile");
    lua_pushnil(state); lua_setglobal(state, "loadfile");

    lua_createtable(state, 0, 1);
    lua_pushlightuserdata(state, &context);
    lua_pushcclosure(state, lua_register_package, 1);
    lua_setfield(state, -2, "registerPackage");
    lua_setglobal(state, "Runtime");

    for (const auto& package : context.arguments.packages) {
        if (!package.server_entry) continue;
        context.loading_package = &package;
        const auto entry = package.root / *package.server_entry;
        if (luaL_loadfilex(state, entry.c_str(), "t") != lua_ok ||
            lua_pcallk(state, 0, 0, 0, 0, nullptr) != lua_ok) {
            fail(lua_error_text(state));
        }
        if (!context.registered_packages.contains(package.id)) {
            fail("Lua package entry did not register its generation identity");
        }
    }
    context.loading_package = nullptr;
    const auto mode = context.modes.find(context.arguments.mode_id);
    if (mode == context.modes.end()) fail("requested game mode was not registered");
    const int players = static_cast<int>(context.arguments.players.size());
    if (players < mode->second.minimum_players || players > mode->second.maximum_players) {
        fail("roster is outside registered game mode capacity");
    }

    lua_rawgeti(state, lua_registry_index, mode->second.function_reference);
    lua_createtable(state, 0, 6);
    lua_pushlstring(state, context.arguments.game_id.data(), context.arguments.game_id.size());
    lua_setfield(state, -2, "game_id");
    lua_pushlstring(state, context.arguments.room_id.data(), context.arguments.room_id.size());
    lua_setfield(state, -2, "room_id");
    lua_pushlstring(state, context.arguments.mode_id.data(), context.arguments.mode_id.size());
    lua_setfield(state, -2, "mode_id");
    lua_pushinteger(state, players); lua_setfield(state, -2, "player_count");
    lua_createtable(state, players, 0);
    for (std::size_t index = 0; index < context.arguments.players.size(); ++index) {
        const auto& player = context.arguments.players[index];
        lua_createtable(state, 0, 3);
        lua_pushlstring(state, player.player_id.data(), player.player_id.size());
        lua_setfield(state, -2, "id");
        lua_pushinteger(state, player.seat); lua_setfield(state, -2, "seat");
        lua_pushlstring(state, player.display_name.data(), player.display_name.size());
        lua_setfield(state, -2, "display_name");
        lua_rawseti(state, -2, static_cast<lua_Integer>(index + 1));
    }
    lua_setfield(state, -2, "players");
    push_json(state, context.settings);
    lua_setfield(state, -2, "settings");
    if (lua_pcallk(state, 1, 0, 0, 0, nullptr) != lua_ok) {
        fail(lua_error_text(state));
    }
}

bool same_init_context(const Arguments& arguments, const GameIpcMessage& init) {
    if (init.type != "InitGame" || init.game_id != arguments.game_id) return false;
    const auto& payload = init.payload;
    if (payload.at("room_id").as_string() != arguments.room_id ||
        payload.at("manifest_id").as_string() != arguments.manifest_id ||
        payload.at("mode_id").as_string() != arguments.mode_id) return false;
    const auto& packages = payload.at("packages").as_array();
    const auto& players = payload.at("players").as_array();
    if (packages.size() != arguments.packages.size() ||
        players.size() != arguments.players.size()) return false;
    for (std::size_t i = 0; i < packages.size(); ++i) {
        const auto& item = packages[i].as_object();
        if (item.at("id").as_string() != arguments.packages[i].id ||
            item.at("commit").as_string() != arguments.packages[i].commit) return false;
    }
    for (std::size_t i = 0; i < players.size(); ++i) {
        const auto& item = players[i].as_object();
        if (item.at("player_id").as_string() != arguments.players[i].player_id ||
            item.at("seat").as_int64() != arguments.players[i].seat ||
            item.at("display_name").as_string() != arguments.players[i].display_name) return false;
    }
    return true;
}

boost::json::array loaded_packages(const Arguments& arguments) {
    boost::json::array result;
    for (const auto& package : arguments.packages) {
        result.emplace_back(boost::json::object{{"id", package.id}, {"commit", package.commit}});
    }
    return result;
}

}  // namespace

int main(int argc, char** argv) {
    std::optional<Arguments> arguments;
    std::uint64_t failure_sequence = 1;
    try {
        arguments = parse_arguments(argc, argv);
        validate_package_paths(*arguments);
        GameIpcFrameDecoder decoder;
        std::deque<std::string> pending_bodies;
        GameIpcSequenceValidator incoming_sequence;
        GameIpcSequenceGenerator outgoing_sequence;
        GameIpcMessage init = read_message(
            arguments->read_descriptor, decoder, pending_bodies);
        incoming_sequence.accept(init.sequence);
        if (!same_init_context(*arguments, init)) fail("launch context and InitGame differ");

        RuntimeContext context{*arguments, nullptr, {}, {}, init.payload.at("settings").as_object()};
        std::unique_ptr<lua_State, decltype(&lua_close)> lua{luaL_newstate(), lua_close};
        load_packages(context, lua.get());
        GameIpcMessage ready{"GameReady", arguments->game_id,
            outgoing_sequence.take(), init.request_token,
            {{"mode_id", arguments->mode_id},
             {"runtime_api_version", "1.0-draft"},
             {"loaded_packages", loaded_packages(*arguments)}}};
        write_message(arguments->write_descriptor, ready);
        failure_sequence = 2;

        GameIpcMessage shutdown = read_message(
            arguments->read_descriptor, decoder, pending_bodies);
        incoming_sequence.accept(shutdown.sequence);
        if (shutdown.type != "Shutdown" || shutdown.game_id != arguments->game_id ||
            shutdown.sequence != 2) {
            fail("expected ordered Shutdown");
        }
        return 0;
    } catch (const std::exception& exception) {
        // 错误只输出固定摘要；GameIPC 错误消息不包含 Lua 原文或游戏 payload。
        std::cerr << "Lua Runtime initialization failed\n";
        if (arguments && arguments->write_descriptor >= 0 && !arguments->game_id.empty()) {
            try {
                GameIpcMessage failed{"GameFailed", arguments->game_id,
                    failure_sequence, std::nullopt,
                    {{"error", {{"code", "LUA_RUNTIME_INIT_FAILED"},
                                {"message", "Lua Runtime initialization failed"},
                                {"retryable", false}}}}};
                write_message(arguments->write_descriptor, failed);
            } catch (...) {
            }
        }
        static_cast<void>(exception);
        return 1;
    }
}
