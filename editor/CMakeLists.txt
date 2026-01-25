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
  src/annot_persist.c
  src/buffer.c
  src/editor.c
  src/pane.c
  src/layout.c
  src/lcl_host.c
  src/interaction.c
  src/tree_sitter_hl.c
  src/decoration.c
  src/layer.c
  src/layer_stack.c
  src/annot_layer.c
  src/plumb_layer.c
  src/tree_sitter_layer.c
  src/ts_lang_registry.c
  src/patch.c
  src/transform.c
  src/color.c
  src/utf8.c
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(WEFT_COMPILE_OPTIONS -Wall -Wextra)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# === Clang Tools (optional) ===
option(WEFT_ENABLE_CLANG_TIDY "Run clang-tidy during build" OFF)

find_program(CLANG_TIDY_EXE NAMES clang-tidy)
find_program(CLANG_FORMAT_EXE NAMES clang-format)

# === AddressSanitizer (ASan) Support ===
option(WEFT_ENABLE_ASAN "Enable AddressSanitizer for debug builds" OFF)

if(WEFT_ENABLE_ASAN)
  message(STATUS "AddressSanitizer enabled")
  # ASan flags for both compiler and linker
  set(ASAN_FLAGS -fsanitize=address -fno-omit-frame-pointer -g)
  set(ASAN_LINK_FLAGS -fsanitize=address)

  # Add to compile options
  list(APPEND WEFT_COMPILE_OPTIONS ${ASAN_FLAGS})

  # We'll add link flags to the target later
endif()

# === Embedded Tree-Sitter Parser Options ===
# Master switch for all embedded parsers
option(WEFT_EMBED_PARSERS "Embed tree-sitter parsers in binary" ON)

# Individual parser options (default to master switch value)
option(WEFT_EMBED_TS_LCL "Embed tree-sitter-lcl parser" ${WEFT_EMBED_PARSERS})
option(WEFT_EMBED_TS_C "Embed tree-sitter-c parser" ${WEFT_EMBED_PARSERS})
option(WEFT_EMBED_TS_JSON "Embed tree-sitter-json parser" ${WEFT_EMBED_PARSERS})
option(WEFT_EMBED_TS_BASH "Embed tree-sitter-bash parser" ${WEFT_EMBED_PARSERS})
option(WEFT_EMBED_TS_JAVASCRIPT "Embed tree-sitter-javascript parser" ${WEFT_EMBED_PARSERS})

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

FetchContent_Declare(
  SDL3_image
  GIT_REPOSITORY https://github.com/libsdl-org/SDL_image
  GIT_TAG        release-3.4.0
  OVERRIDE_FIND_PACKAGE
)
# Use vendored PNG library so we don't require system libpng
set(SDLIMAGE_VENDORED ON CACHE BOOL "" FORCE)
set(SDLIMAGE_PNG ON CACHE BOOL "" FORCE)
set(SDLIMAGE_PNG_SHARED OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_PNG_LIBPNG OFF CACHE BOOL "" FORCE)
# Disable formats we don't need
set(SDLIMAGE_JPG OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_WEBP OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_AVIF OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_JXL OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_TIF OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL3_image)

