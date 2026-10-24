cmake_minimum_required(VERSION 3.20)

project(CardGameServer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Boost 1.83 CONFIG REQUIRED COMPONENTS json)
find_package(Threads REQUIRED)

function(cardgame_enable_warnings target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    elseif(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    endif()
endfunction()

add_library(cardgame_server_core STATIC
    src/application.cpp
    src/network/session.cpp
    src/network/tcp_server.cpp
    src/protocol/message_codec.cpp
    src/protocol/message_router.cpp
)
target_include_directories(cardgame_server_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
cardgame_enable_warnings(cardgame_server_core)
target_link_libraries(cardgame_server_core PUBLIC
    Boost::json
    Threads::Threads
)

add_executable(cardgame-server
    src/main.cpp
)
target_link_libraries(cardgame-server PRIVATE cardgame_server_core)
cardgame_enable_warnings(cardgame-server)

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)

    add_executable(cardgame-smoke-test
        tests/smoke_test.cpp
    )
    target_link_libraries(cardgame-smoke-test PRIVATE cardgame_server_core)
    cardgame_enable_warnings(cardgame-smoke-test)

    add_test(NAME application_smoke COMMAND cardgame-smoke-test)
    add_test(
        NAME network_integration
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/network_integration_test.py"
                "$<TARGET_FILE:cardgame-server>"
    )
    set_tests_properties(network_integration PROPERTIES TIMEOUT 30)
endif()
