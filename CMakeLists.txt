cmake_minimum_required(VERSION 3.4.3)
project(super_mario_kart_recompilation)

find_package(LLVM 8.0 REQUIRED)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

set(CMAKE_CXX_STANDARD 17)

set(recompiler_SOURCES Recompiler/main.cpp Recompiler/Recompiler.cpp Recompiler/Recompiler.hpp Recompiler/json.hpp)
set(smk_SOURCES smk_main.cpp)

file(GLOB_RECURSE SRC_GL3W
    "${CMAKE_CURRENT_SOURCE_DIR}/gl3w/*.cpp"
		"${CMAKE_CURRENT_SOURCE_DIR}/gl3w/*.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/gl3w/*.h"
		"${CMAKE_CURRENT_SOURCE_DIR}/gl3w/*.hpp"
)

file(GLOB SRC_IMGUI
    "${CMAKE_CURRENT_SOURCE_DIR}/imgui/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/imgui/*.h"
		"${CMAKE_CURRENT_SOURCE_DIR}/imgui/*.hpp"
)

file(GLOB SRC_SPC
    "${CMAKE_CURRENT_SOURCE_DIR}/hardware/spc/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/hardware/spc/*.h"
		"${CMAKE_CURRENT_SOURCE_DIR}/hardware/spc/*.hpp"
)

file(GLOB SRC_DSP
    "${CMAKE_CURRENT_SOURCE_DIR}/hardware/dsp/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/hardware/dsp/*.h"
		"${CMAKE_CURRENT_SOURCE_DIR}/hardware/dsp/*.hpp"
)

file(GLOB SRC_DMA
    "${CMAKE_CURRENT_SOURCE_DIR}/hardware/dma/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/hardware/dma/*.h"
		"${CMAKE_CURRENT_SOURCE_DIR}/hardware/dma/*.hpp"
)

file(GLOB SRC_PPU
    "${CMAKE_CURRENT_SOURCE_DIR}/hardware/ppu/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/hardware/ppu/*.h"
		"${CMAKE_CURRENT_SOURCE_DIR}/hardware/ppu/*.hpp"
)

file(GLOB SRC_HARDWARE
    "${CMAKE_CURRENT_SOURCE_DIR}/hardware/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/hardware/*.h"
		"${CMAKE_CURRENT_SOURCE_DIR}/hardware/*.hpp"
)

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

add_executable(smk ${smk_SOURCES} ${SRC_GL3W} ${SRC_IMGUI} ${SRC_SPC} ${SRC_DSP} ${SRC_DMA} ${SRC_PPU} ${SRC_HARDWARE} ${GENERATED_OBJ})
source_group("gl3w"          	FILES ${SRC_GL3W})
source_group("imgui"        	FILES ${SRC_IMGUI})
source_group("spc"        		FILES ${SRC_SPC})
source_group("dsp"        		FILES ${SRC_DSP})
source_group("dma"        		FILES ${SRC_DMA})
source_group("ppu"        		FILES ${SRC_PPU})
source_group("hardware"       FILES ${SRC_HARDWARE})

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