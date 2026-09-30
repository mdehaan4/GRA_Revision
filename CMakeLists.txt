cmake_minimum_required(VERSION 3.16)
project(CoralBay C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Download and build raylib automatically (needs git and an internet connection the first time)
include(FetchContent)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  raylib
  GIT_REPOSITORY https://github.com/raysan5/raylib.git
  GIT_TAG 5.5
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(raylib)

add_executable(coralbay src/main.cpp)
target_link_libraries(coralbay PRIVATE raylib)

if(APPLE)
  target_link_libraries(coralbay PRIVATE "-framework IOKit" "-framework Cocoa" "-framework OpenGL")
endif()
