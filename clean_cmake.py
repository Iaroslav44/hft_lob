import re
with open('CMakeLists.txt', 'r') as f:
    data = f.read()

# remove Execution Domain Configuration block
data = re.sub(r'# ===.*?Execution Domain Configuration.*?endif\(\)\n', '', data, flags=re.DOTALL)
# remove NUMA_LIB
data = re.sub(r'find_library\(NUMA_LIB numa\)\nif\(NUMA_LIB\).*?endif\(\)\n', '', data, flags=re.DOTALL)

with open('CMakeLists.txt', 'w') as f:
    f.write(data)
