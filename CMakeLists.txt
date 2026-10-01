cmake_minimum_required(VERSION 3.25)

project(PowerFailureManager
  VERSION 1.0.0
  DESCRIPTION "Electrical failure response coordination for data center facilities"
  HOMEPAGE_URL "https://github.com/summonlabs/Power-Failure-Manager"
  LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(PFM_IS_TOP_LEVEL ON)
else()
  set(PFM_IS_TOP_LEVEL OFF)
endif()

option(PFM_BUILD_TESTS "Build the test suite" ${PFM_IS_TOP_LEVEL})
option(PFM_BUILD_TOOLS "Build the administration CLI" ON)
option(PFM_BUILD_EXAMPLES "Build examples" ON)
option(PFM_BUILD_BENCHMARKS "Build the benchmark" ON)
option(PFM_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(PFM_ENABLE_ASAN "Build with AddressSanitizer" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

function(pfm_strict_options target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
    if(PFM_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
    if(PFM_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()

if(PFM_ENABLE_ASAN)
  if(MSVC)
    add_compile_options(/fsanitize=address /Zi)
    add_link_options(/INCREMENTAL:NO)
  else()
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address)
  endif()
endif()

set(PFM_SOURCES
  src/authority.cpp
  src/classification.cpp
  src/clock.cpp
  src/codec.cpp
  src/evidence.cpp
  src/ids.cpp
  src/journal.cpp
  src/obligations.cpp
  src/plan.cpp
  src/policy.cpp
  src/recovery.cpp
  src/refs.cpp
  src/report.cpp
  src/request.cpp
  src/runtime.cpp
  src/scenario.cpp
  src/scope.cpp
  src/serialization.cpp
  src/state.cpp
  src/status.cpp
  src/store.cpp
  src/topology.cpp
  src/transport.cpp
  src/units.cpp
  src/version.cpp
)

add_library(pfm ${PFM_SOURCES})
add_library(Summon::pfm ALIAS pfm)

set_target_properties(pfm PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  POSITION_INDEPENDENT_CODE ON)

target_include_directories(pfm
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(pfm PUBLIC cxx_std_20)

find_package(Threads REQUIRED)
target_link_libraries(pfm PRIVATE Threads::Threads)

pfm_strict_options(pfm)

# ---------------------------------------------------------------------------
# Administration CLI, example, benchmark
# ---------------------------------------------------------------------------

if(PFM_BUILD_TOOLS)
  add_executable(pfmctl tools/pfmctl.cpp)
  target_link_libraries(pfmctl PRIVATE Summon::pfm)
  pfm_strict_options(pfmctl)
endif()

if(PFM_BUILD_EXAMPLES)
  add_executable(pfm_example_electrical_failure examples/electrical_failure.cpp)
  target_link_libraries(pfm_example_electrical_failure PRIVATE Summon::pfm)
  pfm_strict_options(pfm_example_electrical_failure)
endif()

if(PFM_BUILD_BENCHMARKS)
  add_executable(pfm_bench bench/pfm_bench.cpp)
  target_link_libraries(pfm_bench PRIVATE Summon::pfm)
  pfm_strict_options(pfm_bench)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(PFM_BUILD_TESTS)
  enable_testing()

  add_executable(pfm_test_child tests/test_child.cpp)
  target_link_libraries(pfm_test_child PRIVATE Summon::pfm)
  pfm_strict_options(pfm_test_child)

  function(pfm_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE Summon::pfm)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_definitions(${name} PRIVATE
      PFM_TEST_CHILD_PATH="$<TARGET_FILE:pfm_test_child>")
    pfm_strict_options(${name})
    add_test(NAME ${name} COMMAND ${name})
  endfunction()

  pfm_add_test(pfm_unit_tests tests/unit_tests.cpp)
  pfm_add_test(pfm_state_machine_tests tests/state_machine_tests.cpp)
  pfm_add_test(pfm_persistence_tests tests/persistence_tests.cpp)
  pfm_add_test(pfm_process_tests tests/process_tests.cpp)
  pfm_add_test(pfm_scenario_tests tests/scenario_tests.cpp)
  pfm_add_test(pfm_adversarial_tests tests/adversarial_tests.cpp)
endif()

# ---------------------------------------------------------------------------
# Install and export
# ---------------------------------------------------------------------------

install(TARGETS pfm
  EXPORT PowerFailureManagerTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(PFM_BUILD_TOOLS)
  install(TARGETS pfmctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT PowerFailureManagerTargets
  FILE PowerFailureManagerTargets.cmake
  NAMESPACE Summon::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PowerFailureManager)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/PowerFailureManagerConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/PowerFailureManagerConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PowerFailureManager)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/PowerFailureManagerConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)

install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/PowerFailureManagerConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/PowerFailureManagerConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PowerFailureManager)

install(FILES README.md LICENSE NOTICE
  DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/PowerFailureManager)

# ---------------------------------------------------------------------------
# Optional out-of-tree consumer validation against an installed prefix
# ---------------------------------------------------------------------------

if(PFM_BUILD_TESTS AND PFM_CONSUMER_PREFIX)
  add_test(NAME pfm_package_consumer
    COMMAND ${CMAKE_COMMAND}
      -DPFM_CONSUMER_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/consumer
      -DPFM_CONSUMER_PREFIX=${PFM_CONSUMER_PREFIX}
      -DPFM_CONSUMER_BINARY=${CMAKE_CURRENT_BINARY_DIR}/consumer-build
      -DPFM_CONSUMER_CONFIG=$<CONFIG>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunConsumer.cmake)
endif()
