#pragma once

#include <iosfwd>
#include <string_view>

namespace cardgame::server {

// Application 是服务端进程的生命周期入口，负责读取进程级配置并装配 TcpServer。
// 网络对象均在 run() 的作用域内，退出时按 RAII 顺序释放。
class Application final {
public:
    static constexpr std::string_view name{"CardGameServer"};
    static constexpr std::string_view version{"0.1.0-dev"};

    // 输出流由调用方持有，Application 只在自身生命周期内借用它，不取得所有权。
    explicit Application(std::ostream& output) noexcept;

    // 启动单线程异步事件循环，收到 SIGINT/SIGTERM 后正常返回 0。
    [[nodiscard]] int run();

private:
    std::ostream& output_;
};

}  // namespace cardgame::server
