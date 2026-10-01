# Power Failure Manager -- out-of-tree consumer project.
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0 (see LICENSE).
#
# This project deliberately knows nothing about the Power Failure Manager
# source tree: it consumes the installed package through
# find_package(PowerFailureManager) and links the exported Summon::pfm target.
cmake_minimum_required(VERSION 3.25)

project(PfmDownstreamConsumer
  VERSION 1.0.0
  DESCRIPTION "Downstream consumer of the installed Power Failure Manager package"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# The installed package is the only source of headers, libraries, and the
# exported target.
find_package(PowerFailureManager 1.0 REQUIRED)

add_executable(pfm_consumer main.cpp)
target_link_libraries(pfm_consumer PRIVATE Summon::pfm)
target_compile_features(pfm_consumer PRIVATE cxx_std_20)

if(MSVC)
  target_compile_options(pfm_consumer PRIVATE /W4 /WX /permissive- /utf-8 /Zc:__cplusplus)
else()
  target_compile_options(pfm_consumer PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

message(STATUS "PfmDownstreamConsumer: PowerFailureManager ${PowerFailureManager_VERSION} from ${PowerFailureManager_DIR}")
