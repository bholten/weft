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

add_executable(weft ${WEFT_SOURCES})
target_link_libraries(weft PRIVATE SDL3::SDL3 SDL3_ttf::SDL3_ttf lcl_static m)
target_include_directories(weft PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../submodules/lcl/include)
