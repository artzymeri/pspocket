#!/bin/zsh
# Route B, stage 2 (D-147): LLVM, clang and lld as static libraries for iOS arm64, so the app can compile the lifters'
# C on the phone and link it into a library it then signs (runtime/codesign, D-150).
#
#   scripts/build-llvm-ios.sh [host|ios|all]
#
# Source: an llvm-project checkout at $PSPOCKET_LLVM_SRC (default: external/llvm-project inside this repository,
# gitignored, LLVM 22.1.8 at ca7933e47d3a), else a shallow clone of llvmorg-22.1.8 there. Outputs: build/llvm-host (llvm-tblgen and clang-tblgen, which the cross build runs on the Mac) and
# build/llvm-ios (the static libraries, AArch64 only, no tools, no tests). Release, no assertions; zlib, zstd, libxml2
# and terminfo off (nothing to link on iOS). Licence: Apache-2.0 WITH LLVM-exception, compatible with GPLv2 (D-151).
set -eu
cd "$(dirname "$0")/.."
mode=${1:-all}
src=${PSPOCKET_LLVM_SRC:-$PWD/external/llvm-project}
if [[ ! -f $src/llvm/CMakeLists.txt ]]; then
  src=$PWD/external/llvm-project
  if [[ ! -f $src/llvm/CMakeLists.txt ]]; then
    echo "==> cloning llvm-project llvmorg-22.1.8 (shallow) into $src"
    git clone --depth 1 --branch llvmorg-22.1.8 https://github.com/llvm/llvm-project.git $src
  fi
fi
echo "==> LLVM source: $src"
host=$PWD/build/llvm-host ios=$PWD/build/llvm-ios
common=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_ASSERTIONS=OFF -DLLVM_TARGETS_TO_BUILD=AArch64
  -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_TERMINFO=OFF
  -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_DOCS=OFF
  -DCLANG_INCLUDE_TESTS=OFF -DCLANG_INCLUDE_DOCS=OFF -DCLANG_ENABLE_STATIC_ANALYZER=OFF -DCLANG_ENABLE_ARCMT=OFF)

if [[ $mode == host || $mode == all ]]; then
  echo "==> host table generators, and the same libraries for the Mac's tests (runtime/codegen), in $host"
  cmake -S $src/llvm -B $host "${common[@]}" -DLLVM_ENABLE_PROJECTS="clang;lld" > $host.configure.log 2>&1 \
    || { tail -20 $host.configure.log; exit 1; }
  ninja -C $host llvm-tblgen clang-tblgen llvm-min-tblgen
  [[ ${PSPOCKET_LLVM_HOST_LIBS:-1} == 1 ]] && ninja -C $host clangFrontend clangCodeGen clangDriver lldMachO lldCommon \
    LLVMAArch64CodeGen LLVMAArch64AsmParser LLVMAArch64Desc LLVMAArch64Info clang-resource-headers
fi

if [[ $mode == ios || $mode == all ]]; then
  echo "==> LLVM, clang and lld for iOS arm64 in $ios"
  cmake -S $src/llvm -B $ios "${common[@]}" -DLLVM_ENABLE_PROJECTS="clang;lld" \
    -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=17.0 -DLLVM_DEFAULT_TARGET_TRIPLE=arm64-apple-ios17.0 \
    -DLLVM_TABLEGEN=$host/bin/llvm-tblgen -DCLANG_TABLEGEN=$host/bin/clang-tblgen \
    -DLLVM_NATIVE_TOOL_DIR=$host/bin -DLLVM_BUILD_TOOLS=OFF -DCLANG_BUILD_TOOLS=OFF -DLLD_BUILD_TOOLS=OFF \
    -DLLVM_ENABLE_PIC=OFF -DBUILD_SHARED_LIBS=OFF -DLLVM_BUILD_LLVM_DYLIB=OFF -DCMAKE_MACOSX_BUNDLE=OFF \
    -DLLVM_INCLUDE_UTILS=OFF > $ios.configure.log 2>&1 \
    || { tail -30 $ios.configure.log; exit 1; }
  # The libraries an in-process compile and link need (clang's front end and code generation, lld's Mach-O port).
  ninja -C $ios clangFrontend clangCodeGen clangDriver lldMachO lldCommon LLVMAArch64CodeGen LLVMAArch64AsmParser \
    LLVMAArch64Desc LLVMAArch64Info LLVMPasses LLVMLTO clang-resource-headers
  du -sh $ios/lib
fi
