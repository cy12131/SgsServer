#include "cardgame/server/application.h"

#include <iostream>

int main() {
    cardgame::server::Application application{std::cout};
    return application.run();
}
