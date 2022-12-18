cmake_minimum_required(VERSION 3.10.0)

project(threadxx)

if(NOT CMAKE_BUILD_TYPE)
    message(STATUS "compile with release mode")
    set(CMAKE_BUILD_TYPE "Release")
else()
    message(STATUS "compile with debug mode")
    set(CMAKE_BUILD_TYPE "Debug")
endif()

set(CMAKE_CXX_FLAGS_DEBUG "$ENV{CXXFLAGS} -std=c++11 -O0 -fprofile-arcs -ftest-coverage -pg -Wall -g -ggdb")
set(CMAKE_CXX_FLAGS_RELEASE "$ENV{CXXFLAGS} -std=c++11 -O3 -Wall -DNDEBUG")

add_executable (threadxx threadxx.cpp)
target_link_libraries (threadxx pthread)
