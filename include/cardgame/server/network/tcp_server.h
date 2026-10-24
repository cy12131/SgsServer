#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/signal_set.hpp>

#include <iosfwd>
#include <memory>
#include <string>
#include <unordered_set>

namespace cardgame::server::network {

class Session;

// TcpServer 拥有 io_context、监听器和活动 Session 集合。M1 使用单线程 run()，
// 因而 accept、Session 回调和停止流程天然串行，不需要额外锁。
class TcpServer final {
public:
    TcpServer(std::string address, unsigned short port, std::ostream& output);
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    void run();
    void stop();

private:
    void start_accept();
    void wait_for_signal();

    std::string address_;
    unsigned short port_;
    std::ostream& output_;
    boost::asio::io_context io_context_;
    boost::asio::ip::tcp::acceptor acceptor_;
    boost::asio::signal_set signals_;
    std::unordered_set<std::shared_ptr<Session>> sessions_;
    bool stopped_ = false;
};

}  // namespace cardgame::server::network
