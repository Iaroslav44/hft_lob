cmake_minimum_required(VERSION 3.20)
project(ZeroAllocationHFT LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT UNIX)
  message(FATAL_ERROR "Build in a POSIX environment; use WSL on Windows")
endif()
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
  message(FATAL_ERROR "GCC or Clang is required")
endif()
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build configuration" FORCE)
endif()

# ==============================================================================
# Low-Latency Optimization & Toolchain Constraint Analysis
# ==============================================================================
# -fno-exceptions / -fno-rtti evaluation:
# Codebase relies on `std::bad_alloc` on hugepage failure and polymorphic
# exception handling in CLI and verification boundaries. Therefore, exceptions
# and RTTI are retained for full contract compliance.
# Aggressive optimizations (-O3, -fno-plt, -march=native) are applied.
option(HFT_NATIVE "Tune for the build host CPU (-march=native)" ON)
option(HFT_LTO "Enable interprocedural optimization when available" ON)
option(HFT_FPGA_TESTS "Enable RTL simulation and FPGA host reference tests" OFF)
option(HFT_FPGA_TOOLING "Expose optional FPGA dependency, cocotb and formal targets" OFF)
set(HFT_QUEUE_CAPACITY 65536 CACHE STRING "Pipeline ring capacity; a power of two, at least two")
if(NOT HFT_QUEUE_CAPACITY MATCHES "^[1-9][0-9]*$")
  message(FATAL_ERROR "HFT_QUEUE_CAPACITY must be a positive integer")
endif()

if(HFT_FPGA_TOOLING)
  include(cmake/FpgaIntegration.cmake)
endif()

find_package(Threads REQUIRED)

