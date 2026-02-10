cmake_minimum_required(VERSION 3.20)
project(COMP4981Assignment1 C)

set(CMAKE_C_STANDARD 11)

add_executable(http_server
        src/main.c
        src/server.c
        src/http.c
        src/path.c
        src/util.c
)

target_include_directories(http_server PRIVATE include)

target_compile_definitions(http_server PRIVATE
        _POSIX_C_SOURCE=200809L
        _XOPEN_SOURCE=700
)