# Add Lcl scripting language (from submodule)
set(LCL_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(LCL_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(LCL_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LCL_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LCL_BUILD_REGEX ON CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../submodules/lcl ${CMAKE_CURRENT_BINARY_DIR}/lcl)

# Add tree-sitter runtime library
FetchContent_Declare(
  tree-sitter
  GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter
  GIT_TAG        v0.25.3
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

# === Embedded Tree-Sitter Parsers ===
# Each parser is conditionally compiled based on options

set(WEFT_TS_PARSER_LIBS "")
set(WEFT_TS_PARSER_INCLUDES "")
set(WEFT_TS_PARSER_DEFINES "")

# tree-sitter-lcl (Weft's scripting language)
if(WEFT_EMBED_TS_LCL)
  add_library(tree-sitter-lcl STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../submodules/tree-sitter-lcl/src/parser.c
  )
  target_include_directories(tree-sitter-lcl PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../submodules/tree-sitter-lcl/bindings/c
    ${CMAKE_CURRENT_SOURCE_DIR}/../submodules/tree-sitter-lcl/src
  )
  target_link_libraries(tree-sitter-lcl PUBLIC tree-sitter-lib)
  list(APPEND WEFT_TS_PARSER_LIBS tree-sitter-lcl)
  list(APPEND WEFT_TS_PARSER_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../submodules/tree-sitter-lcl/bindings/c)
  list(APPEND WEFT_TS_PARSER_DEFINES WEFT_EMBED_TS_LCL)
  message(STATUS "Embedding tree-sitter-lcl parser")
endif()

# tree-sitter-c
if(WEFT_EMBED_TS_C)
  add_library(tree-sitter-c STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../submodules/c/src/parser.c
  )
  target_include_directories(tree-sitter-c PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../submodules/c/bindings/c
    ${CMAKE_CURRENT_SOURCE_DIR}/../submodules/c/src
  )
  target_link_libraries(tree-sitter-c PUBLIC tree-sitter-lib)
  list(APPEND WEFT_TS_PARSER_LIBS tree-sitter-c)
  list(APPEND WEFT_TS_PARSER_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../submodules/c/bindings/c)
  list(APPEND WEFT_TS_PARSER_DEFINES WEFT_EMBED_TS_C)
  message(STATUS "Embedding tree-sitter-c parser")
endif()

# tree-sitter-json
if(WEFT_EMBED_TS_JSON)
  add_library(tree-sitter-json STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../submodules/tree-sitter-json/src/parser.c
  )
  target_include_directories(tree-sitter-json PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../submodules/tree-sitter-json/bindings/c
    ${CMAKE_CURRENT_SOURCE_DIR}/../submodules/tree-sitter-json/src
  )
  target_link_libraries(tree-sitter-json PUBLIC tree-sitter-lib)
  list(APPEND WEFT_TS_PARSER_LIBS tree-sitter-json)
  list(APPEND WEFT_TS_PARSER_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../submodules/tree-sitter-json/bindings/c)
  list(APPEND WEFT_TS_PARSER_DEFINES WEFT_EMBED_TS_JSON)
  message(STATUS "Embedding tree-sitter-json parser")
endif()

# tree-sitter-bash (has scanner.c)
if(WEFT_EMBED_TS_BASH)
  add_library(tree-sitter-bash STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../submodules/tree-sitter-bash/src/parser.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../submodules/tree-sitter-bash/src/scanner.c
  )
  target_include_directories(tree-sitter-bash PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../submodules/tree-sitter-bash/bindings/c
    ${CMAKE_CURRENT_SOURCE_DIR}/../submodules/tree-sitter-bash/src
  )
  target_link_libraries(tree-sitter-bash PUBLIC tree-sitter-lib)
  list(APPEND WEFT_TS_PARSER_LIBS tree-sitter-bash)
  list(APPEND WEFT_TS_PARSER_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../submodules/tree-sitter-bash/bindings/c)
  list(APPEND WEFT_TS_PARSER_DEFINES WEFT_EMBED_TS_BASH)
  message(STATUS "Embedding tree-sitter-bash parser")
endif()

# tree-sitter-javascript (has scanner.c)
if(WEFT_EMBED_TS_JAVASCRIPT)
  add_library(tree-sitter-javascript STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../submodules/tree-sitter-javascript/src/parser.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../submodules/tree-sitter-javascript/src/scanner.c
  )
  target_include_directories(tree-sitter-javascript PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../submodules/tree-sitter-javascript/bindings/c
    ${CMAKE_CURRENT_SOURCE_DIR}/../submodules/tree-sitter-javascript/src
  )
  target_link_libraries(tree-sitter-javascript PUBLIC tree-sitter-lib)
  list(APPEND WEFT_TS_PARSER_LIBS tree-sitter-javascript)
  list(APPEND WEFT_TS_PARSER_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/../submodules/tree-sitter-javascript/bindings/c)
  list(APPEND WEFT_TS_PARSER_DEFINES WEFT_EMBED_TS_JAVASCRIPT)
  message(STATUS "Embedding tree-sitter-javascript parser")
endif()

# Generate embedded font header using a CMake script
set(EMBEDDED_FONT_PATH "${CMAKE_CURRENT_SOURCE_DIR}/fonts/Roboto_Mono/static/RobotoMono-Regular.ttf")
set(EMBEDDED_FONT_HEADER "${CMAKE_CURRENT_BINARY_DIR}/embedded_font.h")
set(EMBED_FONT_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_font.cmake")

add_custom_command(
  OUTPUT ${EMBEDDED_FONT_HEADER}
  COMMAND ${CMAKE_COMMAND}
    -DINPUT_FILE=${EMBEDDED_FONT_PATH}
    -DOUTPUT_FILE=${EMBEDDED_FONT_HEADER}
    -P ${EMBED_FONT_SCRIPT}
  DEPENDS ${EMBEDDED_FONT_PATH} ${EMBED_FONT_SCRIPT}
  COMMENT "Generating embedded font header"
)

add_custom_target(embedded_font DEPENDS ${EMBEDDED_FONT_HEADER})

# Generate embedded stdlib header
set(EMBEDDED_STDLIB_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lib/weft.lcl")
set(EMBEDDED_STDLIB_HEADER "${CMAKE_CURRENT_BINARY_DIR}/embedded_stdlib.h")
set(EMBED_STDLIB_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_stdlib.cmake")

add_custom_command(
  OUTPUT ${EMBEDDED_STDLIB_HEADER}
  COMMAND ${CMAKE_COMMAND}
    -DINPUT_FILE=${EMBEDDED_STDLIB_PATH}
    -DOUTPUT_FILE=${EMBEDDED_STDLIB_HEADER}
    -P ${EMBED_STDLIB_SCRIPT}
  DEPENDS ${EMBEDDED_STDLIB_PATH} ${EMBED_STDLIB_SCRIPT}
  COMMENT "Generating embedded stdlib header"
)

add_custom_target(embedded_stdlib DEPENDS ${EMBEDDED_STDLIB_HEADER})

# Generate embedded icon header
set(EMBEDDED_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.png")
set(EMBEDDED_ICON_HEADER "${CMAKE_CURRENT_BINARY_DIR}/embedded_icon.h")
set(EMBED_ICON_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_icon.cmake")

add_custom_command(
  OUTPUT ${EMBEDDED_ICON_HEADER}
  COMMAND ${CMAKE_COMMAND}
    -DINPUT_FILE=${EMBEDDED_ICON_PATH}
    -DOUTPUT_FILE=${EMBEDDED_ICON_HEADER}
    -P ${EMBED_ICON_SCRIPT}
  DEPENDS ${EMBEDDED_ICON_PATH} ${EMBED_ICON_SCRIPT}
  COMMENT "Generating embedded icon header"
)

add_custom_target(embedded_icon DEPENDS ${EMBEDDED_ICON_HEADER})

add_executable(weft ${WEFT_SOURCES})
add_dependencies(weft embedded_font embedded_stdlib embedded_icon)
target_link_libraries(weft PRIVATE
  SDL3::SDL3
  SDL3_ttf::SDL3_ttf
  SDL3_image::SDL3_image
  lcl_static
  lcl_regex
  tree-sitter-lib
  ${WEFT_TS_PARSER_LIBS}
  m
  ${CMAKE_DL_LIBS}
)
target_include_directories(weft PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../submodules/lcl/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../submodules/lcl/packages/lcl-regex/include
  ${tree-sitter_SOURCE_DIR}/lib/include
  ${WEFT_TS_PARSER_INCLUDES}
  ${CMAKE_CURRENT_BINARY_DIR}  # For embedded_font.h
)
target_compile_definitions(weft PRIVATE
  ${WEFT_TS_PARSER_DEFINES}
  WEFT_SUBMODULES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../submodules"
)
target_compile_options(weft PRIVATE ${WEFT_COMPILE_OPTIONS})

# Run clang-tidy during build (only on weft sources, not dependencies)
if(WEFT_ENABLE_CLANG_TIDY AND CLANG_TIDY_EXE)
  set_target_properties(weft PROPERTIES C_CLANG_TIDY "${CLANG_TIDY_EXE}")
  message(STATUS "clang-tidy enabled for weft target")
endif()

# Add ASan linker flags if enabled
if(WEFT_ENABLE_ASAN)
  target_link_options(weft PRIVATE ${ASAN_LINK_FLAGS})
endif()

# === Tests ===
option(WEFT_BUILD_TESTS "Build test executables" ON)

if(WEFT_BUILD_TESTS)
  enable_testing()

  # Test for document module
  add_executable(test_document
    tests/test_document.c
    src/document.c
  )
  target_compile_options(test_document PRIVATE ${WEFT_COMPILE_OPTIONS})
  if(WEFT_ENABLE_ASAN)
    target_link_options(test_document PRIVATE ${ASAN_LINK_FLAGS})
  endif()
  add_test(NAME document COMMAND test_document)

  # Test for annotation store module
  add_executable(test_annot_store
    tests/test_annot_store.c
    src/annot_store.c
  )
  target_compile_options(test_annot_store PRIVATE ${WEFT_COMPILE_OPTIONS})
  if(WEFT_ENABLE_ASAN)
    target_link_options(test_annot_store PRIVATE ${ASAN_LINK_FLAGS})
  endif()
  add_test(NAME annot_store COMMAND test_annot_store)

  # Custom target to run all tests
  add_custom_target(check
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS test_document test_annot_store
    COMMENT "Running all tests"
  )
endif()

# === Clang Tools Targets ===
# Collect all source files for formatting/linting
set(WEFT_ALL_SOURCES "")
foreach(src ${WEFT_SOURCES})
  list(APPEND WEFT_ALL_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/${src}")
endforeach()

# Also include headers
file(GLOB WEFT_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")
list(APPEND WEFT_ALL_SOURCES ${WEFT_HEADERS})

# make format - run clang-format on all sources
if(CLANG_FORMAT_EXE)
  add_custom_target(format
    COMMAND ${CLANG_FORMAT_EXE} -i ${WEFT_ALL_SOURCES}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Running clang-format on weft sources"
    VERBATIM
  )
  message(STATUS "clang-format found: ${CLANG_FORMAT_EXE} (use 'make format')")
endif()

# make tidy - run clang-tidy manually (separate from build)
if(CLANG_TIDY_EXE)
  add_custom_target(tidy
    COMMAND ${CLANG_TIDY_EXE} -p ${CMAKE_BINARY_DIR} ${WEFT_ALL_SOURCES}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Running clang-tidy on weft sources"
    VERBATIM
  )
  message(STATUS "clang-tidy found: ${CLANG_TIDY_EXE} (use 'make tidy')")
endif()
