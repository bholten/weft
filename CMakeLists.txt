cmake_minimum_required(VERSION 3.14)

project(weft
  VERSION 0.1.0
  DESCRIPTION "Weft: a programmable textual workspace on lk/Lcl"
  LANGUAGES C
)

# ---- Package matrix (plan section 3): which Lcl packages this host
# registers.  Core weft never calls packages directly -- only host
# capability bindings do -- so turning these off changes the host, not
# the application.
option(WEFT_LCL_IO "Register the Lcl io package (ANSI C file I/O)" ON)
option(WEFT_LCL_POSIX "Register the Lcl posix package (fs predicates, cwd, dirs)" ON)
option(WEFT_LCL_REGEX "Register the Lcl regex package" ON)
option(WEFT_LCL_PROCESS "Register the Lcl process package" ON)
option(WEFT_LCL_JSON "Register the Lcl json package" ON)
option(WEFT_LCL_TIME "Register the Lcl time package" ON)
option(WEFT_LCL_TS "Register the tree-sitter package (embedded grammars)" ON)

# ---- lk dependency: FetchContent pinned by commit, with a local-path
# override for co-development (plan section 3, decided).
set(WEFT_LK_SOURCE_DIR "" CACHE PATH
  "Local lk checkout to build against (co-development override)")

