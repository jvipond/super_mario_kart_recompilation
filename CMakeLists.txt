cmake_minimum_required(VERSION 3.4.3)
project(super_mario_kart_recompilation)

find_package(LLVM 8.0 REQUIRED)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

set(CMAKE_CXX_STANDARD 17)

set(recompiler_SOURCES Recompiler/main.cpp Recompiler/Recompiler.cpp Recompiler/Recompiler.hpp Recompiler/json.hpp)
set(smk_SOURCES smk_main.cpp gl3w/src/gl3w.c imgui/imgui.h imgui/imgui.cpp imgui/imgui_impl_sdl.h imgui/imgui_impl_sdl.cpp imgui/imgui_impl_opengl3.h imgui/imgui_impl_opengl3.cpp imgui/imgui_internal.h imgui/imgui_widgets.cpp imgui/imgui_draw.cpp imgui/imgui_demo.cpp imgui/imgui_memory_editor.h)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake/sdl2)
find_package(SDL2 REQUIRED)

include_directories(${LLVM_INCLUDE_DIRS} gl3w/include)
add_definitions(${LLVM_DEFINITIONS})

SET(GENERATED_OBJ ${CMAKE_CURRENT_BINARY_DIR}/smk.o)
SET_SOURCE_FILES_PROPERTIES(
  ${GENERATED_OBJ}
  PROPERTIES
  EXTERNAL_OBJECT true
  GENERATED true
)

SET(GENERATED_JSON ${CMAKE_CURRENT_BINARY_DIR}/super_mario_kart_ast.json)
SET_SOURCE_FILES_PROPERTIES(
  ${GENERATED_JSON}
  PROPERTIES
  GENERATED true
)

add_executable(smk ${smk_SOURCES} ${GENERATED_OBJ})
add_executable(recompiler ${recompiler_SOURCES})
									
add_custom_command(OUTPUT smk.ll
									COMMAND recompiler
									DEPENDS ${recompiler_SOURCES} ${GENERATED_JSON}
									WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
									COMMENT "run generated recompiler in ${CMAKE_CURRENT_BINARY_DIR}")		

add_custom_command(OUTPUT smk.o
									COMMAND llc -filetype=obj -relocation-model=pic smk.ll -o smk.o
									DEPENDS smk.ll
									WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
									COMMENT "run llc on smk.ll in ${CMAKE_CURRENT_BINARY_DIR}")										

# Find the libraries that correspond to the LLVM components
# that we wish to use
llvm_map_components_to_libnames(llvm_libs BitWriter Core Support native)

# Link against LLVM libraries
target_link_libraries(recompiler ${llvm_libs})

target_include_directories(smk PRIVATE ${SDL2_INCLUDE_DIRS})
target_link_libraries(smk ${SDL2_LIBRARIES} ${CMAKE_DL_LIBS})