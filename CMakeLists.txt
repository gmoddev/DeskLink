cmake_minimum_required(VERSION 3.20)
project(DeskLinkFoundation VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(DESKLINK_BUILD_TESTS "Build DeskLink tests" ON)
option(DESKLINK_BUILD_SIM "Build DeskLink simulation CLI" ON)
option(DESKLINK_BUILD_WINDOWS_BACKENDS "Build Win32 backend adapters" ON)
option(DESKLINK_BUILD_MSQUIC "Build the native MsQuic transport adapter" OFF)
set(DESKLINK_MSQUIC_ROOT "" CACHE PATH "Path to a pinned MsQuic 2.5.8 distribution")
set(DESKLINK_MSQUIC_SCHANNEL_ROOT "${DESKLINK_MSQUIC_ROOT}" CACHE PATH
    "Path to the pinned MsQuic 2.5.8 Schannel distribution")

if(DESKLINK_BUILD_TESTS)
    enable_testing()
endif()

add_library(desklink_core
    src/protocol.cpp
    src/focus.cpp
    src/host.cpp
    src/session.cpp
    src/pairing.cpp
    src/pairing_wire.cpp
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

if(WIN32 AND DESKLINK_BUILD_MSQUIC)
    find_path(DESKLINK_MSQUIC_INCLUDE_DIR msquic.h
        HINTS
            "${DESKLINK_MSQUIC_SCHANNEL_ROOT}/include"
            "${DESKLINK_MSQUIC_SCHANNEL_ROOT}/src/inc"
            "${DESKLINK_MSQUIC_SCHANNEL_ROOT}/build/native/include"
        REQUIRED)
    find_file(DESKLINK_MSQUIC_SCHANNEL_DLL NAMES msquic.dll
        HINTS
            "${DESKLINK_MSQUIC_SCHANNEL_ROOT}/bin"
            "${DESKLINK_MSQUIC_SCHANNEL_ROOT}/build/native/bin/x64"
        REQUIRED)
    file(SHA256 "${DESKLINK_MSQUIC_SCHANNEL_DLL}"
         DESKLINK_MSQUIC_SCHANNEL_SHA256)

    if(NOT DESKLINK_BUILD_WINDOWS_BACKENDS)
        message(FATAL_ERROR "DESKLINK_BUILD_MSQUIC requires DESKLINK_BUILD_WINDOWS_BACKENDS")
    endif()

    add_library(desklink_msquic
        src/msquic_runtime.cpp
        src/msquic_transport.cpp
        src/msquic_bootstrap.cpp)
    target_include_directories(desklink_msquic PUBLIC include)
    target_include_directories(desklink_msquic SYSTEM
        PUBLIC "${DESKLINK_MSQUIC_INCLUDE_DIR}")
    target_link_libraries(desklink_msquic
        PUBLIC desklink_core desklink_windows
        PRIVATE crypt32 ncrypt ws2_32)
    target_compile_definitions(desklink_msquic PRIVATE
        DESKLINK_MSQUIC_VERSION="2.5.8"
        DESKLINK_MSQUIC_SCHANNEL_SHA256="${DESKLINK_MSQUIC_SCHANNEL_SHA256}")
    if(MSVC)
        target_compile_options(desklink_msquic PRIVATE /W4 /permissive-)
    else()
        target_compile_options(desklink_msquic PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    endif()

    function(DeskLinkStageSchannelRuntime Target)
        add_custom_command(TARGET ${Target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:${Target}>/runtime/schannel"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${DESKLINK_MSQUIC_SCHANNEL_DLL}"
                "$<TARGET_FILE_DIR:${Target}>/runtime/schannel/msquic.dll")
    endfunction()

    if(DESKLINK_BUILD_TESTS)
        add_executable(desklink_msquic_runtime_tests tests/msquic_runtime_tests.cpp)
        target_link_libraries(desklink_msquic_runtime_tests PRIVATE desklink_msquic)
        if(MINGW)
            target_link_options(desklink_msquic_runtime_tests PRIVATE
                -static -static-libgcc -static-libstdc++)
        endif()
        DeskLinkStageSchannelRuntime(desklink_msquic_runtime_tests)
        add_test(NAME desklink_msquic_runtime_tests
                 COMMAND desklink_msquic_runtime_tests)

        add_executable(desklink_msquic_loopback_tests tests/msquic_loopback_tests.cpp)
        target_link_libraries(desklink_msquic_loopback_tests PRIVATE desklink_msquic)
        if(MINGW)
            target_link_options(desklink_msquic_loopback_tests PRIVATE
                -static -static-libgcc -static-libstdc++)
        endif()
        DeskLinkStageSchannelRuntime(desklink_msquic_loopback_tests)
        add_test(NAME desklink_msquic_loopback_tests
                 COMMAND desklink_msquic_loopback_tests)
    endif()

    add_executable(desklink_pair apps/desklink_pair.cpp)
    target_link_libraries(desklink_pair PRIVATE desklink_msquic shell32 ole32)
    if(MSVC)
        target_compile_options(desklink_pair PRIVATE /W4 /permissive-)
    else()
        target_compile_options(desklink_pair PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    endif()
    if(MINGW)
        target_link_options(desklink_pair PRIVATE
            -municode -static -static-libgcc -static-libstdc++)
    endif()
    DeskLinkStageSchannelRuntime(desklink_pair)
endif()

if(WIN32 AND DESKLINK_BUILD_WINDOWS_BACKENDS)
    add_library(desklink_windows
        src/win32_input.cpp
        src/win32_capture.cpp
        src/win32_pairing.cpp
        src/win32_device_certificate.cpp
    )
    target_include_directories(desklink_windows PUBLIC include)
    target_link_libraries(desklink_windows PUBLIC desklink_core user32 bcrypt crypt32 ncrypt)
    if(MSVC)
        target_compile_options(desklink_windows PRIVATE /W4 /permissive-)
    else()
        target_compile_options(desklink_windows PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    endif()
endif()

if(DESKLINK_BUILD_SIM)
    add_executable(desklink_sim apps/desklink_sim.cpp)
    target_link_libraries(desklink_sim PRIVATE desklink_core)
endif()

if(DESKLINK_BUILD_TESTS)
    add_executable(desklink_tests tests/core_tests.cpp)
    target_link_libraries(desklink_tests PRIVATE desklink_core)
    if(WIN32 AND DESKLINK_BUILD_WINDOWS_BACKENDS)
        target_link_libraries(desklink_tests PRIVATE desklink_windows)
    endif()
    if(MINGW)
        target_link_options(desklink_tests PRIVATE
            -static -static-libgcc -static-libstdc++)
    endif()
    add_test(NAME desklink_tests COMMAND desklink_tests)
endif()
