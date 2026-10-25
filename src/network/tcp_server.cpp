#include "cardgame/server/network/tcp_server.h"

#include "cardgame/server/network/session.h"

#include <boost/asio/ip/address.hpp>
#include <boost/system/system_error.hpp>

#include <algorithm>
#include <csignal>
#include <ostream>
#include <utility>
#include <vector>

namespace cardgame::server::network {

TcpServer::TcpServer(
    std::string address,
    unsigned short port,
    std::filesystem::path package_root,
    std::ostream& output)
    : address_(std::move(address)),
      port_(port),
      output_(output),
      acceptor_(io_context_),
      signals_(io_context_, SIGINT, SIGTERM),
      package_manager_({std::move(package_root)}),
      package_sync_(package_manager_) {
    package_sync_.set_changed_handler([this](const std::string& manifest_id) {
        package_manifest_changed(manifest_id);
    });
}

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
                const std::uint64_t session_id = next_session_id_++;
                auto session = std::make_shared<Session>(
                    session_id,
                    std::move(socket),
                    lobby_service_,
                    package_sync_,
                    [this](const std::shared_ptr<Session>& closed) {
                        session_closed(closed);
                    },
                    [this](std::vector<lobby::Notification> notifications) {
                        deliver(std::move(notifications));
                    });
                sessions_.emplace(session_id, session);
                session->start();
            }
            if (!stopped_ && acceptor_.is_open()) {
                start_accept();
            }
        });
}

void TcpServer::session_closed(const std::shared_ptr<Session>& session) {
    const auto iterator = std::find_if(
        sessions_.begin(), sessions_.end(), [&session](const auto& entry) {
            return entry.second == session;
        });
    if (iterator == sessions_.end()) {
        return;
    }
    const std::uint64_t session_id = iterator->first;
    std::vector<lobby::Notification> notifications = lobby_service_.disconnect(session_id);
    package_sync_.disconnect(session_id);
    sessions_.erase(iterator);
    if (!stopped_) {
        deliver(std::move(notifications));
    }
}

void TcpServer::package_manifest_changed(const std::string& manifest_id) {
    protocol::Message notification;
    notification.type = "PackageManifestChanged";
    notification.request_id = 0;
    notification.data = {{"manifest_id", manifest_id}};
    for (const auto& [session_id, session] : sessions_) {
        static_cast<void>(session_id);
        if (session->handshake_completed()) {
            session->send_notification(notification);
        }
    }
}

package::PackageManager& TcpServer::package_manager() noexcept {
    return package_manager_;
}

void TcpServer::deliver(std::vector<lobby::Notification> notifications) {
    for (lobby::Notification& notification : notifications) {
        const auto target = sessions_.find(notification.session_id);
        if (target != sessions_.end()) {
            target->second->send_notification(std::move(notification.message));
        }
    }
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

    std::vector<std::shared_ptr<Session>> active;
    active.reserve(sessions_.size());
    for (const auto& [session_id, session] : sessions_) {
        static_cast<void>(session_id);
        active.push_back(session);
    }
    for (const auto& session : active) {
        session->close();
    }
    io_context_.stop();
}

}  // namespace cardgame::server::network
