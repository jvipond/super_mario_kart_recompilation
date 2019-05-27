cmake_minimum_required(VERSION 3.4.3)
project(super_mario_kart_recompilation)

find_package(LLVM 8.0 REQUIRED)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

set(CMAKE_CXX_STANDARD 17)

set(recompiler_SOURCES Recompiler/main.cpp Recompiler/Recompiler.cpp Recompiler/Recompiler.hpp Recompiler/json.hpp)
set(smk_SOURCES smk_main.cpp)

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

SET(GENERATED_OBJ ${CMAKE_CURRENT_BINARY_DIR}/smk.o)
SET_SOURCE_FILES_PROPERTIES(
  ${GENERATED_OBJ}
  PROPERTIES
  EXTERNAL_OBJECT true
  GENERATED true
)

add_executable(smk ${smk_SOURCES} ${GENERATED_OBJ})
add_executable(recompiler ${recompiler_SOURCES})
add_custom_target(recompiler_run
									COMMAND recompiler
									WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
									COMMENT "run generated recompiler in ${CMAKE_CURRENT_BINARY_DIR}"
									SOURCES ${recompiler_SOURCES} )
									
add_custom_target(llc_run
									COMMAND llc -filetype=obj smk.ll
									DEPENDS recompiler_run
									WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
									COMMENT "run llc on smk.ll in ${CMAKE_CURRENT_BINARY_DIR}"
									SOURCES ${recompiler_SOURCES} )									

# Find the libraries that correspond to the LLVM components
# that we wish to use
llvm_map_components_to_libnames(llvm_libs BitWriter Core Support native)

# Link against LLVM libraries
target_link_libraries(recompiler ${llvm_libs})

add_dependencies(smk llc_run)
