cmake_minimum_required(VERSION 3.22)
project(fix_allocator_demo VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(fix_allocator_demo src/main.cpp src/run_modes.cpp)
target_include_directories(fix_allocator_demo PRIVATE include)
