#include "cardgame/server/protocol/message_router.h"

#include <boost/json/object.hpp>

#include <string>

namespace cardgame::server::protocol {
namespace {

RouteResult invalid_message(std::int32_t request_id, std::string text) {
    Message response;
    response.type = "Error";
    response.request_id = request_id;
    response.data = {{"code", "INVALID_MESSAGE"}, {"message", std::move(text)}};
    return {std::move(response), false, false};
}

}  // namespace

Message MessageRouter::error(
    std::int32_t request_id, std::string code, std::string text) {
    Message response;
    response.type = "Error";
    response.request_id = request_id;
    response.data = {{"code", std::move(code)}, {"message", std::move(text)}};
    return response;
}

RouteResult MessageRouter::route(const Message& request, bool ready) const {
    if (request.protocol != 1) {
        return {error(request.request_id, "UNSUPPORTED_PROTOCOL",
                      "Unsupported protocol version"),
                false, true};
    }

    if (!ready) {
        if (request.type != "Hello") {
            return {error(request.request_id, "HANDSHAKE_REQUIRED",
                          "Hello handshake is required"),
                    false, false};
        }
        const auto* client_name = request.data.if_contains("client_name");
        const auto* client_version = request.data.if_contains("client_version");
        if (client_name == nullptr || client_version == nullptr ||
            !client_name->is_string() || !client_version->is_string()) {
            return invalid_message(request.request_id, "Invalid Hello data");
        }
        Message response;
        response.type = "HelloAck";
        response.request_id = request.request_id;
        response.data = {{"server_name", "SgsServer"},
                         {"server_version", "0.1.0-dev"}};
        return {std::move(response), true, false};
    }

    if (request.type == "Hello") {
        return invalid_message(request.request_id, "Hello has already completed");
    }
    if (request.type == "Ping") {
        if (!request.data.empty()) {
            return invalid_message(request.request_id, "Ping data must be empty");
        }
        Message response;
        response.type = "Pong";
        response.request_id = request.request_id;
        return {std::move(response), false, false};
    }
    if (request.type == "Echo") {
        const auto* text = request.data.if_contains("text");
        if (text == nullptr || !text->is_string()) {
            return invalid_message(request.request_id, "Echo text must be a string");
        }
        Message response;
        response.type = "EchoResult";
        response.request_id = request.request_id;
        response.data = {{"text", text->as_string()}};
        return {std::move(response), false, false};
    }

    return {error(request.request_id, "UNKNOWN_MESSAGE_TYPE",
                  "Unsupported message type"),
            false, false};
}

}  // namespace cardgame::server::protocol
