# PPU runtime: context, decoder, disassembler, interpreter (oracle + fallback), dispatch table, lifted-code ABI.
# The iOS app compiles every source in src/ directly (app/ios/project.yml), so nothing here may depend on
# configure-time generated headers.
add_library(pspocket_ppu STATIC
    src/decoder.cpp
    src/disasm.cpp
    src/interpreter.cpp
    src/interpreter_vmx.cpp
    src/runtime.cpp)
target_include_directories(pspocket_ppu PUBLIC include)
target_link_libraries(pspocket_ppu PUBLIC pspocket::core)
pspocket_set_warnings(pspocket_ppu)
add_library(pspocket::ppu ALIAS pspocket_ppu)