# Base Core Interface
add_library(hft_core INTERFACE)
target_include_directories(hft_core INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(hft_core INTERFACE Threads::Threads)
target_compile_options(hft_core INTERFACE
  -Wall -Wextra -Wpedantic -Werror
  $<$<CONFIG:Release>:-O3>
  $<$<CONFIG:Release>:-fno-plt>
  $<$<CONFIG:Release>:-pipe>)
if(HFT_NATIVE)
  target_compile_options(hft_core INTERFACE -march=native)
endif()

if(HFT_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT hft_ipo_supported OUTPUT hft_ipo_error LANGUAGES CXX)
  if(hft_ipo_supported)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
  endif()
endif()

# ==============================================================================
# Isolated Sanitizer Interface Configurations
# ==============================================================================
# ASan and TSan are mutually exclusive runtimes and must never be linked together.
add_library(hft_core_asan INTERFACE)
target_link_libraries(hft_core_asan INTERFACE hft_core)
target_compile_options(hft_core_asan INTERFACE
  -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all -g)
target_link_options(hft_core_asan INTERFACE
  -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)

add_library(hft_core_tsan INTERFACE)
target_link_libraries(hft_core_tsan INTERFACE hft_core)
target_compile_options(hft_core_tsan INTERFACE
  -fsanitize=thread -fno-omit-frame-pointer -g)
target_link_options(hft_core_tsan INTERFACE
  -fsanitize=thread -fno-omit-frame-pointer)

add_library(hft_core_ubsan INTERFACE)
target_link_libraries(hft_core_ubsan INTERFACE hft_core)
target_compile_options(hft_core_ubsan INTERFACE
  -fsanitize=undefined -fno-omit-frame-pointer -fno-sanitize-recover=all -g)
target_link_options(hft_core_ubsan INTERFACE
  -fsanitize=undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)

# ==============================================================================
# Executable Targets
# ==============================================================================
# 1. Standard Targets
add_executable(hft_pipeline src/main.cpp)
target_link_libraries(hft_pipeline PRIVATE hft_core)
target_compile_definitions(hft_pipeline PRIVATE HFT_QUEUE_CAPACITY=${HFT_QUEUE_CAPACITY})

add_executable(hft_tests tests/test_main.cpp)
target_link_libraries(hft_tests PRIVATE hft_core)

add_executable(stress_test tests/stress_test.cpp)
target_link_libraries(stress_test PRIVATE hft_core)

# 2. Isolated ASan Executables
add_executable(hft_tests_asan tests/test_main.cpp)
target_link_libraries(hft_tests_asan PRIVATE hft_core_asan)

add_executable(stress_test_asan tests/stress_test.cpp)
target_link_libraries(stress_test_asan PRIVATE hft_core_asan)

# 3. Isolated TSan Executables
add_executable(hft_tests_tsan tests/test_main.cpp)
target_link_libraries(hft_tests_tsan PRIVATE hft_core_tsan)

add_executable(stress_test_tsan tests/stress_test.cpp)
target_link_libraries(stress_test_tsan PRIVATE hft_core_tsan)

# 4. Isolated UBSan Executables
add_executable(hft_tests_ubsan tests/test_main.cpp)
target_link_libraries(hft_tests_ubsan PRIVATE hft_core_ubsan)

add_executable(stress_test_ubsan tests/stress_test.cpp)
target_link_libraries(stress_test_ubsan PRIVATE hft_core_ubsan)

# Boundary regressions exercise the same source under each existing runtime.
foreach(component IN ITEMS disruptor cxl)
  add_executable(hft_${component}_tests tests/test_${component}.cpp)
  target_link_libraries(hft_${component}_tests PRIVATE hft_core)
  foreach(sanitizer IN ITEMS asan tsan ubsan)
    add_executable(hft_${component}_tests_${sanitizer} tests/test_${component}.cpp)
    target_link_libraries(hft_${component}_tests_${sanitizer} PRIVATE hft_core_${sanitizer})
  endforeach()
endforeach()

if(HFT_FPGA_TESTS)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_executable(hft_fpga_host_tests tests/test_fpga_host.cpp src/nexus/hls/hls4ml_predictor.cpp)
  target_link_libraries(hft_fpga_host_tests PRIVATE hft_core)
  add_executable(hft_fpga_event_tests tests/test_fpga_event.cpp)
  target_link_libraries(hft_fpga_event_tests PRIVATE hft_core)
endif()

# ==============================================================================
# CTest Integration
# ==============================================================================
include(CTest)
if(BUILD_TESTING)
  if(HFT_FPGA_TESTS)
    add_test(NAME hft_fpga_host_tests COMMAND hft_fpga_host_tests)
    add_test(NAME hft_fpga_event_tests COMMAND hft_fpga_event_tests)
    add_test(NAME hft_rtl_tests COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/rtl/run_tests.py"
      --build-dir "${CMAKE_CURRENT_BINARY_DIR}/rtl")
    set_tests_properties(hft_fpga_host_tests hft_fpga_event_tests PROPERTIES TIMEOUT 30)
    set_tests_properties(hft_rtl_tests PROPERTIES TIMEOUT 120)
  endif()
  find_program(SETARCH_EXECUTABLE setarch)
  if(SETARCH_EXECUTABLE)
    # Older ASan and TSan runtimes can collide with WSL address-space layouts
    # before main. Disable ASLR for sanitizer processes only; failures still fail.
    set(SANITIZER_WRAPPER "${SETARCH_EXECUTABLE}" "${CMAKE_SYSTEM_PROCESSOR}" -R)
  else()
    set(SANITIZER_WRAPPER "")
  endif()

  # Standard Unit and Pipeline Smoke Tests
  add_test(NAME hft_tests COMMAND hft_tests)
  add_test(NAME hft_pipeline_smoke COMMAND hft_pipeline -1 -1)
  if(HFT_QUEUE_CAPACITY LESS 32)
    set(hft_test_batch_size "${HFT_QUEUE_CAPACITY}")
  else()
    set(hft_test_batch_size 32)
  endif()
  add_test(NAME hft_pipeline_batch COMMAND hft_pipeline -1 -1 "${hft_test_batch_size}")
  set_tests_properties(hft_tests hft_pipeline_smoke hft_pipeline_batch PROPERTIES TIMEOUT 120)

  # Hostile Multithreaded Concurrency Stress Tests
  add_test(NAME stress_test COMMAND stress_test)
  set_tests_properties(stress_test PROPERTIES TIMEOUT 60)

  # Isolated Sanitizer Execution Targets
  add_test(NAME stress_test_asan COMMAND ${SANITIZER_WRAPPER} "$<TARGET_FILE:stress_test_asan>")
  add_test(NAME stress_test_ubsan COMMAND stress_test_ubsan)
  add_test(NAME stress_test_tsan COMMAND ${SANITIZER_WRAPPER} "$<TARGET_FILE:stress_test_tsan>")
  set_tests_properties(stress_test_asan stress_test_ubsan stress_test_tsan PROPERTIES TIMEOUT 120)

  add_test(NAME hft_tests_asan COMMAND ${SANITIZER_WRAPPER} "$<TARGET_FILE:hft_tests_asan>")
  add_test(NAME hft_tests_ubsan COMMAND hft_tests_ubsan)
  add_test(NAME hft_tests_tsan COMMAND ${SANITIZER_WRAPPER} "$<TARGET_FILE:hft_tests_tsan>")
  set_tests_properties(hft_tests_asan hft_tests_ubsan hft_tests_tsan PROPERTIES TIMEOUT 120)

  foreach(component IN ITEMS disruptor cxl)
    add_test(NAME hft_${component}_tests COMMAND hft_${component}_tests)
    set_tests_properties(hft_${component}_tests PROPERTIES TIMEOUT 120)
    foreach(sanitizer IN ITEMS asan ubsan tsan)
      if(sanitizer STREQUAL "tsan" OR sanitizer STREQUAL "asan")
        add_test(NAME hft_${component}_tests_${sanitizer}
          COMMAND ${SANITIZER_WRAPPER} "$<TARGET_FILE:hft_${component}_tests_${sanitizer}>")
      else()
        add_test(NAME hft_${component}_tests_${sanitizer} COMMAND hft_${component}_tests_${sanitizer})
      endif()
      set_tests_properties(hft_${component}_tests_${sanitizer} PROPERTIES TIMEOUT 120)
    endforeach()
  endforeach()

  # CLI Argument & Error Matrix Tests
  add_test(NAME hft_cli_help COMMAND hft_pipeline --help)
  set_tests_properties(hft_cli_help PROPERTIES TIMEOUT 10)

  function(hft_add_cli_error_test name expected_error)
    add_test(NAME "${name}" COMMAND "${CMAKE_COMMAND}"
      "-DPROGRAM=$<TARGET_FILE:hft_pipeline>" "-DARGUMENTS=${ARGN}"
      "-DEXPECTED_ERROR=${expected_error}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_cli.cmake")
    set_tests_properties("${name}" PROPERTIES TIMEOUT 10)
  endfunction()

  hft_add_cli_error_test(hft_cli_invalid_text "CPU must be" abc)
  hft_add_cli_error_test(hft_cli_trailing_text "CPU must be" 2abc)
  hft_add_cli_error_test(hft_cli_negative_cpu "CPU must be" -2)
  hft_add_cli_error_test(hft_cli_large_cpu "CPU exceeds CPU_SETSIZE|CPU affinity is supported only" 2147483647)
  hft_add_cli_error_test(hft_cli_integer_overflow "CPU must be" 99999999999999999999)
  hft_add_cli_error_test(hft_cli_extra_argument "Expected at most three arguments" -1 -1 1 extra)
  hft_add_cli_error_test(hft_cli_zero_batch "Batch size must be" -1 -1 0)
  hft_add_cli_error_test(hft_cli_invalid_batch "Batch size must be" -1 -1 invalid)
  hft_add_cli_error_test(hft_cli_large_batch "Batch size must be" -1 -1 99999999999999999999)
endif()
