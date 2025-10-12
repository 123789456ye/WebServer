cmake_minimum_required(VERSION 3.20)
project(WebServerWithStdexec)

# Set C++23 standard
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Add stdexec and uring_exec include directories
include_directories(/home/mycomputer/stdexec/include)
include_directories(/home/mycomputer/uring_exec/include)

# Add compiler flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -w -g -O3 -fPIC")
set(CMAKE_CXX_COMPILER "/usr/local/gcc-15.2.0/bin/g++")

# Find required packages
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBURING REQUIRED liburing)

# Find MySQL client library properly
find_package(PkgConfig REQUIRED)
pkg_check_modules(MYSQL REQUIRED mysqlclient)

# If pkg-config doesn't work, try find_library
if(NOT MYSQL_FOUND)
    find_library(MYSQL_LIB NAMES mysqlclient PATHS /usr/lib/x86_64-linux-gnu /usr/lib64 /usr/lib)
    if(MYSQL_LIB)
        set(MYSQL_LIBRARIES ${MYSQL_LIB})
        set(MYSQL_FOUND TRUE)
    endif()
endif()

# Original webserver sources
file(GLOB_RECURSE WEBSERVER_SOURCES
    "code/http/*.cpp"
    "code/buffer/*.cpp"
    "code/pool/*.cpp"
    "code/timer/*.cpp"
    "code/server/*.cpp"
    "code/event/*.cpp"
    "code/log/*.cpp"
)

# Main webserver executable (traditional version for now)
add_executable(webserver
    code/legacy_main.cpp
    ${WEBSERVER_SOURCES}
)

target_link_libraries(webserver pthread ${MYSQL_LIBRARIES})

# Add MySQL include directories if found
if(MYSQL_INCLUDE_DIRS)
    target_include_directories(webserver PRIVATE ${MYSQL_INCLUDE_DIRS})
endif()


# Proactor webserver executable (pure proactor + multi-threaded io_uring)
add_executable(proactor_webserver
    code/coro/proactor_main.cpp
    ${WEBSERVER_SOURCES}
)

target_link_libraries(proactor_webserver
    pthread
    ${MYSQL_LIBRARIES}
    ${LIBURING_LIBRARIES}
)

target_include_directories(proactor_webserver PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/code/coro
    /home/mycomputer/stdexec/include
    /home/mycomputer/uring_exec/include
    ${LIBURING_INCLUDE_DIRS}
)

# Add MySQL include directories for proactor webserver too
if(MYSQL_INCLUDE_DIRS)
    target_include_directories(proactor_webserver PRIVATE ${MYSQL_INCLUDE_DIRS})
endif()

target_compile_options(proactor_webserver PRIVATE
    ${LIBURING_CFLAGS_OTHER}
)