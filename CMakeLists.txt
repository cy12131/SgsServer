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
    src/game/game_ipc.cpp
    src/game/game_process_manager.cpp
    src/lobby/lobby_service.cpp
    src/lobby/room_manager.cpp
    src/lobby/user_manager.cpp
    src/network/session.cpp
    src/network/tcp_server.cpp
    src/package/package_manager.cpp
    src/package/package_manifest.cpp
    src/package/package_sync_service.cpp
    src/package/package_utils.cpp
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

add_executable(sgs-package-admin
    src/package_admin.cpp
)
target_link_libraries(sgs-package-admin PRIVATE cardgame_server_core)
cardgame_enable_warnings(sgs-package-admin)

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)

    add_executable(cardgame-smoke-test
        tests/smoke_test.cpp
    )
    target_link_libraries(cardgame-smoke-test PRIVATE cardgame_server_core)
    cardgame_enable_warnings(cardgame-smoke-test)

    add_executable(cardgame-package-test
        tests/package_manager_test.cpp
    )
    target_link_libraries(cardgame-package-test PRIVATE cardgame_server_core)
    cardgame_enable_warnings(cardgame-package-test)

    add_executable(cardgame-game-ipc-test
        tests/game_ipc_test.cpp
    )
    target_link_libraries(cardgame-game-ipc-test PRIVATE cardgame_server_core)
    target_compile_definitions(cardgame-game-ipc-test PRIVATE
        GAME_IPC_VECTOR_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/game-ipc"
    )
    cardgame_enable_warnings(cardgame-game-ipc-test)

    add_executable(cardgame-game-process-stub
        tests/game_process_stub.cpp
    )
    target_link_libraries(cardgame-game-process-stub PRIVATE cardgame_server_core)
    cardgame_enable_warnings(cardgame-game-process-stub)

    add_executable(cardgame-game-process-test
        tests/game_process_manager_test.cpp
    )
    target_link_libraries(cardgame-game-process-test PRIVATE cardgame_server_core)
    cardgame_enable_warnings(cardgame-game-process-test)

    add_test(NAME application_smoke COMMAND cardgame-smoke-test)
    add_test(NAME package_manager COMMAND cardgame-package-test)
    set_tests_properties(package_manager PROPERTIES TIMEOUT 60)
    add_test(NAME game_ipc COMMAND cardgame-game-ipc-test)
    add_test(
        NAME game_process_manager
        COMMAND cardgame-game-process-test $<TARGET_FILE:cardgame-game-process-stub>
    )
    set_tests_properties(game_process_manager PROPERTIES TIMEOUT 45)
    add_test(
        NAME package_admin_concurrency
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/package_admin_concurrency_test.py"
                "$<TARGET_FILE:sgs-package-admin>"
    )
    set_tests_properties(package_admin_concurrency PROPERTIES TIMEOUT 30)
    add_test(
        NAME network_integration
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/network_integration_test.py"
                "$<TARGET_FILE:cardgame-server>"
    )
    set_tests_properties(network_integration PROPERTIES TIMEOUT 30)

    add_test(
        NAME lobby_room_integration
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/lobby_room_integration_test.py"
                "$<TARGET_FILE:cardgame-server>"
    )
    set_tests_properties(lobby_room_integration PROPERTIES TIMEOUT 45)

    add_test(
        NAME package_sync_integration
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/package_sync_integration_test.py"
                "$<TARGET_FILE:cardgame-server>"
    )
    set_tests_properties(package_sync_integration PROPERTIES TIMEOUT 30)
endif()
