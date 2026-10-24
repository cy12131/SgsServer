#include "cardgame/server/application.h"

#include <ostream>

namespace cardgame::server {

Application::Application(std::ostream& output) noexcept : output_(output) {}

int Application::run() {
    output_ << name << ' ' << version << '\n';
    output_ << name << " started successfully." << '\n';
    return 0;
}

}  // namespace cardgame::server
