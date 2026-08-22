cmake_minimum_required(VERSION 3.20)
project(DeskLinkFoundation VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(DESKLINK_BUILD_TESTS "Build DeskLink tests" ON)
option(DESKLINK_BUILD_SIM "Build DeskLink simulation CLI" ON)
option(DESKLINK_BUILD_WINDOWS_BACKENDS "Build Win32 backend adapters" ON)

add_library(desklink_core
    src/protocol.cpp
    src/focus.cpp
    src/host.cpp
    src/session.cpp
    src/agent.cpp
    src/audio.cpp
    src/in_memory_transport.cpp
)

target_include_directories(desklink_core PUBLIC include)

target_compile_definitions(desklink_core PUBLIC DESKLINK_PROTOCOL_VERSION=1)

if(MSVC)
    target_compile_options(desklink_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(desklink_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
endif()

if(WIN32 AND DESKLINK_BUILD_WINDOWS_BACKENDS)
    add_library(desklink_windows
        src/win32_input.cpp
    )
    target_include_directories(desklink_windows PUBLIC include)
    target_link_libraries(desklink_windows PUBLIC desklink_core user32)
    if(MSVC)
        target_compile_options(desklink_windows PRIVATE /W4 /permissive-)
    endif()
endif()

if(DESKLINK_BUILD_SIM)
    add_executable(desklink_sim apps/desklink_sim.cpp)
    target_link_libraries(desklink_sim PRIVATE desklink_core)
endif()

if(DESKLINK_BUILD_TESTS)
    enable_testing()
    add_executable(desklink_tests tests/core_tests.cpp)
    target_link_libraries(desklink_tests PRIVATE desklink_core)
    add_test(NAME desklink_tests COMMAND desklink_tests)
endif()