if(NOT WEFT_LK_SOURCE_DIR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../lk/CMakeLists.txt")
  set(WEFT_LK_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../lk")
  message(STATUS "weft: using sibling lk checkout at ${WEFT_LK_SOURCE_DIR}")
endif()

if(NOT WEFT_LK_SOURCE_DIR)
  # TODO(release cut): add a GIT_REPOSITORY/GIT_TAG pin here once the
  # weft-features branch work lands on lk master.
  message(FATAL_ERROR
    "weft: set -DWEFT_LK_SOURCE_DIR=/path/to/lk (no git pin yet)")
endif()

# Same override for lcl: lk pins lcl from GitHub, but co-development
# needs the local checkout.  FetchContent's first declaration wins, so
# declaring lcl here (before lk configures) supersedes lk's pin.
set(WEFT_LCL_SOURCE_DIR "" CACHE PATH
  "Local lcl checkout to build against (co-development override)")

if(NOT WEFT_LCL_SOURCE_DIR AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../lcl/CMakeLists.txt")
  set(WEFT_LCL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../lcl")
  message(STATUS "weft: using sibling lcl checkout at ${WEFT_LCL_SOURCE_DIR}")
endif()

# lk's own options, forced before it configures (it configures lcl the
# same way).  lk maps LK_LCL_IO/REGEX onto LCL_BUILD_IO/REGEX itself;
# the remaining lcl packages are ours to enable.
set(LK_BUILD_LCL ON CACHE BOOL "" FORCE)
set(LK_LCL_IO ${WEFT_LCL_IO} CACHE BOOL "" FORCE)
set(LK_LCL_REGEX ${WEFT_LCL_REGEX} CACHE BOOL "" FORCE)
set(LCL_BUILD_POSIX ${WEFT_LCL_POSIX} CACHE BOOL "" FORCE)
set(LCL_BUILD_PROCESS ${WEFT_LCL_PROCESS} CACHE BOOL "" FORCE)
set(LCL_BUILD_JSON ${WEFT_LCL_JSON} CACHE BOOL "" FORCE)
set(LCL_BUILD_TIME ${WEFT_LCL_TIME} CACHE BOOL "" FORCE)

include(FetchContent)

if(WEFT_LCL_SOURCE_DIR)
  FetchContent_Declare(lcl SOURCE_DIR "${WEFT_LCL_SOURCE_DIR}")
endif()

FetchContent_Declare(lk SOURCE_DIR "${WEFT_LK_SOURCE_DIR}")
FetchContent_MakeAvailable(lk)

# ---- The weft host binary ----
add_executable(weft host/weft-main.c)

target_link_libraries(weft PRIVATE lcl_lk)
target_compile_options(weft PRIVATE -Wall -Wextra)
set_target_properties(weft PROPERTIES
  C_STANDARD 11
  C_STANDARD_REQUIRED OFF
  C_EXTENSIONS OFF)

target_include_directories(weft PRIVATE
  "${lk_SOURCE_DIR}/include"
  "${lk_SOURCE_DIR}/src"
  "${lk_SOURCE_DIR}/src/lcl"
  "${lk_SOURCE_DIR}/src/sdl")

# Dev-tree default paths; each has a runtime env-var override
# (WEFT_LIB_DIR, WEFT_CONFIG_DIR, WEFT_BOOT) so an installed layout
# works without a rebuild.
# lcl's lib/ of Lcl-implemented libraries (Doc, ...) is a require
# root too, so weft dogfoods them: require "doc/src/Doc.lcl".
# GetProperties recovers lcl_SOURCE_DIR whichever declaration won
# (ours above, or lk's pin).
FetchContent_GetProperties(lcl)
target_compile_definitions(weft PRIVATE
  LK_DSL_PATH="${lk_SOURCE_DIR}/lib/lk-dsl.lcl"
  WEFT_ROOT_DEFAULT="${CMAKE_CURRENT_SOURCE_DIR}"
  WEFT_LIB_DIR_DEFAULT="${CMAKE_CURRENT_SOURCE_DIR}/lib"
  WEFT_LCL_LIB_DIR_DEFAULT="${lcl_SOURCE_DIR}/lib"
  WEFT_CONFIG_DIR_DEFAULT="${CMAKE_CURRENT_SOURCE_DIR}/config"
  WEFT_BOOT_DEFAULT="${CMAKE_CURRENT_SOURCE_DIR}/app/boot.lcl")

if(WEFT_LCL_IO)
  target_compile_definitions(weft PRIVATE WEFT_HAVE_IO)
  target_link_libraries(weft PRIVATE lcl_io)
endif()
if(WEFT_LCL_POSIX)
  target_compile_definitions(weft PRIVATE WEFT_HAVE_POSIX)
  target_link_libraries(weft PRIVATE lcl_posix)
endif()
if(WEFT_LCL_REGEX)
  target_compile_definitions(weft PRIVATE WEFT_HAVE_REGEX)
  target_link_libraries(weft PRIVATE lcl_regex)
endif()
if(WEFT_LCL_PROCESS)
  target_compile_definitions(weft PRIVATE WEFT_HAVE_PROCESS)
  target_link_libraries(weft PRIVATE lcl_process)
endif()
if(WEFT_LCL_JSON)
  target_compile_definitions(weft PRIVATE WEFT_HAVE_JSON)
  target_link_libraries(weft PRIVATE lcl_json)
endif()
if(WEFT_LCL_TIME)
  target_compile_definitions(weft PRIVATE WEFT_HAVE_TIME)
  target_link_libraries(weft PRIVATE lcl_time)
endif()

# ---- tree-sitter (P4): runtime pinned via FetchContent (the legacy
# host's approach, same tag); grammars compiled from the checked-out
# submodules.  Highlight queries are read from the submodule tree at
# session open -- absolute path baked here, so it works from any CWD.
if(WEFT_LCL_TS)
  FetchContent_Declare(
    tree-sitter
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter
    GIT_TAG        v0.25.3
  )
  FetchContent_MakeAvailable(tree-sitter)

  add_library(weft-ts-runtime STATIC ${tree-sitter_SOURCE_DIR}/lib/src/lib.c)
  target_include_directories(weft-ts-runtime PUBLIC
    ${tree-sitter_SOURCE_DIR}/lib/include
    ${tree-sitter_SOURCE_DIR}/lib/src)

  add_library(weft-ts-lcl STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/submodules/tree-sitter-lcl/src/parser.c)
  target_include_directories(weft-ts-lcl PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/submodules/tree-sitter-lcl/src)

  add_library(weft-ts-c STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/submodules/c/src/parser.c)
  target_include_directories(weft-ts-c PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/submodules/c/src)

  target_sources(weft PRIVATE host/weft-ts.c)
  target_compile_definitions(weft PRIVATE
    WEFT_HAVE_TS WEFT_TS_LCL WEFT_TS_C
    WEFT_TS_QUERY_DIR="${CMAKE_CURRENT_SOURCE_DIR}/submodules")
  target_link_libraries(weft PRIVATE weft-ts-lcl weft-ts-c weft-ts-runtime)
endif()
