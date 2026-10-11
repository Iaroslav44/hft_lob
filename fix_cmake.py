import re
with open('CMakeLists.txt', 'r') as f:
    data = f.read()

data = data.replace(
    'target_compile_options(hft_core INTERFACE -Wall',
    'find_package(Threads REQUIRED)\nadd_library(hft_core INTERFACE)\ntarget_include_directories(hft_core INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")\ntarget_link_libraries(hft_core INTERFACE Threads::Threads)\n\ntarget_compile_options(hft_core INTERFACE -Wall'
)

with open('CMakeLists.txt', 'w') as f:
    f.write(data)
