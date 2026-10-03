# SPU tests: interpreter vectors, decoder/assembler, channels, MFC, atomics litmus, analyzer/lifter, differential
# fuzz and program-level tests.

# ---- unit tests (hand-checked vectors, decoder, disassembler, assembler)
pspocket_add_test(test_spu
    SOURCES test_decoder.cpp test_interp_int.cpp test_interp_float.cpp test_interp_branch_mem.cpp
    LIBS pspocket::recomp_spu)
target_include_directories(test_spu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
