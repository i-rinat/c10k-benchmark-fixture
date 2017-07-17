project(c10k-http-reverse-proxy-benchmark)
cmake_minimum_required(VERSION 2.8)
set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

include(RagelCompile)

find_package(PkgConfig REQUIRED)
pkg_check_modules(REQ REQUIRED glib-2.0)

set(COMMON_FLAGS "${COMMON_FLAGS} -pthread -pipe -Wall")
set(COMMON_FLAGS "${COMMON_FLAGS} -Wno-unused-const-variable")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu99 ${COMMON_FLAGS}")

ragel_compile(http_parser.rl.c SRC_HTTP_PARSER_RL_C)
create_header_symlink(http_parser.h)
create_header_symlink(macro.h)

include_directories(${REQ_INCLUDE_DIRS})
link_directories(${REQ_LIBRARY_DIRS})

add_executable(server_threads
    server_threads.c
    ${SRC_HTTP_PARSER_RL_C}
)

target_link_libraries(server_threads
    ${REQ_LIBRARIES}
)

add_executable(server_epoll
    server_epoll.c
    ${SRC_HTTP_PARSER_RL_C}
)

target_link_libraries(server_epoll
    ${REQ_LIBRARIES}
)
