cmake_minimum_required(VERSION 3.13)
project(loomora LANGUAGES C CXX)

option(LOOMORA_BUILD_TESTS "Build the backend self-test" ON)

add_subdirectory(core)
