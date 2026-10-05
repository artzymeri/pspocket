# runtime/codegen: the C compiler and linker on the device (route B, D-151), from LLVM's libraries. Built when
# PSPOCKET_LLVM_DIR names an LLVM build tree with clang and lld (scripts/build-llvm-ios.sh host: build/llvm-host on the
# Mac; the iOS app links build/llvm-ios's). LLVM, clang and lld are Apache-2.0 WITH LLVM-exception (GPLv2-compatible).
set(PSPOCKET_LLVM_DIR "" CACHE PATH "LLVM build tree with clang and lld (route B's compiler on the device)")
if(NOT PSPOCKET_LLVM_DIR)
    return()
endif()
find_package(LLVM REQUIRED CONFIG PATHS ${PSPOCKET_LLVM_DIR}/lib/cmake/llvm NO_DEFAULT_PATH)
find_package(Clang REQUIRED CONFIG PATHS ${PSPOCKET_LLVM_DIR}/lib/cmake/clang NO_DEFAULT_PATH)
find_package(LLD REQUIRED CONFIG PATHS ${PSPOCKET_LLVM_DIR}/lib/cmake/lld NO_DEFAULT_PATH)

add_library(pspocket_codegen STATIC src/toolchain.cpp src/build.cpp)
target_include_directories(pspocket_codegen PUBLIC include)
target_include_directories(pspocket_codegen SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS} ${LLD_INCLUDE_DIRS})
target_compile_options(pspocket_codegen PRIVATE -fno-rtti) # as LLVM is built
target_link_libraries(pspocket_codegen PRIVATE clangFrontend clangCodeGen clangDriver lldMachO lldCommon
    LLVMAArch64CodeGen LLVMAArch64AsmParser LLVMAArch64Desc LLVMAArch64Info)
# Where the sysroot and clang's own headers are on this machine (the app passes its bundle's instead).
target_compile_definitions(pspocket_codegen PUBLIC
    PSPOCKET_CODEGEN_SYSROOT="${CMAKE_CURRENT_SOURCE_DIR}/sysroot"
    PSPOCKET_CODEGEN_RESOURCE_DIR="${PSPOCKET_LLVM_DIR}/lib/clang/${LLVM_VERSION_MAJOR}")
pspocket_set_warnings(pspocket_codegen)
add_library(pspocket::codegen ALIAS pspocket_codegen)

# Host tool: the device's compiler as a command, to time a whole game's translated code on the Mac.
if(NOT IOS)
    add_executable(pspocket-cc tools/pspocket_cc.cpp)
    target_link_libraries(pspocket-cc PRIVATE pspocket::codegen)
    pspocket_set_warnings(pspocket-cc)
    # And a game library from a game's translated C, as the phone makes it (D-152).
    add_executable(pspocket-prepare tools/pspocket_prepare.cpp)
    target_link_libraries(pspocket-prepare PRIVATE pspocket::codegen pspocket::codesign pspocket::game
        pspocket::recomp_common)
    # CMake repeats static libraries that other static libraries need; Apple's linker warns about the repeats.
    target_link_options(pspocket-prepare PRIVATE -Wl,-no_warn_duplicate_libraries)
    target_compile_definitions(pspocket-prepare PRIVATE PSPOCKET_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
        PSPOCKET_VERSION="${PROJECT_VERSION}")
    pspocket_set_warnings(pspocket-prepare)
endif()
