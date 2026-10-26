#include "cardgame/server/game/game_ipc.h"

#include <boost/json/basic_parser_impl.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>

#include <algorithm>
#include <charconv>
#include <limits>
#include <set>
#include <unordered_set>
#include <utility>

namespace cardgame::server::game {
namespace {

[[noreturn]] void fail(std::string code, std::string message) {
    throw GameIpcError{std::move(code), std::move(message)};
}

struct DuplicateKeyHandler {
    static constexpr std::size_t max_array_size = game_ipc_maximum_body_size;
    static constexpr std::size_t max_object_size = game_ipc_maximum_body_size;
    static constexpr std::size_t max_string_size = game_ipc_maximum_body_size;
    static constexpr std::size_t max_key_size = game_ipc_maximum_body_size;

    bool duplicate = false;
    std::string key;
    std::vector<std::unordered_set<std::string>> object_keys;

    bool on_document_begin(boost::system::error_code&) { return true; }
    bool on_document_end(boost::system::error_code&) { return true; }
    bool on_array_begin(boost::system::error_code&) { return true; }
    bool on_array_end(std::size_t, boost::system::error_code&) { return true; }
    bool on_object_begin(boost::system::error_code&) {
        object_keys.emplace_back();
        return true;
    }
    bool on_object_end(std::size_t, boost::system::error_code&) {
        object_keys.pop_back();
        return true;
    }
    bool on_string_part(
        boost::json::string_view, std::size_t, boost::system::error_code&) {
        return true;
    }
    bool on_string(
        boost::json::string_view, std::size_t, boost::system::error_code&) {
        return true;
    }
    bool on_key_part(
        boost::json::string_view part,
        std::size_t,
        boost::system::error_code&) {
        key.append(part.data(), part.size());
        return true;
    }
    bool on_key(
        boost::json::string_view part,
        std::size_t,
        boost::system::error_code& error) {
        key.append(part.data(), part.size());
        if (object_keys.empty() || !object_keys.back().insert(key).second) {
            duplicate = true;
            error = boost::json::error::syntax;
            return false;
        }
        key.clear();
        return true;
    }
    bool on_number_part(boost::json::string_view, boost::system::error_code&) {
        return true;
    }
    bool on_int64(
        std::int64_t, boost::json::string_view, boost::system::error_code&) {
        return true;
    }
    bool on_uint64(
        std::uint64_t, boost::json::string_view, boost::system::error_code&) {
        return true;
    }
    bool on_double(
        double, boost::json::string_view, boost::system::error_code&) {
        return true;
    }
    bool on_bool(bool, boost::system::error_code&) { return true; }
    bool on_null(boost::system::error_code&) { return true; }
    bool on_comment_part(boost::json::string_view, boost::system::error_code&) {
        return true;
    }
    bool on_comment(boost::json::string_view, boost::system::error_code&) {
        return true;
    }
};

void reject_duplicate_keys(std::string_view body) {
    boost::json::basic_parser<DuplicateKeyHandler> parser{
        boost::json::parse_options{}};
    boost::system::error_code error;
    const std::size_t consumed = parser.write_some(
        false, body.data(), body.size(), error);
    if (parser.handler().duplicate) {
        fail("IPC_JSON_INVALID", "duplicate JSON object key");
    }
    if (error || consumed != body.size() || !parser.done()) {
        fail("IPC_JSON_INVALID", "invalid UTF-8 JSON body");
    }
}

bool contains(
    std::initializer_list<std::string_view> fields,
    std::string_view candidate) {
    return std::find(fields.begin(), fields.end(), candidate) != fields.end();
}

void require_exact_fields(
    const boost::json::object& object,
    std::initializer_list<std::string_view> fields,
    std::string_view context) {
    if (object.size() != fields.size()) {
        fail("IPC_SCHEMA_INVALID", std::string{context} + " has unexpected fields");
    }
    for (const auto& entry : object) {
        if (!contains(fields, entry.key())) {
            fail("IPC_SCHEMA_INVALID", std::string{context} + " has an unknown field");
        }
    }
    for (const std::string_view field : fields) {
        if (!object.contains(field)) {
            fail("IPC_SCHEMA_INVALID", std::string{context} + " is missing a field");
        }
    }
}

const boost::json::value& field(
    const boost::json::object& object,
    std::string_view name,
    std::string_view context) {
    const boost::json::value* value = object.if_contains(name);
    if (value == nullptr) {
        fail("IPC_SCHEMA_INVALID", std::string{context} + " is missing a field");
    }
    return *value;
}

std::string_view require_string(
    const boost::json::object& object,
    std::string_view name,
    std::string_view context) {
    const boost::json::value& value = field(object, name, context);
    if (!value.is_string()) {
        fail("IPC_SCHEMA_INVALID", std::string{context} + " field must be a string");
    }
    return value.as_string();
}

const boost::json::object& require_object(
    const boost::json::object& object,
    std::string_view name,
    std::string_view context) {
    const boost::json::value& value = field(object, name, context);
    if (!value.is_object()) {
        fail("IPC_SCHEMA_INVALID", std::string{context} + " field must be an object");
    }
    return value.as_object();
}

const boost::json::array& require_array(
    const boost::json::object& object,
    std::string_view name,
    std::string_view context) {
    const boost::json::value& value = field(object, name, context);
    if (!value.is_array()) {
        fail("IPC_SCHEMA_INVALID", std::string{context} + " field must be an array");
    }
    return value.as_array();
}

std::uint64_t require_integer(
    const boost::json::object& object,
    std::string_view name,
    std::uint64_t minimum,
    std::uint64_t maximum,
    std::string_view context) {
    const boost::json::value& value = field(object, name, context);
    std::uint64_t result = 0;
    if (value.is_uint64()) {
        result = value.as_uint64();
    } else if (value.is_int64() && value.as_int64() >= 0) {
        result = static_cast<std::uint64_t>(value.as_int64());
    } else {
        fail("IPC_SCHEMA_INVALID", std::string{context} + " field must be an integer");
    }
    if (result < minimum || result > maximum) {
        fail("IPC_SCHEMA_INVALID", std::string{context} + " integer is out of range");
    }
    return result;
}

std::size_t utf8_length(std::string_view text) {
    std::size_t result = 0;
    for (const unsigned char value : text) {
        if ((value & 0xC0U) != 0x80U) {
            ++result;
        }
    }
    return result;
}

void require_length(
    std::string_view text,
    std::size_t minimum,
    std::size_t maximum,
    std::string_view context) {
    const std::size_t length = utf8_length(text);
    if (length < minimum || length > maximum) {
        fail("IPC_SCHEMA_INVALID", std::string{context} + " string length is invalid");
    }
}

void require_decimal_digits(
    std::string_view text,
    std::size_t minimum,
    std::size_t maximum,
    std::string_view context) {
    if (text.size() < minimum || text.size() > maximum ||
        !std::all_of(text.begin(), text.end(), [](char value) {
            return value >= '0' && value <= '9';
        })) {
        fail("IPC_SCHEMA_INVALID", std::string{context} + " decimal string is invalid");
    }
}

bool is_lower_hex(std::string_view text) {
    return std::all_of(text.begin(), text.end(), [](char value) {
        return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
    });
}

void require_hash(std::string_view text, std::string_view context) {
    if (text.size() != 64 || !is_lower_hex(text)) {
        fail("IPC_SCHEMA_INVALID", std::string{context} + " must be a SHA-256 hex string");
    }
}

void require_commit(std::string_view text, std::string_view context) {
    if ((text.size() != 40 && text.size() != 64) || !is_lower_hex(text)) {
        fail("IPC_SCHEMA_INVALID", std::string{context} + " must be a Git commit hex string");
    }
}

bool is_url_safe(std::string_view text) {
    return std::all_of(text.begin(), text.end(), [](char value) {
        return (value >= 'A' && value <= 'Z') ||
               (value >= 'a' && value <= 'z') ||
               (value >= '0' && value <= '9') || value == '_' || value == '-';
    });
}

void require_game_id(std::string_view text) {
    if (text.size() < 8 || text.size() > 128 || !is_url_safe(text)) {
        fail("IPC_SCHEMA_INVALID", "game_id is invalid");
    }
}

void require_token(std::string_view text) {
    if (text.size() < 16 || text.size() > 128 || !is_url_safe(text)) {
        fail("IPC_SCHEMA_INVALID", "request_token is invalid");
    }
}

void require_player_id(std::string_view text, std::string_view context) {
    if (text.empty() || text.size() > 128 ||
        !std::all_of(text.begin(), text.end(), [](char value) {
            return (value >= 'A' && value <= 'Z') ||
                   (value >= 'a' && value <= 'z') ||
                   (value >= '0' && value <= '9') || value == '_' ||
                   value == '.' || value == ':' || value == '-';
        })) {
        fail("IPC_SCHEMA_INVALID", std::string{context} + " player_id is invalid");
    }
}

std::uint64_t parse_decimal(
    std::string_view text,
    bool allow_zero,
    bool allow_leading_zero,
    std::string_view context) {
    if (text.empty() || text.size() > 20 ||
        !std::all_of(text.begin(), text.end(), [](char value) {
            return value >= '0' && value <= '9';
        }) || (!allow_leading_zero && text.size() > 1 && text.front() == '0')) {
        fail("IPC_SCHEMA_INVALID", std::string{context} + " decimal string is invalid");
    }
    std::uint64_t result = 0;
    const auto conversion = std::from_chars(
        text.data(), text.data() + text.size(), result);
    if (conversion.ec != std::errc{} || conversion.ptr != text.data() + text.size() ||
        (!allow_zero && result == 0)) {
        fail("IPC_SCHEMA_INVALID", std::string{context} + " decimal string is out of range");
    }
    return result;
}

void validate_package_list(const boost::json::array& packages) {
    if (packages.size() > 256) {
        fail("IPC_SCHEMA_INVALID", "package list is too large");
    }
    std::set<std::string> ids;
    for (const boost::json::value& value : packages) {
        if (!value.is_object()) {
            fail("IPC_SCHEMA_INVALID", "package entry must be an object");
        }
        const boost::json::object& package = value.as_object();
        require_exact_fields(package, {"id", "commit"}, "package entry");
        const std::string_view id = require_string(package, "id", "package entry");
        const std::string_view commit = require_string(package, "commit", "package entry");
        require_commit(commit, "package commit");
        if (!ids.emplace(id).second) {
            fail("IPC_SCHEMA_INVALID", "package IDs must be unique");
        }
    }
}

std::vector<std::string> validate_recipients(const boost::json::array& recipients) {
    if (recipients.empty() || recipients.size() > 8) {
        fail("IPC_SCHEMA_INVALID", "recipients count is invalid");
    }
    std::set<std::string> unique;
    std::vector<std::string> result;
    for (const boost::json::value& value : recipients) {
        if (!value.is_string()) {
            fail("IPC_SCHEMA_INVALID", "recipient must be a string");
        }
        const std::string id{value.as_string()};
        require_player_id(id, "recipient");
        if (!unique.insert(id).second) {
            fail("IPC_SCHEMA_INVALID", "recipients must be unique");
        }
        result.push_back(id);
    }
    return result;
}

void require_object_keys_subset(
    const boost::json::object& object,
    const std::vector<std::string>& allowed,
    std::string_view context) {
    for (const auto& entry : object) {
        if (std::find(allowed.begin(), allowed.end(), entry.key()) == allowed.end()) {
            fail("IPC_SCHEMA_INVALID", std::string{context} + " contains a non-recipient key");
        }
    }
}

bool requires_token(std::string_view type) {
    return type == "InitGame" || type == "GameReady" ||
           type == "PlayerRequest" || type == "PlayerReply";
}

bool known_type(std::string_view type) {
    return type == "InitGame" || type == "GameReady" ||
           type == "PlayerAction" || type == "PlayerRequest" ||
           type == "PlayerReply" || type == "GameOutput" ||
           type == "GameFinished" || type == "GameFailed" ||
           type == "Shutdown";
}

void validate_direction(std::string_view type, GameIpcDirection direction) {
    const bool parent_message = type == "InitGame" || type == "PlayerAction" ||
                                type == "PlayerReply" || type == "Shutdown";
    if ((direction == GameIpcDirection::parent_to_child && !parent_message) ||
        (direction == GameIpcDirection::child_to_parent && parent_message)) {
        fail("IPC_DIRECTION_INVALID", "message type is invalid for this IPC direction");
    }
}

void validate_init_game(const boost::json::object& payload) {
    require_exact_fields(payload,
        {"room_id", "manifest_id", "mode_id", "seed", "players", "packages", "settings"},
        "InitGame payload");
    parse_decimal(require_string(payload, "room_id", "InitGame payload"),
                  false, false, "room_id");
    require_hash(require_string(payload, "manifest_id", "InitGame payload"), "manifest_id");
    require_length(require_string(payload, "mode_id", "InitGame payload"), 1, 128, "mode_id");
    // Schema 将 seed 定义为 1～20 位十进制字符串，而不是 uint64。
    // Runtime 后续如何构造确定性 RNG 不应缩窄当前 wire 有效域。
    require_decimal_digits(require_string(payload, "seed", "InitGame payload"),
                           1, 20, "seed");
    const boost::json::array& players = require_array(payload, "players", "InitGame payload");
    if (players.size() < 2 || players.size() > 8) {
        fail("IPC_SCHEMA_INVALID", "InitGame players count is invalid");
    }
    std::set<std::string> ids;
    std::set<std::uint64_t> seats;
    for (const boost::json::value& value : players) {
        if (!value.is_object()) {
            fail("IPC_SCHEMA_INVALID", "InitGame player must be an object");
        }
        const auto& player = value.as_object();
        require_exact_fields(player, {"player_id", "seat", "display_name"}, "InitGame player");
        const std::string id{require_string(player, "player_id", "InitGame player")};
        require_player_id(id, "InitGame player");
        const std::uint64_t seat = require_integer(player, "seat", 1, 8, "InitGame player");
        require_length(require_string(player, "display_name", "InitGame player"),
                       1, 80, "display_name");
        if (!ids.insert(id).second || !seats.insert(seat).second) {
            fail("IPC_SCHEMA_INVALID", "InitGame player IDs and seats must be unique");
        }
    }
    validate_package_list(require_array(payload, "packages", "InitGame payload"));
    static_cast<void>(require_object(payload, "settings", "InitGame payload"));
}

void validate_game_ready(const boost::json::object& payload) {
    require_exact_fields(payload,
        {"mode_id", "runtime_api_version", "loaded_packages"},
        "GameReady payload");
    require_length(require_string(payload, "mode_id", "GameReady payload"), 1, 128, "mode_id");
    if (require_string(payload, "runtime_api_version", "GameReady payload") != "1.0-draft") {
        fail("IPC_SCHEMA_INVALID", "runtime_api_version is unsupported");
    }
    validate_package_list(require_array(payload, "loaded_packages", "GameReady payload"));
}

void validate_player_action(const boost::json::object& payload) {
    require_exact_fields(payload, {"player_id", "action_type", "data"}, "PlayerAction payload");
    require_player_id(require_string(payload, "player_id", "PlayerAction payload"), "PlayerAction");
    require_length(require_string(payload, "action_type", "PlayerAction payload"),
                   1, 128, "action_type");
    static_cast<void>(field(payload, "data", "PlayerAction payload"));
}

void validate_player_request(const boost::json::object& payload) {
    require_exact_fields(payload,
        {"recipients", "request_type", "deadline_ms", "reply_mode", "required_replies",
         "cancel_policy", "disconnect_policy", "public_data", "private_data", "default_replies"},
        "PlayerRequest payload");
    const std::vector<std::string> recipients =
        validate_recipients(require_array(payload, "recipients", "PlayerRequest payload"));
    require_length(require_string(payload, "request_type", "PlayerRequest payload"),
                   1, 128, "request_type");
    static_cast<void>(require_integer(payload, "deadline_ms", 1, 600000, "PlayerRequest payload"));
    const std::string_view reply_mode = require_string(payload, "reply_mode", "PlayerRequest payload");
    if (reply_mode != "all" && reply_mode != "first" && reply_mode != "quorum") {
        fail("IPC_SCHEMA_INVALID", "reply_mode is invalid");
    }
    const std::uint64_t required = require_integer(
        payload, "required_replies", 1, 8, "PlayerRequest payload");
    if (required > recipients.size() ||
        (reply_mode == "all" && required != recipients.size()) ||
        (reply_mode == "first" && required != 1)) {
        fail("IPC_SCHEMA_INVALID", "required_replies is inconsistent with reply_mode");
    }
    const std::string_view cancel = require_string(payload, "cancel_policy", "PlayerRequest payload");
    if (cancel != "allow" && cancel != "use_default" && cancel != "reject") {
        fail("IPC_SCHEMA_INVALID", "cancel_policy is invalid");
    }
    const std::string_view disconnect =
        require_string(payload, "disconnect_policy", "PlayerRequest payload");
    if (disconnect != "use_default" && disconnect != "wait" &&
        disconnect != "cancel_request" && disconnect != "fail_game") {
        fail("IPC_SCHEMA_INVALID", "disconnect_policy is invalid");
    }
    static_cast<void>(require_object(payload, "public_data", "PlayerRequest payload"));
    const auto& private_data = require_object(payload, "private_data", "PlayerRequest payload");
    const auto& defaults = require_object(payload, "default_replies", "PlayerRequest payload");
    require_object_keys_subset(private_data, recipients, "private_data");
    require_object_keys_subset(defaults, recipients, "default_replies");
}

void validate_player_reply(const boost::json::object& payload) {
    require_exact_fields(payload, {"player_id", "resolution", "data"}, "PlayerReply payload");
    require_player_id(require_string(payload, "player_id", "PlayerReply payload"), "PlayerReply");
    const std::string_view resolution = require_string(payload, "resolution", "PlayerReply payload");
    if (resolution != "reply" && resolution != "cancel" && resolution != "timeout" &&
        resolution != "disconnect" && resolution != "shutdown" && resolution != "default") {
        fail("IPC_SCHEMA_INVALID", "PlayerReply resolution is invalid");
    }
    static_cast<void>(field(payload, "data", "PlayerReply payload"));
}

void validate_game_output(const boost::json::object& payload) {
    require_exact_fields(payload, {"recipients", "output_type", "data"}, "GameOutput payload");
    static_cast<void>(validate_recipients(
        require_array(payload, "recipients", "GameOutput payload")));
    const std::string_view output_type = require_string(payload, "output_type", "GameOutput payload");
    require_length(output_type, 1, 128, "output_type");
    const boost::json::value& data = field(payload, "data", "GameOutput payload");
    if (output_type == "view_snapshot" || output_type == "view_delta") {
        if (!data.is_object()) {
            fail("IPC_SCHEMA_INVALID", "view output data must be an object");
        }
        const auto& view = data.as_object();
        if (output_type == "view_snapshot") {
            require_exact_fields(view, {"revision", "state"}, "view_snapshot data");
            parse_decimal(require_string(view, "revision", "view_snapshot data"),
                          false, false, "revision");
        } else {
            require_exact_fields(view, {"base_revision", "revision", "patch"}, "view_delta data");
            parse_decimal(require_string(view, "base_revision", "view_delta data"),
                          false, false, "base_revision");
            parse_decimal(require_string(view, "revision", "view_delta data"),
                          false, false, "revision");
        }
    }
}

void validate_game_failed(const boost::json::object& payload) {
    require_exact_fields(payload, {"error"}, "GameFailed payload");
    const auto& error = require_object(payload, "error", "GameFailed payload");
    if (error.contains("details")) {
        require_exact_fields(error, {"code", "message", "retryable", "details"}, "GameFailed error");
        static_cast<void>(require_object(error, "details", "GameFailed error"));
    } else {
        require_exact_fields(error, {"code", "message", "retryable"}, "GameFailed error");
    }
    const std::string_view code = require_string(error, "code", "GameFailed error");
    if (code.size() < 3 || code.size() > 64 || code.front() < 'A' || code.front() > 'Z' ||
        !std::all_of(code.begin(), code.end(), [](char value) {
            return (value >= 'A' && value <= 'Z') ||
                   (value >= '0' && value <= '9') || value == '_';
        })) {
        fail("IPC_SCHEMA_INVALID", "GameFailed error code is invalid");
    }
    require_length(require_string(error, "message", "GameFailed error"),
                   1, 512, "GameFailed error message");
    if (!field(error, "retryable", "GameFailed error").is_bool()) {
        fail("IPC_SCHEMA_INVALID", "GameFailed retryable must be boolean");
    }
}

void validate_payload(std::string_view type, const boost::json::object& payload) {
    if (type == "InitGame") {
        validate_init_game(payload);
    } else if (type == "GameReady") {
        validate_game_ready(payload);
    } else if (type == "PlayerAction") {
        validate_player_action(payload);
    } else if (type == "PlayerRequest") {
        validate_player_request(payload);
    } else if (type == "PlayerReply") {
        validate_player_reply(payload);
    } else if (type == "GameOutput") {
        validate_game_output(payload);
    } else if (type == "GameFinished") {
        require_exact_fields(payload, {"result"}, "GameFinished payload");
        static_cast<void>(require_object(payload, "result", "GameFinished payload"));
    } else if (type == "GameFailed") {
        validate_game_failed(payload);
    } else if (type == "Shutdown") {
        require_exact_fields(payload, {"reason"}, "Shutdown payload");
        require_length(require_string(payload, "reason", "Shutdown payload"),
                       1, 256, "Shutdown reason");
    }
}

GameIpcMessage validate_root(
    const boost::json::object& object,
    GameIpcDirection direction) {
    const boost::json::value* type_value = object.if_contains("type");
    if (type_value == nullptr || !type_value->is_string()) {
        fail("IPC_SCHEMA_INVALID", "GameIPC type must be a string");
    }
    const std::string type{type_value->as_string()};
    if (!known_type(type)) {
        fail("IPC_MESSAGE_TYPE_INVALID", "unknown GameIPC message type");
    }
    if (requires_token(type)) {
        require_exact_fields(object,
            {"ipc_version", "type", "game_id", "sequence", "request_token", "payload"},
            "GameIPC message");
    } else {
        require_exact_fields(object,
            {"ipc_version", "type", "game_id", "sequence", "payload"},
            "GameIPC message");
    }
    if (require_string(object, "ipc_version", "GameIPC message") != "1") {
        fail("IPC_SCHEMA_INVALID", "ipc_version must be 1");
    }
    const std::string game_id{require_string(object, "game_id", "GameIPC message")};
    require_game_id(game_id);
    const std::uint64_t sequence = parse_decimal(
        require_string(object, "sequence", "GameIPC message"),
        false, false, "sequence");
    std::optional<std::string> token;
    if (requires_token(type)) {
        token = std::string{require_string(object, "request_token", "GameIPC message")};
        require_token(*token);
    }
    const boost::json::object& payload = require_object(object, "payload", "GameIPC message");
    validate_direction(type, direction);
    validate_payload(type, payload);
    return {type, game_id, sequence, std::move(token), payload};
}

}  // namespace

GameIpcError::GameIpcError(std::string error_code, std::string message)
    : std::runtime_error{std::move(message)}, code{std::move(error_code)} {}

GameIpcMessage GameIpcCodec::decode_body(
    std::string_view body,
    GameIpcDirection direction) {
    if (body.empty() || body.size() > game_ipc_maximum_body_size) {
        fail("IPC_FRAME_INVALID", "GameIPC body length is outside protocol limits");
    }
    reject_duplicate_keys(body);
    boost::system::error_code error;
    boost::json::value parsed = boost::json::parse(body, error);
    if (error || !parsed.is_object()) {
        fail("IPC_JSON_INVALID", "GameIPC body must be one UTF-8 JSON object");
    }
    return validate_root(parsed.as_object(), direction);
}

std::string GameIpcCodec::encode_body(
    const GameIpcMessage& message,
    GameIpcDirection direction) {
    boost::json::object object{
        {"ipc_version", "1"},
        {"type", message.type},
        {"game_id", message.game_id},
        {"sequence", std::to_string(message.sequence)},
        {"payload", message.payload},
    };
    if (message.request_token.has_value()) {
        object["request_token"] = *message.request_token;
    }
    const std::string body = boost::json::serialize(object);
    if (body.empty() || body.size() > game_ipc_maximum_body_size) {
        fail("IPC_FRAME_INVALID", "encoded GameIPC body is outside protocol limits");
    }
    static_cast<void>(decode_body(body, direction));
    return body;
}

std::vector<std::uint8_t> GameIpcCodec::encode_frame(
    const GameIpcMessage& message,
    GameIpcDirection direction) {
    const std::string body = encode_body(message, direction);
    const auto size = static_cast<std::uint32_t>(body.size());
    std::vector<std::uint8_t> frame(4 + body.size());
    frame[0] = static_cast<std::uint8_t>((size >> 24U) & 0xFFU);
    frame[1] = static_cast<std::uint8_t>((size >> 16U) & 0xFFU);
    frame[2] = static_cast<std::uint8_t>((size >> 8U) & 0xFFU);
    frame[3] = static_cast<std::uint8_t>(size & 0xFFU);
    std::copy(body.begin(), body.end(), frame.begin() + 4);
    return frame;
}

std::vector<std::string> GameIpcFrameDecoder::feed(
    std::span<const std::uint8_t> bytes) {
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
    std::vector<std::string> result;
    std::size_t consumed = 0;
    while (buffer_.size() - consumed >= 4) {
        const std::uint32_t size =
            (static_cast<std::uint32_t>(buffer_[consumed]) << 24U) |
            (static_cast<std::uint32_t>(buffer_[consumed + 1]) << 16U) |
            (static_cast<std::uint32_t>(buffer_[consumed + 2]) << 8U) |
            static_cast<std::uint32_t>(buffer_[consumed + 3]);
        if (size == 0 || size > game_ipc_maximum_body_size) {
            reset();
            fail("IPC_FRAME_INVALID", "GameIPC frame length is outside protocol limits");
        }
        if (buffer_.size() - consumed < 4 + size) {
            break;
        }
        const auto* first = reinterpret_cast<const char*>(buffer_.data() + consumed + 4);
        result.emplace_back(first, size);
        consumed += 4 + size;
    }
    if (consumed != 0) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(consumed));
    }
    return result;
}

