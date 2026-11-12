cmake_minimum_required(VERSION 3.16)
project(frame_channel LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(FRAMECHAN_BUILD_TESTS "Build tests" ON)
option(FRAMECHAN_BUILD_EXAMPLES "Build examples" ON)
option(FRAMECHAN_BUILD_BENCH "Build benchmark" ON)
# e.g. -DFRAMECHAN_SANITIZE=thread or address,undefined
set(FRAMECHAN_SANITIZE "" CACHE STRING "Sanitizers passed to -fsanitize=")

find_package(Threads REQUIRED)

add_library(frame_channel src/frame_channel.cpp)
target_include_directories(frame_channel PUBLIC include)
target_link_libraries(frame_channel PUBLIC Threads::Threads)
if(NOT MSVC)
  target_compile_options(frame_channel PRIVATE -Wall -Wextra -Wpedantic -Wconversion)
endif()

if(FRAMECHAN_SANITIZE)
  add_compile_options(-fsanitize=${FRAMECHAN_SANITIZE} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${FRAMECHAN_SANITIZE})
  target_compile_options(frame_channel PRIVATE -fsanitize=${FRAMECHAN_SANITIZE} -fno-omit-frame-pointer -g)
endif()

if(FRAMECHAN_BUILD_TESTS)
  enable_testing()
  add_executable(frame_channel_test tests/frame_channel_test.cpp)
  target_link_libraries(frame_channel_test PRIVATE frame_channel)
  add_test(NAME frame_channel_test COMMAND frame_channel_test)
endif()

if(FRAMECHAN_BUILD_EXAMPLES)
  add_executable(basic_usage examples/basic_usage.cpp)
  target_link_libraries(basic_usage PRIVATE frame_channel)
endif()

if(FRAMECHAN_BUILD_BENCH)
  add_executable(frame_channel_bench bench/frame_channel_bench.cpp)
  target_link_libraries(frame_channel_bench PRIVATE frame_channel)
endif()
