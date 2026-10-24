#pragma once

#include "cardgame/server/protocol/message_router.h"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

namespace cardgame::server::network {

class Session final : public std::enable_shared_from_this<Session> {
public:
    using CloseHandler = std::function<void(const std::shared_ptr<Session>&)>;

    Session(boost::asio::ip::tcp::socket socket, CloseHandler on_close);

    void start();
    void close();

private:
    struct PendingWrite {
        std::vector<std::uint8_t> bytes;
        bool close_after = false;
    };

    void read_header();
    void read_body(std::size_t body_size);
    void handle_body();
    void send(protocol::Message message, bool close_after);
    void write_next();
    void start_handshake_timer();

    boost::asio::ip::tcp::socket socket_;
    boost::asio::steady_timer handshake_timer_;
    CloseHandler on_close_;
    protocol::MessageRouter router_;
    std::array<std::uint8_t, 4> header_{};
    std::vector<std::uint8_t> body_;
    std::deque<PendingWrite> writes_;
    bool ready_ = false;
    bool closed_ = false;
};

}  // namespace cardgame::server::network
