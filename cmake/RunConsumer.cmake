# Power Failure Manager -- out-of-tree consumer driver (script mode).
# Copyright 2026 Summon Software Labs.
# Licensed under the Apache License, Version 2.0 (see LICENSE).
#
# Usage:
#   cmake -DPFM_CONSUMER_SOURCE=<dir> \
#         -DPFM_CONSUMER_PREFIX=<prefix> \
#         -DPFM_CONSUMER_BINARY=<dir> \
#         [-DPFM_CONSUMER_CONFIG=<cfg>] \
#         -P cmake/RunConsumer.cmake
#
# The driver removes and recreates the binary directory, configures the
# consumer against the installed prefix, builds it, locates the produced
# executable in the per-configuration subdirectory or in the binary root, and
# runs it. The first step that does not succeed is reported and the script
# fails. There is no timeout mechanism of any kind, and a partial run is never
# treated as a pass.

foreach(required IN ITEMS PFM_CONSUMER_SOURCE PFM_CONSUMER_PREFIX PFM_CONSUMER_BINARY)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "RunConsumer: ${required} is required")
  endif()
endforeach()

if(NOT IS_DIRECTORY "${PFM_CONSUMER_SOURCE}")
  message(FATAL_ERROR "RunConsumer: consumer source directory not found: ${PFM_CONSUMER_SOURCE}")
endif()
if(NOT EXISTS "${PFM_CONSUMER_SOURCE}/CMakeLists.txt")
  message(FATAL_ERROR "RunConsumer: ${PFM_CONSUMER_SOURCE} does not contain a CMakeLists.txt")
endif()
if(NOT IS_DIRECTORY "${PFM_CONSUMER_PREFIX}")
  message(FATAL_ERROR
    "RunConsumer: installed prefix not found: ${PFM_CONSUMER_PREFIX}\n"
    "Install the library first, for example:\n"
    "  cmake --install <build-dir> --config <cfg> --prefix ${PFM_CONSUMER_PREFIX}")
endif()

get_filename_component(PFM_CONSUMER_SOURCE "${PFM_CONSUMER_SOURCE}" ABSOLUTE)
get_filename_component(PFM_CONSUMER_PREFIX "${PFM_CONSUMER_PREFIX}" ABSOLUTE)
get_filename_component(PFM_CONSUMER_BINARY "${PFM_CONSUMER_BINARY}" ABSOLUTE)

set(consumer_config "")
if(DEFINED PFM_CONSUMER_CONFIG AND NOT "${PFM_CONSUMER_CONFIG}" STREQUAL "")
  set(consumer_config "${PFM_CONSUMER_CONFIG}")
endif()

# A clean binary directory, so no executable left by an earlier run can be
# mistaken for the product of this one.
file(REMOVE_RECURSE "${PFM_CONSUMER_BINARY}")
file(MAKE_DIRECTORY "${PFM_CONSUMER_BINARY}")

# --- configure -------------------------------------------------------------

set(configure_command
  "${CMAKE_COMMAND}"
  -S "${PFM_CONSUMER_SOURCE}"
  -B "${PFM_CONSUMER_BINARY}"
  "-DCMAKE_PREFIX_PATH=${PFM_CONSUMER_PREFIX}")
if(NOT consumer_config STREQUAL "")
  # cmake only accepts --config in build mode; a configure run receives the
  # configuration as the build type (and a multi-config generator ignores it).
  list(APPEND configure_command "-DCMAKE_BUILD_TYPE=${consumer_config}")
endif()

execute_process(
  COMMAND ${configure_command}
  RESULT_VARIABLE configure_result
  OUTPUT_VARIABLE configure_output
  ERROR_VARIABLE configure_error)
if(NOT configure_result EQUAL 0)
  message(FATAL_ERROR
    "RunConsumer: configuring ${PFM_CONSUMER_SOURCE} against ${PFM_CONSUMER_PREFIX} failed "
    "with ${configure_result}\n--- stdout ---\n${configure_output}\n--- stderr ---\n${configure_error}")
endif()
message(STATUS "RunConsumer: configured ${PFM_CONSUMER_SOURCE} against ${PFM_CONSUMER_PREFIX}")

# --- build -----------------------------------------------------------------

set(build_command "${CMAKE_COMMAND}" --build "${PFM_CONSUMER_BINARY}")
if(NOT consumer_config STREQUAL "")
  list(APPEND build_command --config "${consumer_config}")
endif()

execute_process(
  COMMAND ${build_command}
  RESULT_VARIABLE build_result
  OUTPUT_VARIABLE build_output
  ERROR_VARIABLE build_error)
if(NOT build_result EQUAL 0)
  message(FATAL_ERROR
    "RunConsumer: building the consumer in ${PFM_CONSUMER_BINARY} failed with ${build_result}\n"
    "--- stdout ---\n${build_output}\n--- stderr ---\n${build_error}")
endif()

# --- locate the produced executable ----------------------------------------

set(executable "")
foreach(subdirectory IN ITEMS "" "Release/" "Debug/" "RelWithDebInfo/" "MinSizeRel/")
  foreach(name IN ITEMS "pfm_consumer" "pfm_consumer.exe")
    set(candidate "${PFM_CONSUMER_BINARY}/${subdirectory}${name}")
    if(EXISTS "${candidate}" AND NOT IS_DIRECTORY "${candidate}")
      set(executable "${candidate}")
      break()
    endif()
  endforeach()
  if(NOT executable STREQUAL "")
    break()
  endif()
endforeach()

if(executable STREQUAL "")
  file(GLOB_RECURSE leftovers "${PFM_CONSUMER_BINARY}/*pfm_consumer*")
  message(FATAL_ERROR
    "RunConsumer: the build produced no pfm_consumer executable under ${PFM_CONSUMER_BINARY}\n"
    "candidates considered: <binary>/pfm_consumer[.exe] and "
    "<binary>/{Release,Debug,RelWithDebInfo,MinSizeRel}/pfm_consumer[.exe]\n"
    "found instead: ${leftovers}")
endif()
message(STATUS "RunConsumer: built ${executable}")

# --- run -------------------------------------------------------------------

execute_process(
  COMMAND "${executable}"
  RESULT_VARIABLE run_result
  OUTPUT_VARIABLE run_output
  ERROR_VARIABLE run_error)
message(STATUS "RunConsumer: ${executable} output:\n${run_output}")
if(NOT run_result EQUAL 0)
  message(FATAL_ERROR
    "RunConsumer: the consumer exited with ${run_result}\n--- stdout ---\n${run_output}\n"
    "--- stderr ---\n${run_error}")
endif()
if(NOT run_output MATCHES "PFM_CONSUMER ok=1")
  message(FATAL_ERROR
    "RunConsumer: the consumer exited successfully but did not report success\n"
    "--- stdout ---\n${run_output}\n--- stderr ---\n${run_error}")
endif()

message(STATUS "RunConsumer: the out-of-tree consumer succeeded against ${PFM_CONSUMER_PREFIX}")
