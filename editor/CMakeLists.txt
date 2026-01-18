include(FetchContent)

cmake_minimum_required(VERSION 3.14)

project(weft
  VERSION 0.1.0
  DESCRIPTION "The Weft text edtior"
  LANGUAGES C
)

set(WEFT_SOURCES
  src/weft.c
  src/document.c
  src/platform.c
  src/renderer.c
  src/input.c
  src/annot_store.c
  src/buffer.c
  src/editor.c
  src/pane.c
  src/layout.c
  src/lcl_host.c
  src/interaction.c
  src/tree_sitter_hl.c
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(WEFT_COMPILE_OPTIONS -Wall -Wextra)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

FetchContent_Declare(
  SDL3
  GIT_REPOSITORY https://github.com/libsdl-org/SDL
  GIT_TAG        release-3.4.0
  OVERRIDE_FIND_PACKAGE
)
FetchContent_MakeAvailable(SDL3)

FetchContent_Declare(
  SDL3_ttf
  GIT_REPOSITORY https://github.com/libsdl-org/SDL_ttf
  GIT_TAG        release-3.2.2
  OVERRIDE_FIND_PACKAGE
)
FetchContent_MakeAvailable(SDL3_ttf)

# Add Lcl scripting language (from submodule)
set(LCL_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(LCL_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(LCL_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LCL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../submodules/lcl ${CMAKE_CURRENT_BINARY_DIR}/lcl)

# Add tree-sitter runtime library
FetchContent_Declare(
  tree-sitter
  GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter
  GIT_TAG        v0.24.6
)
FetchContent_MakeAvailable(tree-sitter)

# Build tree-sitter as a static library from the fetched source
add_library(tree-sitter-lib STATIC
  ${tree-sitter_SOURCE_DIR}/lib/src/lib.c
)
target_include_directories(tree-sitter-lib PUBLIC
  ${tree-sitter_SOURCE_DIR}/lib/include
  ${tree-sitter_SOURCE_DIR}/lib/src
)

# Add tree-sitter-lcl parser (from submodule)
add_library(tree-sitter-lcl STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../submodules/tree-sitter-lcl/src/parser.c
)
target_include_directories(tree-sitter-lcl PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../submodules/tree-sitter-lcl/bindings/c
  ${CMAKE_CURRENT_SOURCE_DIR}/../submodules/tree-sitter-lcl/src
)
target_link_libraries(tree-sitter-lcl PUBLIC tree-sitter-lib)

add_executable(weft ${WEFT_SOURCES})
target_link_libraries(weft PRIVATE SDL3::SDL3 SDL3_ttf::SDL3_ttf lcl_static tree-sitter-lib tree-sitter-lcl m)
target_include_directories(weft PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../submodules/lcl/include
  ${tree-sitter_SOURCE_DIR}/lib/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../submodules/tree-sitter-lcl/bindings/c
)
