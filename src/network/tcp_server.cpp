#include "cardgame/server/network/tcp_server.h"

#include "cardgame/server/network/session.h"

#include <boost/asio/ip/address.hpp>
#include <boost/system/system_error.hpp>

#include <csignal>
#include <ostream>
#include <utility>
#include <vector>

namespace cardgame::server::network {

TcpServer::TcpServer(std::string address, unsigned short port, std::ostream& output)
    : address_(std::move(address)),
      port_(port),
      output_(output),
      acceptor_(io_context_),
      signals_(io_context_, SIGINT, SIGTERM) {}

TcpServer::~TcpServer() {
    stop();
}

void TcpServer::run() {
    const boost::asio::ip::tcp::endpoint endpoint{
        boost::asio::ip::make_address(address_), port_};
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(boost::asio::socket_base::reuse_address{true});
    acceptor_.bind(endpoint);
    acceptor_.listen();

    output_ << "Listening on " << address_ << ':' << port_ << '\n';
    output_ << "CardGameServer started successfully." << '\n';
    output_.flush();

    start_accept();
    wait_for_signal();
    io_context_.run();
}

void TcpServer::start_accept() {
    acceptor_.async_accept(
        [this](const boost::system::error_code& error,
               boost::asio::ip::tcp::socket socket) {
            if (!error) {
                auto session = std::make_shared<Session>(
                    std::move(socket), [this](const std::shared_ptr<Session>& closed) {
                        sessions_.erase(closed);
                    });
                sessions_.insert(session);
                session->start();
            }
            if (!stopped_ && acceptor_.is_open()) {
                start_accept();
            }
        });
}

void TcpServer::wait_for_signal() {
    signals_.async_wait(
        [this](const boost::system::error_code& error, int) {
            if (!error) {
                output_ << "Stopping CardGameServer." << '\n';
                output_.flush();
                stop();
            }
        });
}

void TcpServer::stop() {
    if (stopped_) {
        return;
    }
    stopped_ = true;
    boost::system::error_code ignored;
    acceptor_.cancel(ignored);
    acceptor_.close(ignored);
    signals_.cancel(ignored);

    const std::vector<std::shared_ptr<Session>> active{
        sessions_.begin(), sessions_.end()};
    for (const auto& session : active) {
        session->close();
    }
    io_context_.stop();
}

}  // namespace cardgame::server::network