void GameIpcFrameDecoder::reset() noexcept {
    buffer_.clear();
}

std::size_t GameIpcFrameDecoder::buffered_bytes() const noexcept {
    return buffer_.size();
}

GameIpcSequenceGenerator::GameIpcSequenceGenerator(std::uint64_t first)
    : next_{first} {
    if (first == 0) {
        fail("IPC_SEQUENCE_INVALID", "GameIPC sequence must start above zero");
    }
}

std::uint64_t GameIpcSequenceGenerator::take() {
    if (exhausted_) {
        fail("IPC_SEQUENCE_INVALID", "GameIPC sequence exhausted and cannot wrap");
    }
    const std::uint64_t result = next_;
    if (next_ == std::numeric_limits<std::uint64_t>::max()) {
        exhausted_ = true;
    } else {
        ++next_;
    }
    return result;
}

GameIpcSequenceValidator::GameIpcSequenceValidator(std::uint64_t first)
    : next_{first} {
    if (first == 0) {
        fail("IPC_SEQUENCE_INVALID", "GameIPC sequence must start above zero");
    }
}

void GameIpcSequenceValidator::accept(std::uint64_t sequence) {
    if (exhausted_ || sequence != next_) {
        fail("IPC_SEQUENCE_INVALID", "GameIPC sequence is repeated, missing, or out of order");
    }
    if (next_ == std::numeric_limits<std::uint64_t>::max()) {
        exhausted_ = true;
    } else {
        ++next_;
    }
}

std::uint64_t GameIpcSequenceValidator::expected() const noexcept {
    return next_;
}

}  // namespace cardgame::server::game
