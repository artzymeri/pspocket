# AArch64 Linux cross build that runs under qemu-user, for x86 Linux hosts (cloud sessions, CI).
# Exercises the AArch64-only paths (lifted VMX, the SPU FP environment) without Apple hardware (D-027).
#
#   apt-get install qemu-user g++-aarch64-linux-gnu      # Ubuntu/Debian: qemu and the AArch64 sysroot
#   CC=clang CXX=clang++ cmake -S . -B build/arm64 -G Ninja -DPSPOCKET_WERROR=ON \
#       -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-aarch64-qemu.cmake
#   ninja -C build/arm64 && ctest --test-dir build/arm64 --output-on-failure

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(PSPOCKET_AARCH64_TRIPLE aarch64-linux-gnu)
set(PSPOCKET_AARCH64_SYSROOT /usr/${PSPOCKET_AARCH64_TRIPLE})

set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET ${PSPOCKET_AARCH64_TRIPLE})
set(CMAKE_CXX_COMPILER_TARGET ${PSPOCKET_AARCH64_TRIPLE})

# Build-time generators (fuzz cases, lifted test programs) and ctest both run through qemu.
find_program(PSPOCKET_QEMU_AARCH64 qemu-aarch64 REQUIRED)
set(CMAKE_CROSSCOMPILING_EMULATOR ${PSPOCKET_QEMU_AARCH64} -L ${PSPOCKET_AARCH64_SYSROOT})

set(CMAKE_FIND_ROOT_PATH ${PSPOCKET_AARCH64_SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
