#pragma once

#include "cardgame/server/lobby/lobby_service.h"
#include "cardgame/server/package/package_manager.h"
#include "cardgame/server/package/package_sync_service.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/signal_set.hpp>

#include <iosfwd>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

namespace cardgame::server::network {

class Session;

// TcpServer 拥有 io_context、监听器和活动 Session 集合。M1 使用单线程 run()，
// 因而 accept、Session 回调和停止流程天然串行，不需要额外锁。
class TcpServer final {
public:
    TcpServer(
        std::string address,
        unsigned short port,
        std::filesystem::path package_root,
        std::ostream& output);
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    void run();
    void stop();
    [[nodiscard]] package::PackageManager& package_manager() noexcept;

private:
    void start_accept();
    void wait_for_signal();
    void session_closed(const std::shared_ptr<Session>& session);
    void deliver(std::vector<lobby::Notification> notifications);
    void package_manifest_changed(const std::string& manifest_id);

    std::string address_;
    unsigned short port_;
    std::ostream& output_;
    boost::asio::io_context io_context_;
    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::signal_set signals_;
    package::PackageManager package_manager_;
    package::PackageSyncService package_sync_;
    lobby::LobbyService lobby_service_;
    std::unordered_map<std::uint64_t, std::shared_ptr<Session>> sessions_;
    std::uint64_t next_session_id_ = 1;
    bool stopped_ = false;
};

}  // namespace cardgame::server::network
