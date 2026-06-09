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

# ── examples ──────────────────────────────────────────────────────────────
add_executable(hello examples/hello.c)
target_link_libraries(hello PRIVATE hurra)
