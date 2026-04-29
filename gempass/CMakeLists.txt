cmake_minimum_required(VERSION 3.16)
project(SporkUnrollPass)

find_package(LLVM REQUIRED CONFIG)
list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
include(AddLLVM)

add_definitions(${LLVM_DEFINITIONS})
include_directories(${LLVM_INCLUDE_DIRS})

add_llvm_pass_plugin(SporkUnroll
  SporkUnrollPass.cpp
)