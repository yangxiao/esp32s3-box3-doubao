cmake_minimum_required(VERSION 3.10)
project(doubao_client C)

set(CMAKE_C_STANDARD 11)

# Find required packages
find_package(PkgConfig REQUIRED)

# libwebsockets
pkg_check_modules(LWS REQUIRED libwebsockets)

# portaudio
pkg_check_modules(PORTAUDIO REQUIRED portaudio-2.0)

# OpenSSL (required by libwebsockets)
find_package(OpenSSL REQUIRED)

# zlib
find_package(ZLIB REQUIRED)

# Source files
set(SOURCES
    main.c
    protocol.c
    client.c
    audio.c
)

add_executable(doubao_client ${SOURCES})

target_include_directories(doubao_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LWS_INCLUDE_DIRS}
    ${PORTAUDIO_INCLUDE_DIRS}
    ${ZLIB_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIR}
)

target_link_directories(doubao_client PRIVATE
    ${LWS_LIBRARY_DIRS}
    ${PORTAUDIO_LIBRARY_DIRS}
)

target_link_libraries(doubao_client
    ${LWS_LIBRARIES}
    ${PORTAUDIO_LIBRARIES}
    ${ZLIB_LIBRARIES}
    pthread
)

# Compiler warnings
target_compile_options(doubao_client PRIVATE -Wall -Wextra -Wno-unused-parameter)
