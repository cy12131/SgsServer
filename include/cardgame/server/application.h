#pragma once

#include <iosfwd>
#include <string_view>

namespace cardgame::server {

// Application 是服务端进程的生命周期入口。M0 中它只负责启动信息；后续模块应由它
// 统一装配和关闭，避免 main 函数逐渐承担网络、配置或游戏逻辑。
class Application final {
public:
    static constexpr std::string_view name{"CardGameServer"};
    static constexpr std::string_view version{"0.1.0-dev"};

    // 输出流由调用方持有，Application 只在自身生命周期内借用它，不取得所有权。
    explicit Application(std::ostream& output) noexcept;

    // 返回进程退出码；正常完成返回 0，便于 main 直接转交给操作系统。
    [[nodiscard]] int run();

private:
    std::ostream& output_;
};

}  // namespace cardgame::server
