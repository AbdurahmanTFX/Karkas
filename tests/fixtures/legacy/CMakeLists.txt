cmake_minimum_required(VERSION 3.16)
project(legacy C CXX)
set(CMAKE_CXX_STANDARD 17)
add_executable(game src/game/main.cpp src/game/world.cpp src/game/player.cpp
                    src/util/camera.cpp src/util/math.c)
