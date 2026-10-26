#include "cardgame/server/network/session.h"

#include "cardgame/server/protocol/message_codec.h"

#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <chrono>
#include <iostream>
#include <utility>

namespace cardgame::server::network {

Session::Session(
    std::uint64_t session_id,
    boost::asio::ip::tcp::socket socket,
    lobby::LobbyService& lobby_service,
    package::PackageSyncService& package_sync,
    CloseHandler on_close,
    DeliveryHandler deliver)
    : socket_(std::move(socket)),
      handshake_timer_(socket_.get_executor()),
      on_close_(std::move(on_close)),
      deliver_(std::move(deliver)),
      router_(lobby_service, package_sync, session_id) {}

void Session::start() {
    start_handshake_timer();
    read_header();
}

void Session::start_handshake_timer() {
    handshake_timer_.expires_after(std::chrono::seconds{10});
    auto self = shared_from_this();
    handshake_timer_.async_wait([self](const boost::system::error_code& error) {
        if (!error && !self->ready_) {
            std::cerr << "Closing session: Hello timeout\n";
            self->close();
        }
    });
}

void Session::read_header() {
    auto self = shared_from_this();
    boost::asio::async_read(
        socket_, boost::asio::buffer(header_),
        [self](const boost::system::error_code& error, std::size_t) {
            if (error) {
                self->close();
                return;
            }
            const std::uint32_t body_size =
                (static_cast<std::uint32_t>(self->header_[0]) << 24U) |
                (static_cast<std::uint32_t>(self->header_[1]) << 16U) |
                (static_cast<std::uint32_t>(self->header_[2]) << 8U) |
                static_cast<std::uint32_t>(self->header_[3]);
            if (body_size == 0 || body_size > protocol::maximum_body_size) {
                std::cerr << "Closing session: invalid frame length\n";
                self->close();
                return;
            }
            self->read_body(body_size);
        });
}

void Session::read_body(std::size_t body_size) {
    body_.assign(body_size, 0);
    auto self = shared_from_this();
    boost::asio::async_read(
        socket_, boost::asio::buffer(body_),
        [self](const boost::system::error_code& error, std::size_t) {
            if (error) {
                self->close();
                return;
            }
            self->handle_body();
        });
}

void Session::handle_body() {
    const std::string_view body{
        reinterpret_cast<const char*>(body_.data()), body_.size()};
    protocol::DecodeResult decoded = protocol::MessageCodec::decode_body(body);
    if (!decoded.message) {
        std::cerr << "Closing session: " << decoded.error << '\n';
        close();
        return;
    }

    protocol::RouteResult result = router_.route(*decoded.message, ready_);
    if (result.handshake_completed) {
        ready_ = true;
        handshake_timer_.cancel();
    }
    send(std::move(result.response), result.close_after_write);
    if (!result.notifications.empty()) {
        deliver_(std::move(result.notifications));
    }
    if (!result.close_after_write) {
        read_header();
    }
}

void Session::send_notification(protocol::Message message) {
    send(std::move(message), false);
}

bool Session::handshake_completed() const noexcept {
    return ready_;
}

void Session::send(protocol::Message message, bool close_after) {
    try {
        const bool write_in_progress = !writes_.empty();
        writes_.push_back(
            {protocol::MessageCodec::encode_frame(message), close_after});
        if (!write_in_progress) {
            write_next();
        }
    } catch (const std::exception& exception) {
        std::cerr << "Closing session: response encoding failed: "
                  << exception.what() << '\n';
        close();
    }
}

void Session::write_next() {
    if (writes_.empty() || closed_) {
        return;
    }
    auto self = shared_from_this();
    boost::asio::async_write(
        socket_, boost::asio::buffer(writes_.front().bytes),
        [self](const boost::system::error_code& error, std::size_t) {
            if (error) {
                self->close();
                return;
            }
            // close() 会立即清空队列；取消中的 async_write 仍可能以成功完成回调。
            // 此时不能再访问 front()，Session 已经没有待发送数据。
            if (self->closed_ || self->writes_.empty()) {
                return;
            }
            const bool close_after = self->writes_.front().close_after;
            self->writes_.pop_front();
            if (close_after) {
                self->close();
            } else {
                self->write_next();
            }
        });
}

void Session::close() {
    if (closed_) {
        return;
    }
    closed_ = true;
    boost::system::error_code ignored;
    handshake_timer_.cancel();
    socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
    socket_.close(ignored);
    // writes_ 也是进行中 async_write 的缓冲区所有者。这里不能提前清空；
    // 完成回调持有 self，回调返回并释放 Session 时再安全销毁队列。
    if (on_close_) {
        on_close_(shared_from_this());
        on_close_ = {};
    }
}

}  // namespace cardgame::server::network
