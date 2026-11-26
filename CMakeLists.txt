cmake_minimum_required(VERSION 3.20)
project(ckit VERSION 1.0.0 LANGUAGES CXX)

option(CKIT_BUILD_TESTS "Build the correctness tests" ON)
option(CKIT_BUILD_BENCHMARKS "Build the micro-benchmarks" ON)
set(CKIT_SANITIZER "" CACHE STRING
    "Sanitizer to build everything with: thread, address, undefined, or address,undefined")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

add_library(ckit INTERFACE)
add_library(ckit::ckit ALIAS ckit)
target_include_directories(ckit INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(ckit INTERFACE cxx_std_20)

find_package(Threads REQUIRED)
target_link_libraries(ckit INTERFACE Threads::Threads)

# std::atomic<16-byte struct> (the split-reference-count stack and the two
# lock-free queues) needs libatomic on Linux/GCC.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(ckit INTERFACE atomic)
endif()

if(CKIT_SANITIZER)
  add_compile_options(-fsanitize=${CKIT_SANITIZER} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${CKIT_SANITIZER})
endif()

if(MSVC)
  add_compile_options(/W4 /permissive-)
else()
  add_compile_options(-Wall -Wextra)
endif()

include(CTest)
if(CKIT_BUILD_TESTS)
  add_subdirectory(tests)
endif()
if(CKIT_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
