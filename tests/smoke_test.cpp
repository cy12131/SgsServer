#include "cardgame/server/application.h"

#include <iostream>
#include <sstream>
#include <string>

int main() {
    // 使用内存输出流可检查启动信息，同时保持 Application 不依赖测试框架。
    std::ostringstream output;
    cardgame::server::Application application{output};

    if (application.run() != 0) {
        std::cerr << "Application returned a non-zero exit code.\n";
        return 1;
    }

    const std::string text = output.str();
    if (text.find(cardgame::server::Application::name) == std::string::npos) {
        std::cerr << "Application output does not contain its program name.\n";
        return 1;
    }

    if (text.find(cardgame::server::Application::version) == std::string::npos) {
        std::cerr << "Application output does not contain its version.\n";
        return 1;
    }

    return 0;
}
