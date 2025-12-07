cmake_minimum_required(VERSION 3.22)
project(fix_allocator_demo VERSION 1.0 LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(fix_allocator_demo src/main.cpp src/run_modes.cpp)
target_include_directories(fix_allocator_demo PRIVATE include)

# Ensure optimization flags are enabled for Release builds.
if (MSVC)
	target_compile_options(fix_allocator_demo PRIVATE
		$<$<CONFIG:Release>:/O2 /DNDEBUG>
	)
else()
	target_compile_options(fix_allocator_demo PRIVATE
		$<$<CONFIG:Release>:-O3 -DNDEBUG>
	)
endif()
