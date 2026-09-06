cmake_minimum_required(VERSION 3.16)
project(HftRiskEngine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O2")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")

# Usage: cmake -DSANITIZER=thread ..   or   -DSANITIZER=address
if(SANITIZER STREQUAL "thread")
    add_compile_options(-fsanitize=thread -g -O1)
    add_link_options(-fsanitize=thread)
elseif(SANITIZER STREQUAL "address")
    add_compile_options(-fsanitize=address,undefined -g -O1)
    add_link_options(-fsanitize=address,undefined)
endif()

find_package(Threads REQUIRED)

add_executable(hft_engine src/main.cpp)
target_include_directories(hft_engine PRIVATE include)
target_link_libraries(hft_engine PRIVATE Threads::Threads)

# Tier 2: real TCP + FIX server, plus a matching test client.
add_executable(hft_server src/server_main.cpp)
target_include_directories(hft_server PRIVATE include)
target_link_libraries(hft_server PRIVATE Threads::Threads)

add_executable(hft_test_client src/test_client.cpp)
target_include_directories(hft_test_client PRIVATE include)
target_link_libraries(hft_test_client PRIVATE Threads::Threads)
