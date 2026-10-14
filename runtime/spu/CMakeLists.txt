# SPU runtime: context, decoder, disassembler, interpreter (oracle + fallback), channels, MFC, run loop, SPU threads,
# and the header lifted SPU code includes (pspocket/spu/lifted.h).
# The iOS app compiles every source in src/ directly (D-014), so nothing here may be host-only tooling or depend on
# configure-time generated headers.
add_library(pspocket_spu STATIC
    src/channels.cpp
    src/decoder.cpp
    src/disasm.cpp
    src/fp.cpp
    src/interpreter.cpp
    src/mfc.cpp
    src/runtime.cpp
    src/thread.cpp
    # Native replacements for known builds of SPU middleware (plan section 6, D-965).
    replace/replace.cpp
    replace/edge_dad4.c
    replace/flashmp3.c)
target_include_directories(pspocket_spu PUBLIC include)
target_link_libraries(pspocket_spu PUBLIC pspocket::core)
pspocket_set_warnings(pspocket_spu)
# Floating-point results must not depend on the compiler: no contraction into fma, no fast-math (fp.h).
target_compile_options(pspocket_spu PRIVATE -ffp-contract=off -fno-fast-math)
find_package(Threads REQUIRED)
target_link_libraries(pspocket_spu PUBLIC Threads::Threads)
add_library(pspocket::spu ALIAS pspocket_spu)
