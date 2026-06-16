cmake_minimum_required(VERSION 3.13)
project(hurra LANGUAGES C VERSION 0.1.0)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)        # Linux serial path uses GNU termios2

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Warnings on, but NOT -Werror — TinyFrame upstream has a few unused-param
# warnings we don't want to gate the build on.
if(MSVC)
    add_compile_options(/W3)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# ── hurra static library ──────────────────────────────────────────────────
set(HURRA_SOURCES
    src/hurra.c
    vendor/TinyFrame/TinyFrame.c
)
if(WIN32)
    list(APPEND HURRA_SOURCES src/serial_win32.c)
else()
    list(APPEND HURRA_SOURCES src/serial_unix.c)
endif()

add_library(hurra STATIC ${HURRA_SOURCES})

target_include_directories(hurra
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/vendor/TinyFrame
)

# POSIX threads.
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(hurra PUBLIC Threads::Threads)
endif()

# ── hurra-bridge binary ───────────────────────────────────────────────────
# Primary product: ferrum-on-PTY <-> hurra-on-serial translation daemon.
set(BRIDGE_SOURCES
    src/bridge.c
    src/selector.c
    src/ferrum_parser.c
    src/input_core.c
    src/frontend_vcom.c
    src/frontend_kmbox.c
    src/kmbox_codec.c
)
if(WIN32)
    list(APPEND BRIDGE_SOURCES src/virtual_port_win32.c)
    list(APPEND BRIDGE_SOURCES src/udp_socket_win32.c)
else()
    list(APPEND BRIDGE_SOURCES src/virtual_port_unix.c)
    list(APPEND BRIDGE_SOURCES src/udp_socket_unix.c)
endif()

add_executable(hurra-bridge ${BRIDGE_SOURCES})
target_link_libraries(hurra-bridge PRIVATE hurra)
target_include_directories(hurra-bridge
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_SOURCE_DIR}/src
)

if(WIN32)
    target_link_libraries(hurra-bridge PRIVATE ws2_32)
endif()

# ── examples ──────────────────────────────────────────────────────────────
add_executable(hello examples/hello.c)
target_link_libraries(hello PRIVATE hurra)

# ── tests ─────────────────────────────────────────────────────────────────
enable_testing()

add_executable(kmbox_codec_test tests/kmbox_codec_test.c src/kmbox_codec.c)
target_include_directories(kmbox_codec_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_test(NAME kmbox_codec COMMAND kmbox_codec_test)

add_executable(input_core_test tests/input_core_test.c)
target_include_directories(input_core_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_test(NAME input_core COMMAND input_core_test)

add_executable(ui_util_test tests/ui_util_test.c)
target_include_directories(ui_util_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_test(NAME ui_util COMMAND ui_util_test)

add_executable(serial_enum_test tests/serial_enum_test.c src/serial_enum.c)
target_include_directories(serial_enum_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_test(NAME serial_enum COMMAND serial_enum_test)

# Ferrum Software-API wire framing: verbatim echo, ">>> " prompt, async
# callback format, NUL-safe button byte, case-insensitive bool (ferrumSpec.md).
add_executable(ferrum_framing_test tests/ferrum_framing_test.c src/ferrum_parser.c)
target_include_directories(ferrum_framing_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_test(NAME ferrum_framing COMMAND ferrum_framing_test)

if(NOT WIN32)
    add_executable(kmbox_loopback_test
        tests/kmbox_loopback_test.c
        src/frontend_kmbox.c
        src/kmbox_codec.c
        src/udp_socket_unix.c)
    target_include_directories(kmbox_loopback_test
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME kmbox_loopback COMMAND kmbox_loopback_test)
endif()
