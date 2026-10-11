# These targets are opt-in and never run as part of the default build or CTest.
# Source retrieval does not install tools, execute upstream hooks or load drivers.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_custom_target(fpga_sources
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/scripts/fpga_deps.py"
          fetch --profile core --profile dma
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  USES_TERMINAL VERBATIM)

add_custom_target(fpga_cocotb
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/rtl/cocotb/run.py"
          --build-dir "${PROJECT_BINARY_DIR}/rtl-cocotb"
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  USES_TERMINAL VERBATIM)

add_custom_target(fpga_formal
  COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/rtl/formal/run.py"
          --build-dir "${PROJECT_BINARY_DIR}/rtl-formal"
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  USES_TERMINAL VERBATIM)

# HLS is invoked only by this explicit target. The recipe requires a device part
# and the fixed 6.4 ns network clock in the environment; it never chooses a board.
set(NEXUS_VITIS_HLS_EXECUTABLE "vitis_hls" CACHE STRING "Vitis HLS executable")
add_custom_target(fpga_tcp_ip
  COMMAND "${NEXUS_VITIS_HLS_EXECUTABLE}" -f
          "${PROJECT_SOURCE_DIR}/src/nexus/fpga/integration/prepare_tcp_ip_stack.tcl"
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  USES_TERMINAL VERBATIM)

# An interface target lets the enclosing host application reuse the event ABI.
add_library(nexus_fpga_host INTERFACE)
add_library(nexus::fpga_host ALIAS nexus_fpga_host)
target_include_directories(nexus_fpga_host INTERFACE "${PROJECT_SOURCE_DIR}/src")
target_compile_features(nexus_fpga_host INTERFACE cxx_std_20)
