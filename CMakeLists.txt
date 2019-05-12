cmake_minimum_required(VERSION 3.4.3)
project(super_mario_kart_recompilation)

find_package(LLVM 8.0 REQUIRED)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

# Set your project compile flags.
# E.g. if using the C++ header files
# you will need to enable C++11 support
# for your compiler.

set(CMAKE_CXX_STANDARD 17)

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

# Now build our tools
add_executable(smk main.cpp Recompiler.cpp Recompiler.hpp json.hpp)

# Find the libraries that correspond to the LLVM components
# that we wish to use
llvm_map_components_to_libnames(llvm_libs BitWriter Core Support native)

# Link against LLVM libraries
target_link_libraries(smk ${llvm_libs})
