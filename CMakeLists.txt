cmake_minimum_required(VERSION 3.20)

project(CardGameServer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(cardgame_enable_warnings target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    elseif(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    endif()
endfunction()

add_library(cardgame_server_core STATIC
    src/application.cpp
)
target_include_directories(cardgame_server_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
cardgame_enable_warnings(cardgame_server_core)

add_executable(cardgame-server
    src/main.cpp
)
target_link_libraries(cardgame-server PRIVATE cardgame_server_core)
cardgame_enable_warnings(cardgame-server)

include(CTest)
if(BUILD_TESTING)
    add_executable(cardgame-smoke-test
        tests/smoke_test.cpp
    )
    target_link_libraries(cardgame-smoke-test PRIVATE cardgame_server_core)
    cardgame_enable_warnings(cardgame-smoke-test)

    add_test(NAME application_smoke COMMAND cardgame-smoke-test)
    add_test(NAME server_starts COMMAND cardgame-server)
    set_tests_properties(server_starts PROPERTIES
        PASS_REGULAR_EXPRESSION "CardGameServer 0\\.1\\.0-dev;started successfully"
    )
endif()
