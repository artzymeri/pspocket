# SPU tests: interpreter vectors, decoder/assembler, channels, MFC, atomics litmus, analyzer/lifter, differential
# fuzz and program-level tests.

# ---- unit tests (hand-checked vectors, decoder, disassembler, assembler)
pspocket_add_test(test_spu
    SOURCES test_decoder.cpp test_interp_int.cpp test_interp_float.cpp test_interp_branch_mem.cpp bench_interp.cpp
    LIBS pspocket::recomp_spu)
target_include_directories(test_spu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# ---- differential fuzz: interpreter vs lifted code, every supported instruction (CLAUDE.md rule 3)
set(PSPOCKET_SPU_FUZZ_CASES 256 CACHE STRING "Differential fuzz cases per SPU instruction (at least 200)")
set(PSPOCKET_SPU_FUZZ_TUS 16 CACHE STRING "Translation units the lifted SPU fuzz cases are split into")

add_executable(spu-fuzz-gen fuzz_gen.cpp)
target_include_directories(spu-fuzz-gen PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(spu-fuzz-gen PRIVATE pspocket::recomp_spu)
pspocket_set_warnings(spu-fuzz-gen)

set(fuzz_dir ${CMAKE_CURRENT_BINARY_DIR}/fuzz)
set(fuzz_sources ${fuzz_dir}/fuzz_cases.c ${fuzz_dir}/fuzz_program.c ${fuzz_dir}/ps_spu_fzp_0.c ${fuzz_dir}/ps_spu_fzp_reg.c
    ${fuzz_dir}/ps_spu_fzq_0.c ${fuzz_dir}/ps_spu_fzq_reg.c)
math(EXPR fuzz_last "${PSPOCKET_SPU_FUZZ_TUS} - 1")
foreach(i RANGE ${fuzz_last})
    list(APPEND fuzz_sources ${fuzz_dir}/fuzz_lifted_${i}.c)
endforeach()
add_custom_command(
    OUTPUT ${fuzz_sources}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${fuzz_dir}
    COMMAND spu-fuzz-gen --out ${fuzz_dir} --cases ${PSPOCKET_SPU_FUZZ_CASES} --tus ${PSPOCKET_SPU_FUZZ_TUS}
    DEPENDS spu-fuzz-gen
    COMMENT "Generating the SPU differential fuzz cases"
    VERBATIM)
add_library(spu_fuzz_generated OBJECT ${fuzz_sources})
target_include_directories(spu_fuzz_generated PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(spu_fuzz_generated PRIVATE pspocket::spu)
pspocket_set_warnings(spu_fuzz_generated)
pspocket_set_generated_warnings(spu_fuzz_generated)

pspocket_add_test(test_spu_fuzz SOURCES test_fuzz.cpp $<TARGET_OBJECTS:spu_fuzz_generated> LIBS pspocket::spu)
target_include_directories(test_spu_fuzz PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
