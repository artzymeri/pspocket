cmake_minimum_required(VERSION 3.24)

project(pspocket
    VERSION 0.1.0
    DESCRIPTION "PS3 to iOS static recompiler"
    LANGUAGES C CXX)

# The toolchain (recomp/, tools/) and the runtime (runtime/) are built here for the host (macOS or Linux).
# The iOS app builds the runtime through app/ios/project.yml instead (see docs/decisions.md D-007).

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Clang only (Apple Clang on macOS/iOS, LLVM Clang on Linux): the FP-contraction pragmas, the warning set and the
# lifted code's extensions are Clang's. GCC is rejected here rather than half-working (D-026).
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "pspocket builds with Clang only (found ${CMAKE_CXX_COMPILER_ID}). "
        "On Linux: CC=clang CXX=clang++ cmake ...")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(PSPOCKET_BUILD_TESTS "Build the test suites" ON)
option(PSPOCKET_WERROR "Treat warnings as errors" OFF)
option(PSPOCKET_SANITIZE "Build with AddressSanitizer and UBSan" OFF)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(PSPocketWarnings)

if(PSPOCKET_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# Every component directory owns its own CMakeLists.txt. Directories that don't exist yet are skipped,
# so components can land independently.
#
# PSPOCKET_ONLY limits the build to the listed component directories (and their tests/<name> suites), e.g.
#   -DPSPOCKET_ONLY="runtime/core;runtime/spu;tests/core;tests/spu"
# so work in progress elsewhere in the tree can't break a focused build. Empty = everything.
set(PSPOCKET_ONLY "" CACHE STRING "Semicolon-separated component directories to build (empty = all)")

function(pspocket_component_enabled dir out)
    if(PSPOCKET_ONLY STREQUAL "" OR "${dir}" IN_LIST PSPOCKET_ONLY OR "${dir}" STREQUAL "third_party" OR "${dir}" STREQUAL "tests")
        set(${out} TRUE PARENT_SCOPE)
    else()
        set(${out} FALSE PARENT_SCOPE)
    endif()
endfunction()

function(pspocket_add_component dir)
    pspocket_component_enabled(${dir} enabled)
    if(enabled AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${dir}/CMakeLists.txt)
        add_subdirectory(${dir})
    endif()
endfunction()

# Third-party
pspocket_add_component(third_party)

# Runtime (host build: used by tests, the macOS dev host and the toolchain's oracles)
pspocket_add_component(runtime/core)
pspocket_add_component(runtime/ppu)
pspocket_add_component(runtime/spu)
pspocket_add_component(runtime/lv2)
pspocket_add_component(runtime/hle)
pspocket_add_component(runtime/rsx)
pspocket_add_component(runtime/disc)
pspocket_add_component(runtime/codesign)
pspocket_add_component(runtime/selftest)
pspocket_add_component(runtime/platform/macos)
pspocket_add_component(runtime/platform/apple)

# Offline toolchain
pspocket_add_component(recomp/common)
pspocket_add_component(recomp/ppu)
pspocket_add_component(recomp/spu)
pspocket_add_component(tools/ps3inspect)

# Process loader and boot host (need both the runtime and recomp/common)
pspocket_add_component(runtime/loader)

if(PSPOCKET_BUILD_TESTS)
    enable_testing()
    pspocket_add_component(tests)
endif()
