#include "cardgame/server/application.h"

#include <iostream>
#include <string_view>

int main() {
    // 长运行服务不能在单元冒烟测试中直接调用 run()，这里验证稳定的程序标识。
    if (cardgame::server::Application::name != std::string_view{"CardGameServer"}) {
        std::cerr << "Unexpected application name.\n";
        return 1;
    }

    if (cardgame::server::Application::version != std::string_view{"0.1.0-dev"}) {
        std::cerr << "Unexpected application version.\n";
        return 1;
    }

    return 0;
}
